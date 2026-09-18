//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "mask_types.h"

#include "render/render_util.h"

#include "Plugin.h"

#include <DirectXMath.h>
#include <d3d11.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

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

    [[nodiscard]] bool matches(ID3D11Device* device, uint32_t width, uint32_t height) const noexcept;
    [[nodiscard]] ID3D11Device* device() const noexcept { return m_ref_device; }
    [[nodiscard]] ID3D11RenderTargetView* rtv() const noexcept { return m_rtv; }
    [[nodiscard]] ID3D11DepthStencilView* dsv() const noexcept { return m_dsv; }
    [[nodiscard]] ID3D11ShaderResourceView* srv() const noexcept { return m_srv; }
    [[nodiscard]] uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] uint32_t height() const noexcept { return m_height; }
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

    [[nodiscard]] ID3D11BlendState* mask_write_blend() const noexcept { return m_blend_mask_write; }
    [[nodiscard]] ID3D11DepthStencilState* depth_nearest() const noexcept { return m_depth_nearest; }
    [[nodiscard]] ID3D11DepthStencilState* depth_none() const noexcept { return m_depth_disabled; }
    [[nodiscard]] ID3D11RasterizerState* cull_none() const noexcept { return m_rasterizer; }

    // On the first frame that has a draw, auto-calibrate the byte orientation of the CB upload
    // (direct upload is expected to hold as the ground truth; on failure the byte order is
    // transposed): anchor = the world origin of the first recorded node, projected through the
    // composed matrix and compared in pixels against the engine's WorldPtToScreenPt3.
    void calibrate_upload_orientation(
        RE::NiCamera* camera,
        DirectX::XMFLOAT4X4 const& view_proj,
        std::vector<MaskDraw> const& draws,
        uint32_t width,
        uint32_t height);

    // Draw every draw (the palette skinning is built here).
    void draw(ID3D11Device* device, ID3D11DeviceContext* context, DirectX::XMFLOAT4X4 const& view_proj, std::span<MaskDraw const> draws);

private:
    // InputLayout cache key (skinned, precision, attribute offsets, stride)
    struct LayoutKey
    {
        bool skinned;
        bool full_prec;
        uint32_t position_format;  // the position format is a calibration result and must enter the key so different formats do not share a layout
        uint32_t position_offset;
        uint32_t skinning_offset;
        uint32_t stride;
        // Skinning weight/index layout: a calibration result that must enter the key so different
        // layouts do not share one InputLayout (static draws keep the default 0/UNKNOWN).
        uint32_t weight_format;
        uint32_t weight_offset;
        uint32_t index_format;
        uint32_t index_offset;

        bool operator==(LayoutKey const&) const = default;
    };

    bool create_pipeline(ID3D11Device* device);

    ID3D11InputLayout* get_layout(
        ID3D11Device* device, ID3DBlob* blob, bool skinned, RE::BSGraphics::VertexDesc const& desc, uint32_t stride,
        DXGI_FORMAT position_format, uint32_t position_offset, MaskSkinLayout const* skin_layout);

    void release_layouts();

private:
    ID3D11VertexShader* m_vs_static;
    ID3D11VertexShader* m_vs_skinned;
    ID3D11PixelShader* m_ps_mask;
    ID3DBlob* m_vs_static_blob;   // CreateInputLayout needs the VS bytecode, so it is kept with the pipeline
    ID3DBlob* m_vs_skinned_blob;
    ID3D11Buffer* m_per_draw_cb;  // b0: row_major float4x4 + uint object_id (80 bytes)
    ID3D11Buffer* m_palette_cb;   // b1: row_major float4x4[Max_Palette_Bones]
    ID3D11BlendState* m_blend_mask_write;       // Integer IDs must never be blended.
    ID3D11DepthStencilState* m_depth_nearest;
    ID3D11DepthStencilState* m_depth_disabled;  // used for the independent outline mask and the full-screen composite
    ID3D11RasterizerState* m_rasterizer;        // CullNone + depth clipping
    bool m_ready;
    bool m_failed;  // after a creation failure there is no per-frame retry (avoids continuously leaking D3D objects)

    // Constant-buffer upload byte orientation (auto-calibrated; direct upload is expected to hold, and on failure the byte order is transposed)
    bool m_upload_transposed;

    // InputLayout cache: keyed by (skinned, precision, attribute offsets, stride) - the attribute
    // offsets come from each mesh's vertexDesc and creating a device object per mesh is not
    // acceptable, so the cache deduplicates (corpse meshes come in very few layout varieties).
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
    size_t m_style_capacity;
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
        uint32_t width,
        uint32_t height,
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
        uint32_t width,
        uint32_t height,
        int thickness,
        ID3D11DepthStencilState* depth_none,
        ID3D11RasterizerState* cull_none) const;
};

MASK_NAMESPACE_END