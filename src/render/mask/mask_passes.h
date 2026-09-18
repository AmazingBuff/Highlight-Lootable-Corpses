//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "mask_types.h"

PLUGIN_NAMESPACE_BEGIN

class CommonStates;

PLUGIN_NAMESPACE_END

MASK_NAMESPACE_BEGIN

class RenderTarget
{
public:
    RenderTarget();
    ~RenderTarget();
    RenderTarget(RenderTarget const&) = delete;
    RenderTarget& operator=(RenderTarget const&) = delete;

    bool init(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height);
    void release();

    [[nodiscard]] bool matches(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height) const noexcept;
    [[nodiscard]] REX::W32::ID3D11Device* device() const noexcept { return m_ref_device; }
    [[nodiscard]] REX::W32::ID3D11RenderTargetView* rtv() const noexcept { return m_rtv; }
    [[nodiscard]] REX::W32::ID3D11DepthStencilView* dsv() const noexcept { return m_dsv; }
    [[nodiscard]] REX::W32::ID3D11ShaderResourceView* srv() const noexcept { return m_srv; }
    [[nodiscard]] uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] uint32_t height() const noexcept { return m_height; }
private:
    REX::W32::ID3D11Device* m_ref_device;
    REX::W32::ID3D11Texture2D* m_texture;
    REX::W32::ID3D11Texture2D* m_depth_texture;
    REX::W32::ID3D11DepthStencilView* m_dsv;
    REX::W32::ID3D11RenderTargetView* m_rtv;
    REX::W32::ID3D11ShaderResourceView* m_srv;
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

    bool init(REX::W32::ID3D11Device* device);
    void release();

    [[nodiscard]] REX::W32::ID3D11DepthStencilState* depth_nearest() const noexcept { return m_depth_nearest; }

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
    void draw(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context, DirectX::XMFLOAT4X4 const& view_proj, std::span<MaskDraw const> draws);

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

    bool create_pipeline(REX::W32::ID3D11Device* device);

    REX::W32::ID3D11InputLayout* get_layout(
        REX::W32::ID3D11Device* device, REX::W32::ID3DBlob* blob, bool skinned, RE::BSGraphics::VertexDesc const& desc, uint32_t stride,
        REX::W32::DXGI_FORMAT position_format, uint32_t position_offset, MaskSkinLayout const* skin_layout);

    void release_layouts();

private:
    REX::W32::ID3D11VertexShader* m_vs_static;
    REX::W32::ID3D11VertexShader* m_vs_skinned;
    REX::W32::ID3D11PixelShader* m_ps_mask;
    REX::W32::ID3DBlob* m_vs_static_blob;   // CreateInputLayout needs the VS bytecode, so it is kept with the pipeline
    REX::W32::ID3DBlob* m_vs_skinned_blob;
    REX::W32::ID3D11Buffer* m_per_draw_cb;  // b0: row_major float4x4 + uint object_id (80 bytes)
    REX::W32::ID3D11Buffer* m_palette_cb;   // b1: row_major float4x4[Max_Palette_Bones]
    REX::W32::ID3D11DepthStencilState* m_depth_nearest;  // Reverse-Z nearest-depth test; no CommonStates equivalent.
    bool m_ready;
    bool m_failed;  // after a creation failure there is no per-frame retry (avoids continuously leaking D3D objects)

    // Constant-buffer upload byte orientation (auto-calibrated; direct upload is expected to hold, and on failure the byte order is transposed)
    bool m_upload_transposed;

    // InputLayout cache: keyed by (skinned, precision, attribute offsets, stride) - the attribute
    // offsets come from each mesh's vertexDesc and creating a device object per mesh is not
    // acceptable, so the cache deduplicates (corpse meshes come in very few layout varieties).
    std::vector<std::pair<LayoutKey, REX::W32::ID3D11InputLayout*>> m_layout_cache;
};

// Shared full-screen shaders and a private, dynamically sized target style table.
class FullscreenPass
{
public:
    FullscreenPass();
    virtual ~FullscreenPass();
    FullscreenPass(FullscreenPass const&) = delete;
    FullscreenPass& operator=(FullscreenPass const&) = delete;
    bool update_styles(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context, std::span<MaskTarget const> targets);

    virtual bool init(REX::W32::ID3D11Device* device);
    virtual void release();
protected:
    REX::W32::ID3D11VertexShader* m_vertex_shader;
    REX::W32::ID3D11PixelShader* m_pixel_shader;

    REX::W32::ID3D11Buffer* m_cb;

    bool m_ready;
    bool m_failed;

    REX::W32::ID3D11Buffer* m_style_buffer;
    REX::W32::ID3D11ShaderResourceView* m_style_srv;
    size_t m_style_capacity;
    bool m_styles_valid;
};

class SilhouettePass final : public FullscreenPass
{
public:
    ~SilhouettePass() override;
    bool init(REX::W32::ID3D11Device* device) override;
    void release() override;
    bool draw(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11RenderTargetView* target,
        REX::W32::ID3D11ShaderResourceView* mask_srv,
        uint32_t width,
        uint32_t height,
        CommonStates const& states) const;
};

class OutlinePass final : public FullscreenPass
{
public:
    ~OutlinePass() override;
    bool init(REX::W32::ID3D11Device* device) override;
    void release() override;
    bool draw(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11RenderTargetView* target,
        REX::W32::ID3D11ShaderResourceView* mask_srv,
        uint32_t width,
        uint32_t height,
        int thickness,
        CommonStates const& states) const;
};

MASK_NAMESPACE_END