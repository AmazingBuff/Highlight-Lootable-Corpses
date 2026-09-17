# Highlight Lootable Corpses（Corpse ESP）

一个用于 Skyrim AE（1.6.x）的 SKSE 插件：**透视显示周围可搜刮的尸体**。

在野外，尸体经常被草地、灌木或地形起伏挡住而找不到。本插件会持续扫描玩家周围
已加载的引用，凡是"可搜刮"的尸体（尸体 Actor、灰烬堆、干尸/裹尸等静态尸体），
都会在屏幕上绘制一个无视遮挡的 ESP 发光描边标记（类似 FPS 游戏的穿墙透视效果），
方便你快速定位尸体并搜刮战利品。

## 功能

- 只标记**还有物品可搜刮**的尸体（人形 NPC 与动物都包含），搜空后下一轮扫描自动消失
- 支持**灰烬堆**：被复活的敌人再次死亡 / 瓦解类效果产生的灰烬堆（含 DLC 的
  Soul Ember、灰烬魔等变体），通过 `ExtraAshPileRef` 关联原始 Actor 判断库存
- 支持**静态尸体**：干尸/裹尸/烧焦尸体等容器物体（`TreasDraugrAmbushCorpse*`、
  `TreasBurntCorpse*`、`defaultGhostCorpse` 等，含 DLC 变体）
- 三种显示模式（`DisplayMode`）三选一：`silhouette` 按高亮物体之间的逐像素深度选择最近物体的穿墙剪影填充、
  `outline` 分别提取每个物体的外侧描边并合并（被其他物体遮住的边界仍可见）、`icon` 尸体顶部上方的向下箭头，空间和屏幕上都接近的尸体合并为双箭头
- 纯标记，无文字干扰；icon 保留距离淡出，同时近处放大、远处缩小；聚合图标额外放大，双箭头区别于单个目标
- icon 使用带迟滞的有界分组，限制高度差以减少跨楼层合并；所有候选参与分组，最多显示最近的 16 组
- 标记位置取自 Havok 碰撞体（`GetAabbWorldspace`）与 ragdoll 刚体的包围盒投影，
  与尸体实际位置一致
- 完全无视草、灌木、墙壁等遮挡（在场景渲染之后绘制，不使用场景深度；剪影仅比较高亮物体之间的深度）
- 热键一键开关（默认未绑定；可在 MCP 菜单中重绑定，或通过 INI 的 `Hotkey` 键配置）
- **游戏内可视化调参**：全部选项可在 Mod Control Panel（SKSE Menu Framework）
  的 "Highlight Lootable Corpses > Settings" 页面实时调整并保存到 INI
- 全部选项由 INI 配置，首次运行自动生成默认配置文件

## 安装

1. 安装 [SKSE64](https://skse.silverlock.org/)（AE 版本，与游戏版本匹配）
2. 安装 [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
3. 将 `HighlightLootableCorpses.dll` 放入游戏目录的 `Data\SKSE\Plugins\` 下
4. （可选但推荐）安装 [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)，
   以获得游戏内设置面板；未安装时插件其余功能不受影响（日志会提示面板禁用）
5. 启动游戏，进入游戏后插件自动生效

> 日志文件：`Documents\My Games\Skyrim Special Edition\SKSE\HighlightLootableCorpses.log`

## 配置文件

首次运行会在 `Data\SKSE\Plugins\HighlightLootableCorpses.ini` 生成默认配置。
所有选项也都可以在游戏内 Mod Control Panel 中实时调整（"Highlight Lootable Corpses > Settings"，
"Save to INI" 按钮写回本文件）：

```ini
[General]
; 是否默认启用
Enabled=true
; 热键虚拟键码（Windows VK 码，0 = 未绑定，默认禁用热键；在 MCP 菜单中可重绑定）
Hotkey=118
; 最大搜索距离（游戏单位，默认约 17 米）
MaxDistance=2000.0
; 尸体扫描间隔（毫秒）
ScanIntervalMs=500

[Display]
; 尸体显示样式：silhouette（穿墙剪影填充，0）| outline（剪影外描边带，1）| icon（尸体顶部箭头，邻近拥挤目标合并为双箭头，2）
DisplayMode=0
; 图标基础半宽（像素，范围 5–20；此处为示例值，原 IconRadius 键仍兼容）
IconRadius=10
; 描边颜色（RGB 十六进制）
OutlineColor=00FF66
; 远处标记最小不透明度
MinOpacity=0.15
; 描边线宽（像素）
OutlineThickness=2.0
; 开始淡出的距离（游戏单位，该距离内完全不透明）
FadeStartDistance=500.0
; 淡出曲线指数（越大衰减越快，1.0 = 线性）
FadePower=2.0

[LootFilter]
; 当尸体被搜索过后，是否停止其描边效果
HideSearchedEnabled=false
; 战利品筛选总开关：开启后只显示库存命中以下任一分类的尸体标记
ValueFilterEnabled=false
; 各分类开关（默认全关，按需勾选；在 MCP 菜单中亦可实时修改）
ValueQuestItems=false
ValueKeys=false
ValueEnchanted=false
ValueHighValue=false
; 高价值单件阈值（金币；金币堆按枚数计）
HighValueThreshold=100.0
; 书籍分类范围（1=法术 2=技能 3=未读取 7=全部）
BookFilterMode=0
; 消耗品（箭矢/炼金材料/灵魂石/药水/卷轴）
ValueConsumables=false
```

## 构建

需要：Visual Studio 2022（含 C++ 桌面开发）、CMake 3.22+。

依赖（spdlog、fmt、DirectXTK、simpleini、DirectXMath）可通过 vcpkg 安装，
也可以复用本工作区已构建好的依赖目录（`FollowerSummonAllyFix\build\vcpkg_installed\...`）。
仓库包含两个 submodule：`extern/CommonLibSSE`（引擎库）与 `extern/SKSE-MCP`
（游戏内菜单的 header-only 封装），首次克隆需：

```powershell
git submodule update --init --recursive
```

```powershell
# 方案 A：使用 vcpkg（需设置 VCPKG_ROOT 环境变量，首次会安装依赖）
cmake --preset Release

# 方案 B：复用本机已装好的依赖（离线，不经过 vcpkg，本机专用 preset）
cmake --preset ReleaseLocal        # 定义在 CMakeUserPresets.json（不提交）

# 构建（产物在 build/src/Release/HighlightLootableCorpses.dll）
cmake --build build --config Release

# 打 ZIP 包（可选）
cpack --config build/CPackConfig.cmake
```

> 说明：`ReleaseLocal` 是本机专用 preset（含个人依赖路径），定义在
> `CMakeUserPresets.json` 中并已被 `.gitignore` 排除。它关闭 CommonLibSSE 的
> `SKSE_SUPPORT_PATCH_SAFETY` 选项（避免构建时在线拉取 hde64）；对应的
> `/WX` 严格编译兼容问题由项目侧解决：`src/CMakeLists.txt` 对源码构建的
> CommonLibSSE 目标附加 `/wd4100`，`esp_renderer.cpp` 通过 memcpy 按固定布局
> 读取 `NiRect`（成员为 protected）——三方库 `extern/CommonLibSSE` 保持原样。

## 技术说明

- **尸体发现**：每 `ScanIntervalMs` 毫秒通过 `TES::ForEachReferenceInRange`
  按配置半径扫描已加载 cell（内部空间/外部网格/天空 cell），对 Actor 筛选：
  `lifeState == kDead`（直接读位域，不用虚表 `IsDead()`——本机实测其 vtable
  分发不可靠）、非复活/幽灵、非禁用/删除、3D 已加载、库存非空。
- **灰烬堆发现**：同一趟扫描按 FormID 匹配 `DefaultAshPile1/2` 及其 DLC 变体
  （Soul Ember、AshSpawn 等，运行时经 `LookupFormID` 换算完整 FormID）。
  灰烬堆自身没有容器数据，物品挂在 `ExtraAshPileRef` 关联的原始 Actor 上——
  判定可搜刮 = 堆自身只读容器数据或关联 Actor 库存非空，搜刮空后自动消失。
- **静态尸体发现**：FormID 匹配干尸/裹尸/烧焦尸体等 CONT 容器（含 DLC 变体），
  判定可搜刮 = 基类容器条目 + 只读运行时容器数据。
- **渲染**：钩住 `IDXGISwapChain::Present`（vtable 第 8 槽位，与运行时版本
  无关），在游戏帧渲染完成后用自绘管线（自编译 VS/PS + 动态顶点缓冲）
  向后台缓冲绘制；剪影使用独立深度缓存选择最近的高亮表面，轮廓逐目标提取后合并，
  两者均忽略场景深度。最终采用预乘 Alpha 混合，绘制后恢复游戏渲染状态。
- **投影**：`NiCamera::WorldPtToScreenPt3`（主），失败时兜底
  `BSGraphics::State` 相机数据缓存的 viewProj 矩阵。
- **线程模型**：扫描经 `SKSE::GetTaskInterface()->AddTask` 派发（可能运行在
  游戏任务线程池上），渲染线程只读取互斥锁保护的尸体快照与不可变配置；
  热键在渲染回调中轮询。
- **游戏内菜单**：`ui_menu.cpp` 通过 [SKSE-MCP](https://github.com/QTR-Modding/SKSE-MCP)
  （header-only）在 `kDataLoaded` 后探测 `SKSEMenuFramework.dll` 并注册
  "Highlight Lootable Corpses > Settings" 面板；imgui 函数经 `GetProcAddress` 动态调用框架
  导出（`igXXX`），无链接依赖。面板回调在游戏主线程执行，直接读写
  `Config::get_mutable()`（与渲染线程的无锁读取同现有模式）。

## 路线图

- [ ] 可选的屏幕边缘方向箭头（屏幕外尸体提示）

## 功能实现文档

参见[功能文档索引](docs/features/README.md)，其中的 [Mask 渲染](docs/features/mask-rendering.md)说明模式语义、逐目标颜色接口、资源与验证。

Icon 的阈值、缩放、顶部定位和验证范围见 [Icon 渲染](docs/features/icon-rendering.md)。尺寸缩放为基础半宽的 0.75–1.25 倍，分组额外乘 1.2，总上限为 1.5 倍。当前阈值仍需游戏内调节验证；不同高度或相距较远的标记不会仅因屏幕重叠而合并，因此不能保证消除所有覆盖。
