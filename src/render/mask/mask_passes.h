//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "base/def.h"
#include "mask_glow.h"
#include "mask_roi.h"
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
    [[nodiscard]] REX::W32::ID3D11RenderTargetView* rtv() const noexcept { return m_rtv; }
    [[nodiscard]] REX::W32::ID3D11DepthStencilView* dsv() const noexcept { return m_dsv; }
    [[nodiscard]] REX::W32::ID3D11ShaderResourceView* srv() const noexcept { return m_srv; }
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

    // Draw every draw (the palette skinning is built here).
    void draw(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context, DirectX::XMFLOAT4X4 const& view_proj, std::span<MaskDraw const> draws);
private:
    // InputLayout cache key (skinned, precision, attribute offsets, stride)
    struct LayoutKey
    {
        bool skinned;
        bool full_precision;
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
    };

    struct LayoutKeyHash
    {
        size_t operator()(LayoutKey const& key) const noexcept
        {
            size_t val = Amazing_Hash;
            hash_combine_mul(val, key.skinned, key.full_precision,
                key.position_format, key.position_offset, key.skinning_offset, key.stride,
                key.weight_format, key.weight_offset, key.index_format, key.index_offset);
            return val;
        }
    };

    struct LayoutKeyEqual
    {
        bool operator()(LayoutKey const& lhs, LayoutKey const& rhs) const noexcept
        {
            return lhs.skinned == rhs.skinned && lhs.full_precision == rhs.full_precision &&
                    lhs.position_format == rhs.position_format && lhs.position_offset == rhs.position_offset &&
                    lhs.skinning_offset == rhs.skinning_offset && lhs.stride == rhs.stride &&
                    lhs.weight_format == rhs.weight_format && lhs.weight_offset == rhs.weight_offset &&
                    lhs.index_format == rhs.index_format && lhs.index_offset == rhs.index_offset;
        }
    };
private:
    bool create_pipeline(REX::W32::ID3D11Device* device);
    REX::W32::ID3D11InputLayout* get_layout(
        REX::W32::ID3D11Device* device,
        REX::W32::ID3DBlob* blob,
        bool skinned,
        RE::BSGraphics::VertexDesc const& desc,
        uint32_t stride,
        REX::W32::DXGI_FORMAT position_format,
        uint32_t position_offset,
        MaskSkinLayout const* skin_layout);
    void release_layouts();
private:
    REX::W32::ID3D11VertexShader* m_ref_vs_static;
    REX::W32::ID3D11VertexShader* m_ref_vs_skinned;
    REX::W32::ID3D11PixelShader* m_ref_ps_mask;
    REX::W32::ID3D11Buffer* m_per_draw_cb;  // b0: row_major float4x4 + uint object_id (80 bytes)
    REX::W32::ID3D11Buffer* m_palette_cb;   // b1: row_major float4x4[Max_Palette_Bones]
    REX::W32::ID3D11DepthStencilState* m_depth_nearest;  // Reverse-Z nearest-depth test; no CommonStates equivalent.

    // InputLayout cache: keyed by (skinned, precision, attribute offsets, stride) - the attribute
    // offsets come from each mesh's vertexDesc and creating a device object per mesh is not
    // acceptable, so the cache deduplicates (corpse meshes come in very few layout varieties).
    std::unordered_map<LayoutKey, REX::W32::ID3D11InputLayout*, LayoutKeyHash, LayoutKeyEqual> m_layout_cache;
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

    virtual bool init([[maybe_unused]] REX::W32::ID3D11Device* device);
    virtual void release();
protected:
    REX::W32::ID3D11VertexShader* m_ref_vertex_shader;
    REX::W32::ID3D11PixelShader* m_ref_pixel_shader;

    REX::W32::ID3D11Buffer* m_cb;

    REX::W32::ID3D11Buffer* m_style_buffer;
    REX::W32::ID3D11ShaderResourceView* m_style_srv;
    size_t m_style_capacity;
};

class GlowScratch
{
public:
    GlowScratch();
    ~GlowScratch();
    GlowScratch(GlowScratch const&) = delete;
    GlowScratch& operator=(GlowScratch const&) = delete;

    bool init(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height);
    void release();

    [[nodiscard]] bool matches(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height) const noexcept;
    [[nodiscard]] REX::W32::ID3D11RenderTargetView* rtv() const noexcept { return m_rtv; }
    [[nodiscard]] REX::W32::ID3D11ShaderResourceView* srv() const noexcept { return m_srv; }

private:
    REX::W32::ID3D11Device* m_ref_device;
    REX::W32::ID3D11Texture2D* m_texture;
    REX::W32::ID3D11RenderTargetView* m_rtv;
    REX::W32::ID3D11ShaderResourceView* m_srv;
    uint32_t m_width;
    uint32_t m_height;
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
    OutlinePass();
    ~OutlinePass() override;
    bool init(REX::W32::ID3D11Device* device) override;
    void release() override;
    bool draw(
        REX::W32::ID3D11Device* device,
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11RenderTargetView* target,
        REX::W32::ID3D11ShaderResourceView* mask_srv,
        uint32_t width,
        uint32_t height,
        uint32_t object_id,
        int thickness,
        ROI::Rect horizontal_rect,
        ROI::Rect vertical_rect,
        CommonStates const& states);
private:
    GlowScratch m_scratch;
    REX::W32::ID3D11PixelShader* m_horizontal_shader;
};

MASK_NAMESPACE_END
