//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <d3d11.h>

#include <cstdint>
#include <vector>

#include "d3d11_util.h"
#include "mask_types.h"

namespace RE
{
	class NiCamera;
}

PLUGIN_NAMESPACE_BEGIN

// mask RT 清屏色
inline constexpr float Mask_Clear_Color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

// mask 离屏渲染目标：与后备缓冲同尺寸（RGBA8_UNORM，无深度缓冲——mask 天然穿墙）。
// 设备或尺寸变化时整体重建；对象生存期由门面管理（设备变化时先释放依附旧设备的管线）。
class MaskRenderTarget
{
public:
    // 确保纹理/RTV/SRV 与 (device, width, height) 匹配；失败返回 false。
    bool ensure(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height);
    void release();

    [[nodiscard]] bool matches(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height) const;
    [[nodiscard]] ID3D11Device* device() const { return m_device; }
    [[nodiscard]] ID3D11RenderTargetView* rtv() const { return m_rtv; }
    [[nodiscard]] ID3D11ShaderResourceView* srv() const { return m_srv; }
    [[nodiscard]] std::uint32_t width() const { return m_width; }
    [[nodiscard]] std::uint32_t height() const { return m_height; }

private:
    void release_views();

    ID3D11Device* m_device = nullptr;  // 仅用于设备变化比较，不持引用
    ID3D11Texture2D* m_texture = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;
    ID3D11ShaderResourceView* m_srv = nullptr;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};

// ---------------------------------------------------------------------------
// 几何 → mask 绘制 pass：static/skinned 两个 VS + 单一 mask PS。
// 蒙皮 VS 使用矩阵调色板常量缓冲（每分区一次 draw，palette 容量受限）。
// 门面负责绑定 OM（mask RT + MAX 混合）/深度/光栅化/视口并清屏；本类提供
// 这些状态对象并只做 IA/VS/PS/CB 与 DrawIndexed。
// ---------------------------------------------------------------------------
class MaskGeometryPass
{
public:
    bool ensure(ID3D11Device* a_device);
    void release();
    // InputLayout 缓存随 mask RT 重建而清空（原 release_mask_target 行为）。
    void release_layouts();

    [[nodiscard]] bool ready() const { return m_ready; }
    [[nodiscard]] ID3D11BlendState* mask_write_blend() const { return m_blend_mask_write; }
    [[nodiscard]] ID3D11DepthStencilState* depth_none() const { return m_depth_disabled; }
    [[nodiscard]] ID3D11RasterizerState* cull_none() const { return m_rasterizer; }

    // 首个有 draw 的帧自动校准 CB 上传字节取向（地面真值直传预期成立；失败时
    // 转置字节序）：锚点 = 首记录节点的世界原点，经组合矩阵投影与引擎
    // WorldPtToScreenPt3 像素比对。
    void calibrate_upload_orientation(
        RE::NiCamera* a_camera,
        MaskMat4 const& a_view_proj,
        std::vector<MaskDraw> const& a_draws,
        std::uint32_t a_width,
        std::uint32_t a_height);

    // 绘制全部 draw（调色板蒙皮在此构建）。
    void render(ID3D11Device* a_device, ID3D11DeviceContext* a_context, MaskMat4 const& a_view_proj, std::vector<MaskDraw> const& a_draws);

private:
    bool create_pipeline(ID3D11Device* a_device);
    ID3D11InputLayout* get_layout(
        ID3D11Device* a_device, bool a_skinned, RE::BSGraphics::VertexDesc const& a_desc, std::uint32_t a_stride,
        DXGI_FORMAT a_position_format, std::uint32_t a_position_offset, MaskSkinLayout const* a_skin_layout);

    ID3D11VertexShader* m_vs_static = nullptr;
    ID3D11VertexShader* m_vs_skinned = nullptr;
    ID3D11PixelShader* m_ps_mask = nullptr;
    ID3DBlob* m_vs_static_blob = nullptr;   // CreateInputLayout 需要 VS 字节码，随管线保留
    ID3DBlob* m_vs_skinned_blob = nullptr;
    ID3D11Buffer* m_per_draw_cb = nullptr;  // b0：row_major float4x4 + mask alpha（80 字节）
    ID3D11Buffer* m_palette_cb = nullptr;   // b1：row_major float4x4[Max_Palette_Bones]
    ID3D11BlendState* m_blend_mask_write = nullptr;       // MAX 混合：mask 取各 draw 覆盖的并集
    ID3D11DepthStencilState* m_depth_disabled = nullptr;  // 深度测试关闭 —— mask 天然穿墙
    ID3D11RasterizerState* m_rasterizer = nullptr;        // CullNone + DepthClipEnable=FALSE
    bool m_ready = false;
    bool m_failed = false;  // 创建失败后不再每帧重试（避免持续泄漏 D3D 对象）

    // 常量缓冲上传字节取向（自动校准；直传预期成立，失败时转置字节序）
    bool m_upload_transposed = false;
};

// ---------------------------------------------------------------------------
// mask 的全屏消费 pass 公共脚手架：全屏三角形 VS（SV_VertexID）、POINT/CLAMP
// 采样器、预乘 alpha 混合与 per-frame alpha LUT 常量缓冲（b1）。
// 深度/光栅化状态借用 MaskGeometryPass 的（绘制参数传入，非本类所有权）。
// ---------------------------------------------------------------------------
class FullscreenPass
{
public:
    void release();

    // per-frame alpha LUT（256 项，与目标快照同序）整块上传（PS b1）
    void update_alpha_lut(ID3D11DeviceContext* a_context, float const* a_lut);

protected:
    bool ensure_common(ID3D11Device* a_device);
    [[nodiscard]] bool common_ready() const { return m_vertex_shader && m_sampler && m_blend_premul_alpha && m_alpha_lut_cb; }

    ID3D11VertexShader* m_vertex_shader = nullptr;
    ID3D11SamplerState* m_sampler = nullptr;
    ID3D11BlendState* m_blend_premul_alpha = nullptr;
    ID3D11Buffer* m_alpha_lut_cb = nullptr;
};

// silhouette 模式：剪影内部填充（coverage × LUT × 填充系数，单色 OutlineColor）
class SilhouettePass final : public FullscreenPass
{
public:
    bool ensure(ID3D11Device* a_device);
    void release();

    [[nodiscard]] bool ready() const { return m_ready; }

    // 返回 false 表示管线对象缺失未绘制（正常路径不发生）
    bool draw(
        ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, ID3D11ShaderResourceView* a_mask_srv,
        std::uint32_t a_width, std::uint32_t a_height, RgbColor const& a_color,
        ID3D11DepthStencilState* a_depth_none, ID3D11RasterizerState* a_cull_none);

private:
    ID3D11PixelShader* m_pixel_shader = nullptr;
    ID3D11Buffer* m_cb = nullptr;  // b0：float4（OutlineColor rgb + 填充系数）
    bool m_ready = false;
    bool m_failed = false;  // 创建失败后不再重试（设备变化 release 后重置，允许重建）
};

// outline 模式：对 mask 做圆盘膨胀，仅在剪影外侧画 OutlineColor 色带
class OutlinePass final : public FullscreenPass
{
public:
    bool ensure(ID3D11Device* a_device);
    void release();

    [[nodiscard]] bool ready() const { return m_ready; }

    // a_thickness 为 INI 配置的描边厚度（像素），此处 clamp 到 PS 采样预算内。
    // 返回 false 表示管线对象缺失未绘制（正常路径不发生）。
    bool draw(
        ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, ID3D11ShaderResourceView* a_mask_srv,
        std::uint32_t a_width, std::uint32_t a_height, RgbColor const& a_color, float a_thickness,
        ID3D11DepthStencilState* a_depth_none, ID3D11RasterizerState* a_cull_none);

private:
    ID3D11PixelShader* m_pixel_shader = nullptr;
    ID3D11Buffer* m_cb = nullptr;  // b0：texel/radius/pad + color（32 字节）
    bool m_ready = false;
    bool m_failed = false;  // 创建失败后不再重试（设备变化 release 后重置，允许重建）
};

PLUGIN_NAMESPACE_END
