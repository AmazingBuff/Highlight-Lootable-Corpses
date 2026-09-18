# HighlightLootableCorpses（原 CorpseESP）边框透明度异常 —— 排查记录与修复

> 记录日期：本次会话
> 涉及文件：`src/esp_renderer.cpp`
> 状态：已修复并提交

## 一、现象

- 游戏内尸体的 ESP 边框"透明度不对"：无论距离远近、无论 `min_opacity` / fade 配置如何，
  边框看起来都接近实心，距离衰减（淡出）几乎无效。
- 为排查而在屏幕中央添加的两个测试方块（左 `alpha=1`、右 `alpha=0.5`）**都显示为实心红色**，
  右方块本应明显半透明。
- 配套的像素取证日志全部读出 `0,0,0,0`，连"参照点"（远离方块、必然有场景颜色）也是全 0。

## 二、环境

- Skyrim AE（1.6.x），D3D11，经 `IDXGISwapChain::Present` vtable 钩子绘制。
- 本机开启 Windows HDR，`SkyrimPrefs.ini` 中 `bUse64bitsHDRRenderTarget=1`：
  **交换链后台缓冲格式为 `DXGI_FORMAT_R10G10B10A2_UNORM`（枚举值 24，非常见的 B8G8R8A8=87）**。
- 调用链上另有第三方模块 `BeamWalking.dll` 先于我们 hook 了 Present（无碍，链式正常）。
- 日志：`present diag: 120 calls, 0 skipped` —— 每帧恰好一次 Present，
  排除"同帧多次 Present 导致 alpha 叠加"的猜想（该防重逻辑可保留为防御，但非本问题根因）。

## 三、关键日志（修复前）

```
Back buffer format: 24                                   // = R10G10B10A2_UNORM（HDR）
pixel diag: left=0,0,0,0 right=0,0,0,0 ref=0,0,0,0      // 取证全 0 —— 取证本身坏了
blend desc diag: enable=1 src=2 dst=6 op=1               // src=2 是 ONE，不是 SRC_ALPHA！
Color RGBA: 0, 1, 0.4, 0.39...                           // 尸体边框直 alpha，随距离变化
```

## 四、根因

### 根因 1（主）：混合状态与着色器输出不匹配 —— "预乘混合 + 直 alpha 颜色"

绘制时绑定的是 `DirectX::CommonStates::AlphaBlend()`，其定义（DirectXTK 源码）为：

```cpp
// CommonStates.cpp
ID3D11BlendState* CommonStates::AlphaBlend() const
{
    return ... CreateBlendState(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, pResult);  // 预乘！
}
```

(Note: since the DirectXTK dependency was removed, this state object is provided by the local mirror `src/render/dx11/common_states.cpp`, whose blend descriptions are identical to the DirectXTK source quoted here.)

配合 SDK 枚举（`D3D11_BLEND_ZERO=1, ONE=2, ..., SRC_ALPHA=5, INV_SRC_ALPHA=6`），
日志 `src=2 dst=6` 即 `(ONE, INV_SRC_ALPHA)` —— **这是"预乘 alpha"混合约定**。
DirectXTK 全家（SpriteBatch 等）默认预乘；直 alpha 混合应为 `CommonStates::NonPremultiplied()`
即 `(SRC_ALPHA, INV_SRC_ALPHA)`。

而自绘像素着色器此前直接输出直 alpha 颜色 `return input.color;`（rgb 未乘 alpha）。

**效果**：`final.rgb = src.rgb × ONE + dst.rgb × (1 − a)`，源颜色以全强度叠加，
alpha 只控制背景透出比例：

- 右测试方块 `(1,0,0,0.5)` → `纯红 + 0.5×背景` ≈ **实心红**（两个方块都"实心"）
- 尸体边框 `(0,1,0.4, a∈[0.15,0.4])` → 绿色永远全强度，距离衰减看起来无效

**修复**：像素着色器输出预乘颜色：

```hlsl
return float4(input.color.rgb * input.color.a, input.color.a);
```

保持 `AlphaBlend()` 不变，一处改动同时覆盖矩形边框与 3D 线框两条绘制路径。

> 为什么不改混合状态为 `NonPremultiplied()`？见根因 2 —— 在 2-bit alpha 的后台缓冲上，
> 直 alpha 混合会把小 alpha（如 0.15）量化成 0 导致远处边框直接消失；预乘方案 RGB 走
> 10-bit 全精度，远处边框仍微弱可见，效果更好。

### 根因 2（次）：后台缓冲是 R10G10B10A2（HDR），alpha 仅 2 bit

D3D11 规范：非浮点渲染目标在混合前把源值转换为目标格式精度。`R10G10B10A2` 的 alpha
通道只有 **2 bit**（0、1/3、2/3、1 四级），因此叠加透明度天然被量化：
例如 `alpha=0.5` 实际量化为 2/3。这是硬件格式限制，无法通过混合状态消除；
只能在语义上接受（预乘方案已尽量保住 RGB 精度）。

### 附：像素取证全 0 的原因（诊断工具自身的 bug）

`CopySubresourceRegion` 要求源/目标格式相同（或同 TYPELESS 族），从 `R10G10B10A2`
后台缓冲拷到固定 `R8G8B8A8` staging 纹理是**跨格式族拷贝，被 D3D11 运行时静默丢弃**，
staging 保持初始全 0（所以连参照点都是 0）。这也意味着早期"实测 alpha 混合完全正确"
的结论是坏取证给的假结果，一度把排查方向带偏（多次 Present、Z 形自交等理论均未打中主因）。

**修复**：取证纹理按 `g_back_buffer_format` 动态创建，并按 R10G10B10A2 位域
（低 10 位 R → 高 2 位 A）解码读回；B8G8R8A8 时注意字节序为 B,G,R,A。

## 五、修复清单（`src/esp_renderer.cpp`）

1. 像素着色器输出预乘颜色 `float4(rgb*a, a)`（配合 `AlphaBlend()` 的 ONE/INV_SRC_ALPHA）。
2. 取证纹理改为按当前后台缓冲格式创建 + 按格式解码（修全 0 读回）。
3. 非 B8G8R8A8 后台缓冲时打 warn，提示 HDR 交换链 alpha 精度受限。
4. 更新误导性注释（删除"实测 alpha 混合完全正确"等）。

## 六、验证方法

- 屏幕中央测试方块：左块实心红、右块明显半透明（因 2-bit alpha 量化为 2/3，右块略偏实属预期）。
- 尸体边框：近处清晰、远处渐虚；`Color RGBA` 日志中 alpha≈0.15 的边框应微弱可见而非消失。
- `pixel diag` 应读到真实像素，而非全 0。

## 七、遗留事项

- **HDR 色彩空间**：游戏输出为 HDR（PQ）编码，叠加值仍是 sRGB 直值。若预乘修复后叠加颜色
  在 HDR 屏上仍偏亮/过饱和，需按 HDR 色彩空间单独处理（属第二阶段问题）。
- **2-bit alpha 上限**：只要交换链是 R10G10B10A2，叠加透明度就只能做到 4 级近似；
  如需高质量半透明需中间渲染目标合成（成本高，暂不采用）。
- 本次为排查而添加的测试代码（测试方块、像素取证、`Color RGBA` 日志、`present diag`、
  `Original Present module`、`Projection source` 等临时诊断）已删除，见后续清理提交。
