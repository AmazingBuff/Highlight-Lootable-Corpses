# CorpseESP 模型剪影描边（Model Silhouette）方案与实现记录

> 日期：2026/08/20
> 状态：设计 → 实现
> 目标：把描边从"包围盒线框"扩展为"尸体模型剪影"，两种模式可由 INI / 游戏内菜单切换

> **修复记录（2026/08/22，2026/08/24 更正）**：初版直接把 `NiCamera::worldToCam` 当裁剪矩阵
> 传入遮罩趟，剪影什么都不显示；当时误判 `worldToCam` 是仿射**视图**矩阵，改而在 CPU 端用
> 视锥参数再组合一次 `M = P·V`（`MeshOutline::compose_world_to_clip`）并加了一次性
> `[silhouette-probe]` 数值自检。2026/08/24 的探针对表证明该诊断本身错了——`worldToCam` 是
> 完整的世界→裁剪矩阵，二次组合反而让描边在屏幕覆盖尺度上炸裂（见文末修复记录）。
> `compose_world_to_clip` 已删除，现整份透传 `worldToCam`；下文相关表述已同步更正。

## 一、需求与约束

| 项 | 结论 |
|---|---|
| 功能 | `OutlineMode`：0 = 包围盒（现状），1 = 模型剪影 |
| 必须保留 | 无视遮挡（穿墙可见）、按距离淡出、颜色/线宽/战利品筛选全部沿用现有配置 |
| 切换方式 | INI `[Display] OutlineMode` + MCP 面板下拉框，运行时切换 |
| 不可行方案 | 用游戏深度缓冲做屏幕空间轮廓：被遮挡处深度属于遮挡物，且尸体包围盒内含地面 → 与"穿墙可见"冲突，否决 |
| 唯一可行路径 | **自行重绘尸体几何到遮罩 RT**（不参与深度测试）→ 边缘检测 → 混合到后台缓冲 |

## 二、为什么可以重绘引擎的几何

SSE 的网格数据在运行时就是 GPU 就绪布局，CommonLibSSE 已暴露全部必要字段：

| 需要的东西 | 来源 |
|---|---|
| 顶点/索引缓冲 | `BSGeometry::GetGeometryRuntimeData().rendererData` → `BSGraphics::TriShape{ vertexBuffer, indexBuffer, vertexDesc }` |
| 顶点布局 | `BSGraphics::VertexDesc::HasFlag() / GetAttributeOffset()`（`RE/V/VertexDesc.h`） |
| 三角形数 | `BSTriShape::GetTrishapeRuntimeData().triangleCount` |
| 蒙皮分区 | `NiSkinInstance::skinPartition` → `NiSkinPartition::Partition{ bones, numBones, triangles, vertexDesc, buffData }` |
| 骨骼世界变换 | `NiSkinInstance::bones[i]->world` |
| 绑定姿态逆变换 | `NiSkinData::GetBoneDataSkinToBone(i)` |
| 裁剪矩阵 | `NiCamera::GetRuntimeData().worldToCam` 本身就是引擎的世界→裁剪 **view-projection** 矩阵（行主序，`clip = M·p`；w 行 = 前向距离，相机背后被 `w <= 0` 裁掉；反 Z 的 z 行因着色器写死 `z = w*0.5` 无关），遮罩趟直接整份字节拷贝使用；引擎 `WorldPtToScreenPt3` 内部是同一份数学（`NDC = (row0·p, row1·p)/(row3·p)`），box 投影与 probe 与之对表 |

**不需要 CPU 回读顶点数据**：直接把引擎的 `ID3D11Buffer` 绑到我们自己的 IA 上，配自己的 InputLayout 与着色器即可。

## 三、渲染管线（三趟）

```
                              ┌─ 每个网格部件（DrawIndexed，无深度、CullNone、Blend=MAX）
[1] 遮罩趟  RT = mask(R8) ────┤   PS 输出该尸体的 fade alpha（0..1）
                              └─ 重叠尸体取 max，alpha 直接编码在遮罩值里
[2] 膨胀趟  RT = dilate(R8)  ← 横向 max 滤波，半径 = OutlineThickness 像素
[3] 描边趟  RT = 后台缓冲     ← 纵向 max 滤波 + 边缘判定：
                                center(mask) 为空 且 dilated 非空 → 输出预乘颜色 (rgb*a, a)
```

- **alpha 编码进遮罩**：一趟全屏 pass 即可保留每具尸体各自的距离淡出，无需按尸体分批。
- **可分离膨胀**：横向 + 纵向各 `2r+1` 次采样（r ≤ 8），比 8 方向星形采样更快且无缺口。
- **预乘输出**：OM 用 `CommonStates::AlphaBlend()`（ONE / INV_SRC_ALPHA），与现有 quad 管线的约定一致（见 `docs/esp-renderer-alpha-blend.md`）。
- **穿墙**：三趟全程不绑 DSV、`DepthNone`，与 box 模式同源。
- 深度写死为 `z = w * 0.5`：避开近/远裁剪面，同时 `w`（前向距离）`<= 0` 仍能正确裁掉相机背后的几何。

## 四、着色器

| 着色器 | 输入 | 说明 |
|---|---|---|
| `mask_rigid_vs` | POSITION | `world_pos = M_world · p`，`clip = world_to_clip · world_pos` |
| `mask_skinned_vs` | POSITION / BLENDWEIGHT / BLENDINDICES | `p' = Σ w_i · (palette[idx_i] · p)` 后同上 |
| `mask_ps` | — | `return alpha`（写 R8 遮罩） |
| `fullscreen_vs` | `SV_VertexID` | 3 顶点覆盖全屏，无顶点缓冲、无 InputLayout |
| `dilate_ps` | — | 横向 max 滤波 |
| `outline_ps` | — | 纵向 max 滤波 + 边缘判定 + 预乘输出 |

常量缓冲（b0，遮罩趟）：`row_major float4x4`（HLSL 内沿用 `g_world_to_cam` 命名，
内容是 `NiCamera::worldToCam` 的**整份字节拷贝**——引擎的世界→裁剪 view-projection 矩阵，
不再二次组合投影）+ `float4 params(alpha)` + `float4 bones[80*3]`
（骨骼矩阵按 3 行 `float4` 存 3×4；刚体部件把世界矩阵放在 `bones[0..2]`）。

InputLayout 组合：位置按 `VF_FULLPREC` 取 `R32G32B32A32_FLOAT` 或 `R16G16B16A16_FLOAT`；
蒙皮权重 `R16G16B16A16_FLOAT`、骨骼索引 `R8G8B8A8_UINT`（偏移取自 `GetAttributeOffset(VA_SKINNING)`）
→ 共 4 个 InputLayout（刚体/蒙皮 × 全精度/半精度）。

顶点步长自行计算（**不用 `VertexDesc::GetSize()`**：它对位置恒按 float4 计，未处理
`VF_FULLPREC` 缺失时的 half4 情况）。

## 五、线程模型（关键）

渲染线程不能遍历场景图（主线程可能正在改），所以**采集与绘制分离**：

```
scan()（游戏线程，每 ScanIntervalMs）
  └─ MeshOutline::collect(ref)：遍历 3D 树 → 每个可绘制部件记录
       · NiPointer<NiAVObject> geometry      // 保活：rendererData 随几何存活
       · NiPointer<NiSkinPartition> partition // 保活：分区 GPU 缓冲
       · BSGraphics::TriShape* / VertexDesc / index_count
       · 蒙皮：palette 骨骼 NiPointer 数组 + 各自 skinToBone（静态数据）
     结果存为 shared_ptr<DrawList const> 挂到 CorpseEntry
       ↓ snapshot（互斥拷贝，只拷 shared_ptr）
on_present（渲染线程）
  └─ 逐尸体：读 bones[i]->world（当帧最新）→ 组装骨骼矩阵 → DrawIndexed 到遮罩
```

- **保活策略**：所有跨线程持有的引擎对象一律 `NiPointer`（引用计数），避免 3D 卸载后
  野指针；引用最多存活到下一次扫描替换列表为止。
- **骨骼变换实时读取**：几何句柄按扫描周期采集，但骨骼世界变换在绘制当帧读取，
  ragdoll 仍在下坠时轮廓也跟得上（最坏是一帧撕裂的矩阵，不影响正确性）。
- **模式切换延迟**：只有 `OutlineMode == 1` 时扫描才采集网格，因此在菜单里切到剪影
  后最多 `ScanIntervalMs`（默认 0.5s）出现轮廓，属预期。

## 六、几何来源过滤与降级

采集期只接受能安全按三角形拓扑绘制的部件：

- 类型白名单：`kTriShape` / `kDynamicTriShape` / `kSubIndexTriShape`（均派生自 `BSTriShape`）；
  粒子、线段、实例化等一律跳过（拓扑不同，按三角形画会得到垃圾）。
- 跳过 `NiAVObject::Flag::kHidden` 节点（尸体身上常有隐藏的备用部件）。
- 蒙皮部件要求 `buffData` / `bones` 非空、`numBones ∈ [1, 80]`、`triangles > 0`，
  且骨骼索引在 `NiSkinData` 骨骼数范围内；任一不满足则跳过该分区。

**逐尸体降级**：`OutlineMode = 1` 但某具尸体采集到 0 个部件时，该尸体仍按原包围盒绘制，
不会"什么都不显示"。资源创建失败（着色器/RT）时整体退回 box 模式并记一次错误日志。

## 七、诊断

- 扫描日志逐尸体附加 `parts=N`（走既有 `log_state_once` 去重）：不进游戏也能从日志确认
  采集是否成功、走的是网格还是回退。
- 资源创建、分区跳过、骨骼数超限等失败路径均有一次性告警。

## 八、配置项

```ini
[Display]
; 描边模式：0 = 包围盒线框，1 = 模型剪影
OutlineMode=0
```

- `OutlineThickness` 在剪影模式下是**描边像素半径**（膨胀半径），沿用同一配置项，clamp 到 [1,8]。
- `OutlineColor` / `MinOpacity` / `FadeStartDistance` / `FadePower` / `LootFilter*` 全部沿用。
- MCP 面板新增下拉框 `Outline Mode`（Bounding Box / Model Silhouette）。

## 九、文件改动清单

| 文件 | 改动 |
|---|---|
| `src/mesh_outline.h` / `.cpp`（新增） | 采集（`collect`/`part_count`）+ 渲染（`begin_frame`/`draw`/`resolve`/`release`） |
| `src/corpse_finder.h` / `.cpp` | `CorpseEntry::mesh`（`DrawListPtr`）；扫描期按模式采集；日志附 `parts=` |
| `src/config.h` / `.cpp` | `outline_mode` 字段、INI 读写、`sanitize` clamp 0..1 |
| `src/esp_renderer.cpp` | 提取 `corpse_alpha()`；按模式分流到遮罩绘制；循环后 `resolve` 并恢复 quad 状态 |
| `src/ui_menu.cpp` | `Outline Mode` 下拉框 |
| `src/CMakeLists.txt` | 登记新源文件 |
| `README.md` | 功能与配置说明、路线图勾掉该项 |

依赖方向：`corpse_finder` → `mesh_outline`（采集）、`esp_renderer` → `mesh_outline`（绘制），
`mesh_outline` 只依赖引擎与 D3D，不反向依赖任何本项目模块，无循环依赖。

## 十、已知局限

1. 剪影是**网格轮廓**，不含法线/光照，视觉上是纯色描边（与需求一致）。
2 `BSDynamicTriShape`（带 morph 的脸部/眼睛）走 `rendererData`，极端情况下可能落后一帧。
3. 遮罩为屏幕分辨率的 R8 纹理 ×2（1080p ≈ 4MB），随后台缓冲尺寸变化重建。
4. 膨胀是方形核，转角处描边略方；如需圆角需改成两趟圆核或 JFA，成本更高。
5. 剪影模式下不再绘制 3D 线框盒，`ShowOutline=false` 仍然整体关闭描边。
6. **顶点布局假设**：位置只按 `VF_FULLPREC` 分 float4 / half4 两种，蒙皮属性按
   "4 个 half 权重 + 4 个 uint8 索引"解码。若遇到不符合该布局的网格，剪影会出现错乱
   （切回 `OutlineMode=0` 即可恢复）。六个着色器已用 SDK 的 `fxc` 离线编译验证，
   常量缓冲偏移（0 / 64 / 80）与 C++ 侧 `MaskConstants` 逐字节一致。
7. **跨线程末次释放**：扫描侧的旧列表在游戏线程析构（`scan()` 里先取回再 clear），
   但渲染线程每帧的快照副本仍可能成为某个 `NiPointer` 的最后一个持有者
   （仅当该尸体的 3D 恰好在同一帧被卸载），此时引擎对象会在渲染线程被删除。
   引擎分配器与 D3D11 资源释放本身线程安全，故按可接受风险处理；若要彻底消除，
   需要把待释放列表回传游戏线程（retire queue）。

## 十一、修复记录（2026/08/24）：worldToCam 是完整的世界→裁剪矩阵

**结论**：`RE::NiCamera::GetRuntimeData().worldToCam` 在 SE/AE 上**不是**仿射视图矩阵，
而是引擎完整的**世界→裁剪 view-projection 矩阵**，行主序存储：

- 第 0/1 行：clip 的 x/y 行（近平面缩放后的裁剪坐标）；
- 第 2 行：反 Z（reversed-Z）的 clip z 行（量级约近平面、几乎与距离无关）；
- 第 3 行：w 行 = 前向距离（视线方向上的位移）。

引擎静态函数 `WorldPtToScreenPt3(matrix, port, ...)` 就是按
`NDC = (row0·p, row1·p) / (row3·p)` 计算再映射到 port 的，与这套语义完全一致；
TrueDirectionalMovement / Precision 等生产插件也是把这种全局 viewProj 式矩阵喂给同一个函数。

**探针数值**：旧探针（先用"仿射视图 + 近平面公式"算期望）对表得到的失败样例是
`[silhouette-probe] FAIL expected=(1.0179, 5.7907) engine=(0.5214, 0.6228)`。
按 `expected − 0.5 = (n/r, n/t) × 33.1`（33.1 = 锚点距离 / 近平面）精确拟合，
证明旧期望值恰好是"把相机空间坐标直接除以近平面距离再缩放"的结果——即旧代码把
**视图矩阵当裁剪矩阵**（`clip.w ≡ 1`，NDC 退化为世界/相机坐标），数值在屏幕覆盖尺度。

**修复**：

- 删除 `MeshOutline::compose_world_to_clip`（CPU 端二次组合 `M = P·V` 是错误做法：
  在已经是 view-projection 的矩阵上再套一次投影，透视除法被破坏、相机背后裁剪失效，
  尸体网格按屏幕覆盖尺度光栅化——这正是"描边看不见 + 严重掉帧"的根因）。
- 剪影门控只判 `world_cam != nullptr`，把 `worldToCam` **整份字节拷贝**进遮罩趟的
  常量缓冲；`to_clip` 着色器本身已实现正确的裁剪矩阵数学，一行未改。
- `run_silhouette_probe` 改为验证遮罩趟实际收到的同一份矩阵：`clip = M·anchor`，
  `clip.w <= 0` 记 SKIP，期望值 `(clip.x/clip.w*0.5+0.5, clip.y/clip.w*0.5+0.5)`（左下原点）
  与按 port 归一化的 `WorldPtToScreenPt3` 输出对表，期望 PASS 且 delta < 1e-2。
