# HighlightLootableCorpses（原 CorpseESP）代码 Review 待办（2026/08/20）

> 来源：对 `src/` 全量 review。**P0 与部分 P1 已在 2026/08/20 修复并通过 `/W4 /WX` Release 构建**，
> 本文档只保留**尚未处理**的条目，供后续按需取用。
> 行号对应修复后的当前代码（`esp_renderer.cpp` 789 行、`corpse_finder.cpp` 773 行）。

## 已修复（仅作对照，不再跟踪）

| 项 | 位置 | 结论 |
|---|---|---|
| `is_ref_form_in` 误用 `std::ranges::all_of`（灰烬堆/静态尸体检测整体失效，空表时反而全命中） | `corpse_finder.cpp` | 改 `any_of` + 空表护栏；`resolve_form_ids` 解析为空时告警 |
| `flush_ui_quads` 无上限 `memcpy`（越界写 D3D 映射区） | `esp_renderer.cpp` | 提取 `Vertex_Buffer_Bytes`/`Max_Vertices`，按整三角形裁剪 |
| 着色器 blob 泄漏 + 失败后每帧重试重建 | `esp_renderer.cpp` | `compile_ui_shader`/`release_ui_pipeline`/`g_ui_failed` |
| INI 无边界校验（`MinOpacity>1` 触发 `std::clamp` lo>hi UB；`MaxDistance<=0` 退化为全量遍历） | `config.cpp` | 新增 `sanitize()`，唯一校验点 |
| 搜空的容器/尸体仍判为"有货"（`numContainerObjects`、`countDelta<0` 残留条目） | `corpse_finder.cpp`/`loot_filter.cpp` | 判据统一到搜刮界面视角的合并库存（QuickLoot IE 式：引擎初始化 + changes/容器模板/掉落物三段合并）+ `countDelta > 0` |
| `LootFilter::evaluate` 基类容器与运行时条目双重计数 | `loot_filter.cpp` | 同上，改用引擎合并库存 |
| `draw_rect_outline` 线宽 > 半宽时矩形反转、alpha 叠加 | `esp_renderer.cpp` | `t` 按盒子半宽/半高夹取 |
| `ConsoleLog::GetSingleton()` 未判空 | `input.cpp` | 判空后再 `Print` |
| 每 0.5s 无条件刷屏日志（`flush_on(info)` 同步刷盘） | `corpse_finder.cpp` | 逐具去重 + 汇总仅数量变化时输出 |
| `LookupForm(完整FormID, "Skyrim.esm")` 语义误用 | `corpse_finder.cpp` | 改 `TESForm::LookupByID` |
| 解锁后再读 `g_corpses` | `corpse_finder.cpp` | 发布快照前遍历本地 `found` |

---

## P1｜遗留的正确性风险

### 1. ragdoll 刚体 `userData` 只判了 null 就发虚函数调用

- 位置：`src/corpse_finder.cpp:252`
- 现状：`reinterpret_cast<RE::bhkRigidBody*>(rb->userData)` 之后仅在 `rigid_body_aabb` 里判 `!a_body`，
  随即调用 `GetAabbWorldspace`（虚表槽 `0x3B`）。`hkpWorldObject::userData` 是 `std::uint64_t`
  （`hkpWorldObject.h:70` 注释为 `bhkWorldObject*?`），非包装指针的非零值会导致对野指针发虚调用 → CTD。
- 建议：至少校验 `wrapper` 的 `referencedObject`/`GetRigidBody() == rb` 自反一致性；或加 `NiRTTI`/指针区间检查。
- 触发面：仅 ragdoll 路径（`IsInRagdollState()` 为真的尸体），本机未复现，属潜在崩溃。

### 2. 常驻持有后台缓冲引用会让 `ResizeBuffers` 失败

- 位置：`src/esp_renderer.cpp:31`、`49-92`（`ensure_back_buffer`）
- 现状：`g_back_buffer`/`g_back_buffer_rtv` 永久持有 swapchain buffer 0。
  DXGI 要求 `ResizeBuffers` 前释放所有 back buffer 引用，否则返回 `DXGI_ERROR_INVALID_CALL`。
  另外用**指针相等**判 resize 不可靠（新纹理可能复用同一地址 → 拿到尺寸不匹配的 RTV）。
- 影响：SSE 原生改分辨率需重启（影响有限），但 Community Shaders 的动态分辨率/升采样、ENB 会走 resize 路径。
- 建议：每帧 `GetBuffer` → `CreateRenderTargetView` → 用完立即 `Release`（缓存 desc 尺寸即可），
  或额外钩 `IDXGISwapChain::ResizeBuffers`（vtable 槽 13）在其中释放并重建。

---

## P2｜渲染状态与线程/性能

### 3. D3D 状态保存/恢复不对称，且未清管线其余阶段

- 位置：`src/esp_renderer.cpp:516-529`（保存）、`768-787`（恢复）
- 问题：
  - 只保存/恢复 1 个 RTV：`OMGetRenderTargets(1, ...)`，游戏若绑了 MRT，其余 RTV 会被解绑；
  - 恢复了 RTV/Blend/Depth/Raster，但**没有**恢复 IA layout / VB / topology / VS / PS（依赖游戏每次 draw 自行重绑）；
  - 没有把 GS/HS/DS 置空：游戏若留着 geometry shader，我们的 draw 会带着它跑（签名不匹配 → 不出图或设备错误）；
  - 全程未 `RSSetViewports`：NDC 是 CPU 端自算的，依赖"最后绑定的 viewport 恰好是全屏"。
- 建议：统一在绘制段前后做一次完整 state 快照/还原（或最小集：`GSSetShader(nullptr)` + 显式 viewport +
  恢复 IA/VS/PS），并把这段抽成 `ScopedRenderState` RAII 类型。

### 4. Present 钩子内可能抛异常，穿过 C ABI 边界

- 位置：`src/esp_renderer.cpp:303`（`std::make_unique<DirectX::CommonStates>`）、`457`（`on_present`）
- 现状：`CommonStates` 构造在 COM 失败时会抛；`present_thunk`/`on_present` 无 `try/catch`，
  异常会穿过 DXGI 的调用边界（UB）。
- 建议：`on_present` 整体包 `try/catch(...)`，失败一次即置位"禁用绘制"标记并记录日志。

### 5. 扫描回调在引擎自旋锁内做重活

- 位置：`src/corpse_finder.cpp:508`（`log_ash_pile_state`）、`669`（调用点）、`225-259`（ragdoll 路径）
- 背景：`TESObjectCELL::ForEachReference` 是**持 cell `spinLock` 调回调**的
  （`extern/CommonLibSSE/src/RE/T/TESObjectCELL.cpp:16-21`）。当前回调内会：
  `fmt::format` 构造诊断串（去重判断在 format 之后，必然先付出格式化开销）、
  `logger::info` 同步刷盘、ragdoll 路径再取动画图 `updateLock` 自旋锁。
- 风险：卡顿；与引擎线程存在 cell 锁 ↔ updateLock 的锁序反转可能。
- 建议：扫描阶段只收集数据，把日志与格式化移出 `ForEachReferenceInRange` 回调（收集到本地容器后统一输出）。

### 6. `log_state_once` 的去重表无上限

- 位置：`src/corpse_finder.cpp:469-492`
- 现状：`s_seen`/`s_details` 保存一次游戏会话内见过的所有 FormID，且每次 O(n) 线性查两遍。
- 建议：换 `std::unordered_map<RE::FormID, std::string>`，必要时加容量上限/LRU。

### 7. 配置结构体的跨线程无锁读写

- 位置：`src/config.h:38`（`get_mutable()`）+ `src/ui_menu.cpp` 全部控件
- 现状：MCP 回调（游戏主线程）直接写 `Settings`，渲染线程 `Config::get()` 无锁读。
  x86 上对齐 4 字节读写实践无害，但形式上是数据竞争（UB），且 `max_distance` 与
  `fade_start_distance` 存在跨字段不变量（前者 < 后者时衰减曲线退化）。
- 建议：游戏线程写入后原子发布不可变快照（`std::shared_ptr<Settings const>` 或双缓冲 + `atomic` 索引），
  渲染线程整帧持同一份快照。

### 8. 每帧开销可削

- 位置：`src/esp_renderer.cpp:568`（`CorpseFinder::snapshot()` 每帧整表拷贝）、
  `571`（`LootFilter::enabled_category_mask()` 在每具尸体循环内重算）
- 建议：掩码提到循环外；快照改为"版本号 + 共享只读指针"，无变化时不拷贝。

### 9. 钩子安装的可见性顺序

- 位置：`src/esp_renderer.cpp:440-441`
- 现状：游戏线程写 `g_original_present` 与 vtable 槽位，渲染线程读，二者均非原子。
  实践上 x86-TSO + 单次安装不会出问题，但形式上缺 release/acquire 语义。
- 建议：`g_original_present` 改 `std::atomic`，安装顺序为"先 release 存原函数，再写 vtable"。

---

## P3｜规范一致性（对照 `small-project-cpp-rules`）

### 10. 命名遗留

- 全局常量应为 `Upper_Snake_Case`：`corpse_finder.cpp:9` `SkyrimPlugin`/`DawnguardPlugin`/`DragonbornPlugin`、
  `corpse_finder.cpp:110` `kClusterGap`、`esp_renderer.cpp:714` `kMinBox`。
- 局部静态应为 `s_` 前缀：`main.cpp:18` `Level`、`esp_renderer.cpp:744` `kEdges`、`ui_menu.cpp:65` `kBookModes`。
- 现状是 Google 风 `k` 前缀与本仓规范混用（`log_state_once` 的 `s_*` 与 `Layout_Desc` 已符合规范）。

### 11. east const 混用

- `config.cpp:16` `const std::filesystem::path game_root`
- `corpse_finder.cpp:549` `const RE::NiPointer<RE::Actor> actor`
- `corpse_finder.cpp:636`、`691` `const RE::NiAVObject* node`
- 另有 `uint32_t` 与 `std::uint32_t` 混用（`config.h`、`corpse_finder.cpp`、`esp_renderer.cpp`）。

### 12. 头文件不自足 / PCH 越界

- `src/corpse_finder.h`：用 `RE::FormID`、`RE::NiPoint3`、`std::uint16_t`，却只 include `<vector>`
  （靠 `target_precompile_headers` 强制包含 PCH 才能编译）。
- `src/pch.h:43`：预编译头 include 了项目头 `Plugin.h`；规范要求 PCH 只放标准库、平台宏、全局配置宏。
- `cmake/Plugin.h.in`：使用 `REL::Version` 但只 include `<string_view>`，依赖被 PCH 在 REL 之后包含。
- `<algorithm>`/`<limits>`/`<memory>`/`<optional>`/`<string>` 全靠 CommonLibSSE 的 PCH 传递包含。

### 13. 无项目根命名空间

- 现有 6 个顶层命名空间：`Config`、`CorpseFinder`、`ESPRenderer`、`Input`、`UiMenu`、`LootFilter`。
- 规范要求第一方声明包裹在项目根命名空间（如 `CorpseESP`）内。

### 14. `on_present` 是巨型函数，且分层反向

- 位置：`src/esp_renderer.cpp:457-789`（约 330 行，最深嵌套 8 层）
- 一个函数同时承担：热键轮询、扫描调度、相机/矩阵解析、投影、包围盒推导、绘制、D3D 状态管理。
- `project_to_screen`、单具尸体的屏幕矩形推导、线框绘制均满足 code-contract 的拆分条件
  （可独立理解的稳定步骤 / 隔离外部副作用）。
- 另外 `esp_renderer` 反向依赖 `input`（`Input::poll()`）与 `corpse_finder`（调度扫描），
  渲染模块承担了帧驱动职责；可考虑引入独立的 frame tick 入口。

### 15. 死代码 / 无效条目

- `src/esp_renderer.cpp:633-634`、`650-651`：`depth_sum`/`depth_count` 累加后从未使用。
- `src/corpse_finder.h:22`：`bounds_from_collision` 只写不读。
- `src/main.cpp:46`：post-AE 下 SKSE 走 `SKSEPlugin_Version` 路径，**不会调用** `SKSEPlugin_Query`，
  其中 `< 1.6.629` 的版本门形同虚设。
- `src/corpse_finder.cpp` 静态尸体表中的 `0x00023968 DraugrBodyLaying0000` 是 STAT，
  没有容器数据，永远不可能通过可搜刮判定。

### 16. 版本数据声明自相矛盾

- 位置：`src/main.cpp:76-78`
- 现状：同时 `UsesAddressLibrary()` 与 `CompatibleVersions({ SKSE::RUNTIME_SSE_LATEST })`（= 1.6.1170）。
  二者语义互斥——CommonLibSSE 自己的 `PluginDeclaration::RuntimeCompatibility` 变参构造就把
  `_addressLibrary` 置为 false（`extern/CommonLibSSE/include/SKSE/Interfaces.h:541`）。
  用地址库就应删掉 `CompatibleVersions`，否则在非 1.6.1170 的 AE 上是个随 SKSE 实现变化的加载门。
- 另：`main.cpp:75` `AuthorName("CorpseESP")` 填的是插件名而非作者名。

### 17. CMake 严格告警作用域过宽

- 位置：`CMakePresets.json:51`、`CMakeUserPresets.json:20`（`CMAKE_CXX_FLAGS = "/EHsc /MP /W4 /WX"`）
- 现状：全局 flags 把 `/W4 /WX` 灌给了 in-tree 的第三方目标，才被迫在
  `src/CMakeLists.txt:153-155` 给 CommonLibSSE 打 `/wd4100`、在 `ui_menu.cpp:10-13` 给
  SKSE-MCP 堆一排 `#pragma warning(disable: ...)`。
- 建议：改为 `target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)`，第三方目标自然免疫，
  `/wd4100` 与部分 pragma 可一并移除。
- 备注：构建时 7 个 `D9025（/Ob2 被 /Ob3 重写）`命令行告警属预期（`/WX` 不作用于 D 系列）。

### 18. 文档与实现漂移

- `src/loot_filter.cpp`/`src/loot_filter.h` 是 LF 行尾，仓库其余源文件为 CRLF（既有差异，未动）。

---

## 建议的后续批次

| 批次 | 内容 | 需要的验证 |
|---|---|---|
| A | P1 第 1、2 条（ragdoll `userData` 校验、back buffer 引用与 resize） | 进游戏验证：ragdoll 尸体框显示正常、切分辨率/窗口不崩 |
| B | P2 第 3、4 条（`ScopedRenderState` + 异常隔离 + viewport/GS） | 进游戏验证：与 ENB/Community Shaders 共存不闪、不丢画面 |
| C | P2 第 5、6、7、8 条（扫描锁内开销、去重表、配置快照、每帧拷贝） | 大战场景/密集尸体下的帧时与日志体积 |
| D | P3 全部（命名、east const、根命名空间、`on_present` 拆分、CMake、文档） | 仅需 `/W4 /WX` 构建通过 + 行为回归 |
