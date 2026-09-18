//
// Created by AmazingBuff on 2026/9/13.
//

#include "mask_overlay.h"

#include "mask_depth.h"
#include "mask_geometry.h"
#include "mask_passes.h"

#include "config/config.h"
#include "render/dx11/common_states.h"
#include "render/dx11/d3d11_util.h"

MASK_NAMESPACE_BEGIN

namespace
{
    [[nodiscard]] ROI::Region group_region(
        std::span<MaskDraw const> group,
        DirectX::XMFLOAT4X4 const& view_proj,
        MaskGeometryPass const& geometry_pass,
        uint32_t width,
        uint32_t height)
    {
        if (geometry_pass.upload_transposed())
            return ROI::full_region();

        std::vector<ROI::Sphere> spheres;
        spheres.reserve(group.size());
        for (MaskDraw const& draw : group)
        {
            if (!draw.node)
                return ROI::full_region();
            RE::NiBound const& bound = draw.node->worldBound;
            spheres.push_back({
                .center_x = static_cast<double>(bound.center.x),
                .center_y = static_cast<double>(bound.center.y),
                .center_z = static_cast<double>(bound.center.z),
                .radius = static_cast<double>(bound.radius),
            });
        }
        return ROI::make_region(spheres, view_proj,
            { static_cast<int32_t>(width), static_cast<int32_t>(height) });
    }

    [[nodiscard]] ROI::Rect full_rect(uint32_t width, uint32_t height) noexcept
    {
        return { 0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height) };
    }

    // Column-vector projection: replace only the z row so z/w = near / view distance.
    // The w gradient accounts for a uniform scale in the engine projection. Applying
    // this before World/palette multiplication preserves homogeneous skin weights.
    bool make_private_depth_projection(DirectX::XMFLOAT4X4& projection, float near_plane, bool ortho)
    {
        if (ortho)
            return false;

        float const scale = std::hypot(projection._41, projection._42, projection._43);
        float const near_clip = near_plane * scale;
        projection._31 = 0.0f;
        projection._32 = 0.0f;
        projection._33 = 0.0f;
        projection._34 = near_clip;

        return true;
    }
}


MaskOverlay::MaskOverlay() : m_ref_device(nullptr), m_width(0), m_height(0), m_ready(false) {}

MaskOverlay::~MaskOverlay() = default;

bool MaskOverlay::init(REX::W32::ID3D11Device* device)
{
    if (m_ready)
        return true;
    
    const bool geometry_ready = m_geometry_pass.init(device);
    const bool silhouette_ready = m_silhouette_pass.init(device);
    const bool outline_ready = m_outline_pass.init(device);
    
    m_ready = geometry_ready && silhouette_ready && outline_ready;
    m_ref_device = device;
    return m_ready;
}

bool MaskOverlay::begin_frame(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height)
{
    if (!m_mask_rt.matches(device, width, height))
    {
        m_mask_rt.release();
        m_ready = init(device) && m_mask_rt.init(m_ref_device, width, height);
    }

    return m_ready;
}

void MaskOverlay::draw(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context, RE::NiCamera* camera,
    REX::W32::ID3D11RenderTargetView* overlay_target, uint32_t width, uint32_t height, std::vector<MaskTarget> const& targets, CommonStates const& states)
{
    DirectX::XMFLOAT4X4 view_proj{};
    float const (&world_to_cam)[4][4] = camera->GetRuntimeData().worldToCam;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            view_proj.m[row][col] = world_to_cam[row][col];

    RE::NiFrustum const& frustum = camera->GetRuntimeData2().viewFrustum;
    if (!make_private_depth_projection(view_proj, camera->GetNearPlane(), frustum.bOrtho))
    {
        logger::warn("Mask overlay: invalid or orthographic camera; frame skipped");
        return;
    }

    std::vector<MaskDraw> draws;
    collect_mask_draws(targets, draws);
    if (draws.empty())
        return;
    
    m_geometry_pass.calibrate_upload_orientation(camera, view_proj, draws, width, height);

    Config const& cfg = Setting::instance().get_config();
    bool const silhouette = cfg.display_mode == Config::DisplayMode::e_silhouette;

    FullscreenPass& consumer = silhouette ? static_cast<FullscreenPass&>(m_silhouette_pass) : static_cast<FullscreenPass&>(m_outline_pass);
    if (!consumer.update_styles(device, context, targets))
    {
        static bool s_style_failure_logged = false;
        if (!s_style_failure_logged)
        {
            logger::warn("Mask overlay: style table upload failed; frame skipped");
            s_style_failure_logged = true;
        }
        return;
    }

    D3D11StateCapture capture(context);
    capture.capture();
    
    REX::W32::D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    REX::W32::ID3D11RenderTargetView* mask_rtv = m_mask_rt.rtv();
    auto const draw_mask = [&](std::span<MaskDraw const> group,
                               bool clear_mask,
                               REX::W32::ID3D11RasterizerState* rasterizer,
                               REX::W32::D3D11_RECT const* scissor)
    {
        REX::W32::ID3D11ShaderResourceView* const empty_srvs[3]{};
        context->PSSetShaderResources(0, 3, empty_srvs);
        context->OMSetRenderTargets(1, &mask_rtv, silhouette ? m_mask_rt.dsv() : nullptr);
        if (clear_mask)
            context->ClearRenderTargetView(mask_rtv, Mask_Clear_Color);
        if (silhouette)
            context->ClearDepthStencilView(m_mask_rt.dsv(), REX::W32::D3D11_CLEAR_DEPTH, 0.0f, 0);
        context->OMSetBlendState(states.opaque(), nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(silhouette ? m_geometry_pass.depth_nearest() : states.depth_none(), 0);
        context->RSSetState(rasterizer);
        if (scissor)
            context->RSSetScissorRects(1, scissor);
        context->RSSetViewports(1, &vp);
        m_geometry_pass.draw(device, context, view_proj, group);
    };
    if (silhouette)
    {
        draw_mask(draws, true, states.cull_none(), nullptr);
        m_silhouette_pass.draw(context, overlay_target, m_mask_rt.srv(), width, height, states);
    }
    else
    {
        REX::W32::ID3D11ShaderResourceView* const empty_srvs[3]{};
        context->PSSetShaderResources(0, 3, empty_srvs);
        context->OMSetRenderTargets(1, &mask_rtv, nullptr);
        context->ClearRenderTargetView(mask_rtv, Mask_Clear_Color);
        Glow::KernelProfile const profile = Glow::make_kernel_profile(cfg.outline_thickness);
        ROI::Viewport const viewport{ static_cast<int32_t>(width), static_cast<int32_t>(height) };
        // Collection appends all meshes of a target contiguously, in target order.
        std::span<MaskDraw const> const all_draws(draws);
        for (size_t begin = 0; begin < draws.size();)
        {
            size_t end = begin + 1;
            while (end < draws.size() && draws[end].target_index == draws[begin].target_index)
                ++end;
            std::span<MaskDraw const> const group = all_draws.subspan(begin, end - begin);
            ROI::Region const base_region = group_region(group, view_proj, m_geometry_pass, width, height);
            if (base_region.kind == ROI::RegionKind::e_empty)
            {
                begin = end;
                continue;
            }
            ROI::Region const horizontal_region = ROI::expand(base_region, profile.radius, true, false, viewport);
            ROI::Region const vertical_region = ROI::expand(base_region, profile.radius, true, true, viewport);
            if (horizontal_region.kind == ROI::RegionKind::e_empty || vertical_region.kind == ROI::RegionKind::e_empty)
            {
                begin = end;
                continue;
            }
            ROI::Rect const base_rect = base_region.kind == ROI::RegionKind::e_full
                                                  ? full_rect(width, height)
                                                  : base_region.rect;
            ROI::Rect const horizontal_rect = horizontal_region.kind == ROI::RegionKind::e_full
                                                        ? full_rect(width, height)
                                                        : horizontal_region.rect;
            ROI::Rect const vertical_rect = vertical_region.kind == ROI::RegionKind::e_full
                                                      ? full_rect(width, height)
                                                      : vertical_region.rect;
            REX::W32::D3D11_RECT const geometry_scissor{
                base_rect.left, base_rect.top, base_rect.right, base_rect.bottom };
            draw_mask(group, false, states.cull_none_scissor(), &geometry_scissor);
            if (!m_outline_pass.draw(device, context, overlay_target, m_mask_rt.srv(), width, height,
                    draws[begin].target_index + 1, profile.thickness, horizontal_rect, vertical_rect, states))
                break;
            begin = end;
        }
    }
    
    capture.restore();
}

void MaskOverlay::end_frame()
{

}
MASK_NAMESPACE_END
