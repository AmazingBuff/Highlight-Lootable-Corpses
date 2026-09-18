//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "mask_passes.h"
#include "mask_types.h"

MASK_NAMESPACE_BEGIN

// Render-thread-only facade. Targets are retained by NiPointer. Silhouettes use
// private depth; outlines merge independent whole-target masks. Both ignore scene depth.
class MaskOverlay
{
public:
    MaskOverlay();
    ~MaskOverlay();

    bool init(REX::W32::ID3D11Device* device);
    bool begin_frame(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height);


    // Called from the Present callback: collect the geometry visible from camera and render the
    // mask, then blend the fill/outline onto overlay_target according to the current display_mode
    // - the caller must pass the RTV of the back buffer Present will show (the contract matches the
    // icon path and does not rely on whichever render target happens to be bound at the engine's
    // Present moment).
    // width/height are the back-buffer dimensions (the mask RT has the same size and is
    // recreated when they change).
    void draw(
        REX::W32::ID3D11Device* device,
        REX::W32::ID3D11DeviceContext* context,
        RE::NiCamera* camera,
        REX::W32::ID3D11RenderTargetView* overlay_target,
        uint32_t width,
        uint32_t height,
        std::vector<MaskTarget> const& targets,
        CommonStates const& states);

    void end_frame();

private:
    // Facade state (owned exclusively by the render thread)
    REX::W32::ID3D11Device* m_ref_device;
    RenderTarget m_mask_rt;
    MaskGeometryPass m_geometry_pass;
    SilhouettePass m_silhouette_pass;
    OutlinePass m_outline_pass;

    uint32_t m_width;
    uint32_t m_height;

    bool m_ready;
};

MASK_NAMESPACE_END
