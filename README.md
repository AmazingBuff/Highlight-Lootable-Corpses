# CorpseESP

一个用于 Skyrim AE（1.6.x）的 SKSE 插件：**透视显示周围可搜刮的尸体**。

在野外，尸体经常被草地、灌木或地形起伏挡住而找不到。本插件会持续扫描玩家周围
已加载的 Actor，凡是"已死亡 **且** 仍有余下可搜刮物品"的尸体，都会在屏幕上绘制
一个无视遮挡的 ESP 发光描边标记（类似 FPS 游戏的穿墙透视效果），方便你快速定位
尸体并搜刮战利品。

## 功能

- 只标记**还有物品可搜刮**的尸体（人形 NPC 与动物都包含）
- 支持**灰烬堆**：被死灵法术复活的敌人再次死亡时会变成灰烬堆（可搜刮），
  本插件会跟踪这类敌人，在其消失后验证灰烬堆并持续标记，直到被搜刮空
- 纯标记，无文字干扰：默认仅绘制**包围盒描边边框**（发光填充与中心点默认关闭，
  可在 INI 开启），按距离自动淡出
- 投影使用引擎本帧实际渲染用的视图×投影矩阵（`BSGraphics::State` 相机数据缓存），
  标记与画面严格对齐，旋转视角不会漂移
- 完全无视草、灌木、墙壁等遮挡（在场景渲染之后绘制，不参与深度测试）
- 热键一键开关（默认 `F7`，可在 INI 中修改）
- 右上角小指示点显示当前开关状态（可在 INI 中关闭）
- 全部选项由 INI 配置，首次运行自动生成默认配置文件
- 线程安全：尸体扫描在游戏主线程执行，渲染线程只读快照数据

## 安装

1. 安装 [SKSE64](https://skse.silverlock.org/)（AE 版本，与游戏版本匹配）
2. 安装 [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
3. 将 `CorpseESP.dll` 放入游戏目录的 `Data\SKSE\Plugins\` 下
4. 启动游戏，进入游戏后插件自动生效

> 日志文件：`Documents\My Games\Skyrim Special Edition\SKSE\CorpseESP.log`

## 配置文件

首次运行会在 `Data\SKSE\Plugins\CorpseESP.ini` 生成默认配置：

```ini
[General]
; 是否默认启用
Enabled=true
; 热键虚拟键码（0x76 = F7）
Hotkey=118
; 最大搜索距离（游戏单位，约 114 米）
MaxDistance=8000.0
; 尸体扫描间隔（毫秒）
ScanIntervalMs=500

[Display]
; 描边颜色（RGB 十六进制）
OutlineColor=00FF66
; 发光强度
GlowAlpha=0.30
; 远处标记最小不透明度
MinOpacity=0.15
; 描边线宽（像素）
OutlineThickness=2.0
; 是否画包围盒描边
ShowOutline=true
; 是否画发光
ShowGlow=true
; 是否画中心点
ShowCenterDot=true
; 是否画右上角开关指示点
ShowIndicator=true
```

## 构建

需要：Visual Studio 2022/2026（含 C++ 桌面开发）、CMake 3.22+。

依赖（spdlog、fmt、DirectXTK、simpleini、DirectXMath）可通过 vcpkg 安装，
也可以直接复用本工作区中已构建好的依赖目录（`FollowerSummonAllyFix\build\vcpkg_installed\...`）。

```powershell
# 方案 A：使用 vcpkg（需设置 VCPKG_ROOT 环境变量，首次会安装依赖）
cmake --preset Release

# 方案 B：复用本机已装好的依赖（离线，不经过 vcpkg）
cmake --preset ReleaseLocal

# 构建（产物在 build/src/Release/CorpseESP.dll）
cmake --build build --config Release

# 打 ZIP 包（可选）
cpack --config build/CPackConfig.cmake
```

> 说明：`ReleaseLocal` 预设会关闭 CommonLibSSE 的 `SKSE_SUPPORT_PATCH_SAFETY`
> 选项（避免构建时在线拉取 hde64），并对 vendored 的
> `extern/CommonLibSSE/src/SKSE/Trampoline.cpp` 中两个未使用形参做了 `(void)`
> 消参处理，以兼容 `/WX` 严格编译。该选项只影响代码补丁写入前的边界校验，
> 本插件不使用代码补丁，不受影响。另外对
> `extern/CommonLibSSE/include/RE/N/NiRect.h` 补充了 4 个只读访问器
> （`GetLeft/GetRight/GetTop/GetBottom`），用于读取相机 port 的原点。

## 技术说明

- **尸体发现**：每 `ScanIntervalMs` 毫秒在游戏主线程扫描
  `ProcessLists` 的全部 Actor 列表（high / middleHigh / middleLow / low），
  筛选：`IsDead()`、非幽灵、非禁用/删除、3D 已加载、距离阈值内、
  `GetInventory()` 非空。
- **灰烬堆发现**：被复活的敌人携带 archetype 为 `kReanimate`/`kTurnUndead`
  的激活效果（其 `magicAttachAshPileOnDeath` 脚本会在死亡时生成
  `DefaultAshPile*` 激活体并转移装备）。插件持续跟踪这类 Actor，当句柄失效
  （被删除）时，用 `TESObjectCELL::ForEachReferenceInRange` 在最后位置附近
  验证是否出现新的、仍有物品的 `DefaultAshPile*` 引用；确认后持续跟踪该引用，
  直到被搜刮空或卸载。
- **渲染**：钩住 `IDXGISwapChain::Present`（vtable 第 8 槽位，与运行时版本
  无关，不依赖 Address Library 的偏移 ID），在游戏帧渲染完成后用
  DirectXTK（`BasicEffect` + `PrimitiveBatch`）向后台缓冲绘制；
  绘制期间关闭深度测试与背面剔除，开启 Alpha 混合，绘制后恢复游戏的
  RenderTarget / Blend / Depth / Rasterizer 状态。
- **投影**：`NiCamera::WorldPtToScreenPt3`（Address Library 标准 ID），
  标记尺寸由尸体世界包围盒半径（`NiAVObject::worldBound`）投影到屏幕得到。
- **线程模型**：扫描与配置读写只在游戏线程；渲染线程只读取互斥锁保护的
  尸体快照与不可变配置；热键在渲染回调中轮询。

## 路线图

- [ ] 真·模型剪影描边（渲染管线钩子重绘尸体轮廓，替代包围盒描边）
- [ ] 可选的屏幕边缘方向箭头（屏幕外尸体提示）
