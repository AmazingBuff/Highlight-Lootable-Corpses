//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <d3d11.h>
#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "render/render_util.h"
#include "mask_types.h"

MASK_NAMESPACE_BEGIN

class RenderTarget
{
public:
    RenderTarget();
    ~RenderTarget();
    RenderTarget(RenderTarget const&) = delete;
    RenderTarget& operator=(RenderTarget const&) = delete;

    bool init(ID3D11Device* device, uint32_t width, uint32_t height);
    void release();

    [[nodiscard]] bool matches(ID3D11Device* device, uint32_t width, uint32_t height) const;
    [[nodiscard]] ID3D11Device* device() const { return m_ref_device; }
    [[nodiscard]] ID3D11RenderTargetView* rtv() const { return m_rtv; }
    [[nodiscard]] ID3D11DepthStencilView* dsv() const { return m_dsv; }
    [[nodiscard]] ID3D11ShaderResourceView* srv() const { return m_srv; }
    [[nodiscard]] std::uint32_t width() const { return m_width; }
    [[nodiscard]] std::uint32_t height() const { return m_height; }
private:
    ID3D11Device* m_ref_device;
    ID3D11Texture2D* m_texture;
    ID3D11Texture2D* m_depth_texture;
    ID3D11DepthStencilView* m_dsv;
    ID3D11RenderTargetView* m_rtv;
    ID3D11ShaderResourceView* m_srv;
    uint32_t m_width;
    uint32_t m_height;
};

class MaskGeometryPass
{
public:
    MaskGeometryPass();
    ~MaskGeometryPass();
    MaskGeometryPass(MaskGeometryPass const&) = delete;
    MaskGeometryPass& operator=(MaskGeometryPass const&) = delete;

    bool init(ID3D11Device* device);
    void release();

    [[nodiscard]] ID3D11BlendState* mask_write_blend() const { return m_blend_mask_write; }
    [[nodiscard]] ID3D11DepthStencilState* depth_nearest() const { return m_depth_nearest; }
    [[nodiscard]] ID3D11DepthStencilState* depth_none() const { return m_depth_disabled; }
    [[nodiscard]] ID3D11RasterizerState* cull_none() const { return m_rasterizer; }

    // 首个有 draw 的帧自动校准 CB 上传字节取向（地面真值直传预期成立；失败时
    // 转置字节序）：锚点 = 首记录节点的世界原点，经组合矩阵投影与引擎
    // WorldPtToScreenPt3 像素比对。
    void calibrate_upload_orientation(
        RE::NiCamera* camera,
        DirectX::XMFLOAT4X4 const& view_proj,
        std::vector<MaskDraw> const& draws,
        std::uint32_t width,
        std::uint32_t height);

    // 绘制全部 draw（调色板蒙皮在此构建）。
    void draw(ID3D11Device* device, ID3D11DeviceContext* context, DirectX::XMFLOAT4X4 const& view_proj, std::span<MaskDraw const> draws);

private:
    // InputLayout 缓存键（蒙皮, 精度, 属性偏移, 步进）
    struct LayoutKey
    {
        bool skinned;
        bool full_prec;
        std::uint32_t position_format;  // 位置格式为标定结果，须入键防不同格式共用布局
        std::uint32_t position_offset;
        std::uint32_t skinning_offset;
        std::uint32_t stride;
        // 蒙皮权重/索引布局：标定结果，须入键防不同布局共用同一 InputLayout
        //（静态 draw 保持默认 0/UNKNOWN）。
        std::uint32_t weight_format;
        std::uint32_t weight_offset;
        std::uint32_t index_format;
        std::uint32_t index_offset;

        bool operator==(LayoutKey const&) const = default;
    };

    bool create_pipeline(ID3D11Device* device);

    ID3D11InputLayout* get_layout(
        ID3D11Device* device, ID3DBlob* blob, bool skinned, RE::BSGraphics::VertexDesc const& desc, std::uint32_t stride,
        DXGI_FORMAT position_format, std::uint32_t position_offset, MaskSkinLayout const* skin_layout);

    void release_layouts();

private:
    ID3D11VertexShader* m_vs_static;
    ID3D11VertexShader* m_vs_skinned;
    ID3D11PixelShader* m_ps_mask;
    ID3DBlob* m_vs_static_blob;   // CreateInputLayout 需要 VS 字节码，随管线保留
    ID3DBlob* m_vs_skinned_blob;
    ID3D11Buffer* m_per_draw_cb;  // b0：row_major float4x4 + uint object_id（80 字节）
    ID3D11Buffer* m_palette_cb;   // b1：row_major float4x4[Max_Palette_Bones]
    ID3D11BlendState* m_blend_mask_write;       // Integer IDs must never be blended.
    ID3D11DepthStencilState* m_depth_nearest;
    ID3D11DepthStencilState* m_depth_disabled;  // 用于独立轮廓 mask 与全屏合成
    ID3D11RasterizerState* m_rasterizer;        // CullNone + depth clipping
    bool m_ready;
    bool m_failed;  // 创建失败后不再每帧重试（避免持续泄漏 D3D 对象）

    // 常量缓冲上传字节取向（自动校准；直传预期成立，失败时转置字节序）
    bool m_upload_transposed;

    // InputLayout 缓存：按 (蒙皮, 精度, 属性偏移, 步进) 缓存——属性偏移来自各 mesh 的
    // vertexDesc，逐 mesh 创建设备对象不可取，故缓存去重（尸体 mesh 布局种类极少）。
    std::vector<std::pair<LayoutKey, ID3D11InputLayout*>> m_layout_cache;
};

// Shared full-screen shaders and a private, dynamically sized target style table.
class FullscreenPass
{
public:
    FullscreenPass();
    virtual ~FullscreenPass();
    FullscreenPass(FullscreenPass const&) = delete;
    FullscreenPass& operator=(FullscreenPass const&) = delete;
    bool update_styles(ID3D11Device* device, ID3D11DeviceContext* context, std::span<MaskTarget const> targets);

    virtual bool init(ID3D11Device* device);
    virtual void release();
protected:
    ID3D11VertexShader* m_vertex_shader;
    ID3D11PixelShader* m_pixel_shader;

    ID3D11BlendState* m_blend_premul_alpha;
    ID3D11Buffer* m_cb;

    bool m_ready;
    bool m_failed;

    ID3D11Buffer* m_style_buffer;
    ID3D11ShaderResourceView* m_style_srv;
    std::size_t m_style_capacity;
    bool m_styles_valid;
};

class SilhouettePass final : public FullscreenPass
{
public:
    ~SilhouettePass() override;
    bool init(ID3D11Device* device) override;
    void release() override;
    bool draw(
        ID3D11DeviceContext* context,
        ID3D11RenderTargetView* target,
        ID3D11ShaderResourceView* mask_srv,
        std::uint32_t width,
        std::uint32_t height,
        ID3D11DepthStencilState* depth_none,
        ID3D11RasterizerState* cull_none) const;
};

class OutlinePass final : public FullscreenPass
{
public:
    ~OutlinePass() override;
    bool init(ID3D11Device* device) override;
    void release() override;
    bool draw(
        ID3D11DeviceContext* context,
        ID3D11RenderTargetView* target,
        ID3D11ShaderResourceView* mask_srv,
        std::uint32_t width,
        std::uint32_t height,
        int thickness,
        ID3D11DepthStencilState* depth_none,
        ID3D11RasterizerState* cull_none) const;
};

MASK_NAMESPACE_END