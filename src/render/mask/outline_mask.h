//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "mask_passes.h"
#include "mask_types.h"

#include "Plugin.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace RE
{
    class NiCamera;
    class TESObjectREFR;
}

PLUGIN_NAMESPACE_BEGIN

// RGB is unpremultiplied; opacity already includes configuration alpha, pulse and fade.
struct OutlineMaskTarget
{
    RE::TESObjectREFR* ref;
    DirectX::XMFLOAT4 color;
};

// Render-thread-only facade. Targets are retained by NiPointer. Silhouettes use
// private depth; outlines merge independent whole-target masks. Both ignore scene depth.
class OutlineMask
{
public:
    OutlineMask(OutlineMask const&) = delete;
    OutlineMask(OutlineMask const&&) = delete;
    OutlineMask operator=(OutlineMask&) = delete;
    OutlineMask operator=(OutlineMask&&) = delete;

    static OutlineMask& instance();

    void set_targets(std::vector<OutlineMaskTarget> const& a_targets);

    // Called from the Present callback: collect the geometry visible from a_camera and render the
    // mask, then blend the fill/outline onto a_overlay_target according to the current display_mode
    // - the caller must pass the RTV of the back buffer Present will show (the contract matches the
    // icon path and does not rely on whichever render target happens to be bound at the engine's
    // Present moment).
    // a_width/a_height are the back-buffer dimensions (the mask RT has the same size and is
    // recreated when they change).
    void render(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::NiCamera* a_camera,
        ID3D11RenderTargetView* a_overlay_target, uint32_t a_width, uint32_t a_height);
private:
    OutlineMask();
    ~OutlineMask();

    void render_impl(ID3D11Device* device, ID3D11DeviceContext* context, RE::NiCamera* camera,
        ID3D11RenderTargetView* overlay_target, uint32_t width, uint32_t height);
private:
    // Facade state (owned exclusively by the render thread)
    std::vector<Mask::MaskTarget> m_targets;
    Mask::RenderTarget m_mask_rt;
    Mask::MaskGeometryPass m_geometry_pass;
    Mask::SilhouettePass m_silhouette_pass;
    Mask::OutlinePass m_outline_pass;
};

PLUGIN_NAMESPACE_END
