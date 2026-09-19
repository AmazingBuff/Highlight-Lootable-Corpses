//
// Created by AmazingBuff on 2026/9/13.
//

#include "mask_overlay.h"

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
        uint32_t width,
        uint32_t height)
    {
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


MaskOverlay::MaskOverlay() : m_ref_device(nullptr), m_mask_rt(REX::W32::DXGI_FORMAT_R32_UINT, REX::W32::DXGI_FORMAT_D32_FLOAT), m_width(0), m_height(0), m_pass_ready(false), m_rt_ready(false) {}

MaskOverlay::~MaskOverlay() = default;

bool MaskOverlay::init(REX::W32::ID3D11Device* device)
{
    if (m_pass_ready)
        return true;
    
    const bool geometry_ready = m_geometry_pass.init(device);
    const bool silhouette_ready = m_silhouette_pass.init(device);
    const bool outline_ready = m_outline_pass.init(device);
    
    m_pass_ready = geometry_ready && silhouette_ready && outline_ready;
    m_ref_device = device;
    return m_pass_ready;
}

bool MaskOverlay::begin_frame(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height)
{
    RenderTarget& scratch_render_target = m_outline_pass.scratch_render_target();

    if (m_rt_ready)
    {
        if (!m_mask_rt.matches(device, width, height))
        {
            m_mask_rt.release();
            scratch_render_target.release();

            m_geometry_pass.release();
            m_silhouette_pass.release();
            m_outline_pass.release();

            m_pass_ready = init(device);
            m_rt_ready = m_mask_rt.init(m_ref_device, width, height) && scratch_render_target.init(m_ref_device, width, height);
        }
    }
    else
        m_rt_ready = m_mask_rt.init(m_ref_device, width, height) && scratch_render_target.init(m_ref_device, width, height);

    return m_pass_ready && m_rt_ready;
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

    REX::W32::D3D11_VIEWPORT const viewport{
        .topLeftX = 0.0f, .topLeftY = 0.0f,
        .width = static_cast<float>(width),
        .height = static_cast<float>(height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };

    if (silhouette)
        draw_silhouette(device, context, overlay_target, draws, view_proj, viewport, states);
    else
        draw_outline(device, context, overlay_target, draws, view_proj, viewport, states);
    
    capture.restore();
}

void MaskOverlay::end_frame()
{

}

void MaskOverlay::draw_silhouette(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11RenderTargetView* overlay_target, std::span<MaskDraw const> group,
    DirectX::XMFLOAT4X4 const& view_proj, REX::W32::D3D11_VIEWPORT const& viewport, CommonStates const& states)
{
    REX::W32::ID3D11RenderTargetView* mask_rtv = m_mask_rt.rtv();
    context->OMSetRenderTargets(1, &mask_rtv, m_mask_rt.dsv());
    context->ClearRenderTargetView(mask_rtv, Mask_Clear_Color);
    context->ClearDepthStencilView(m_mask_rt.dsv(), REX::W32::D3D11_CLEAR_DEPTH, 0.0f, 0);

    context->OMSetBlendState(states.opaque(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_geometry_pass.depth_nearest(), 0);
    context->RSSetState(states.cull_none());
    context->RSSetViewports(1, &viewport);
    m_geometry_pass.draw(device, context, view_proj, group);

    context->OMSetRenderTargets(1, &overlay_target, nullptr);
    context->OMSetBlendState(states.non_premultiplied(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(states.depth_none(), 0);
    context->RSSetState(states.cull_none());
    context->RSSetViewports(1, &viewport);
    m_silhouette_pass.draw(context, m_mask_rt.srv());
}

void MaskOverlay::draw_outline(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11RenderTargetView* overlay_target, std::vector<MaskDraw> const& draws,
    DirectX::XMFLOAT4X4 const& view_proj, REX::W32::D3D11_VIEWPORT const& vp, CommonStates const& states)
{
    REX::W32::ID3D11RenderTargetView* mask_rtv = m_mask_rt.rtv();

    context->OMSetDepthStencilState(states.depth_none(), 0);
    context->RSSetState(states.cull_none_scissor());
    context->RSSetViewports(1, &vp);

    ROI::Viewport const viewport{
        .width = static_cast<int32_t>(vp.width),
        .height = static_cast<int32_t>(vp.height)
    };

    Glow::KernelProfile const profile = Glow::make_kernel_profile(Setting::instance().get_config().outline_thickness);
    // Collection appends all meshes of a target contiguously, in target order.
    std::span<MaskDraw const> const all_draws(draws);
    for (size_t begin = 0; begin < draws.size();)
    {
        size_t end = begin + 1;
        while (end < draws.size() && draws[end].target_index == draws[begin].target_index)
            ++end;
        std::span<MaskDraw const> const group = all_draws.subspan(begin, end - begin);
        ROI::Region const base_region = group_region(group, view_proj, static_cast<uint32_t>(viewport.width), static_cast<uint32_t>(viewport.height));
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
        ROI::Rect const base_rect = base_region.kind == ROI::RegionKind::e_full ?
            full_rect(viewport.width, viewport.height) : base_region.rect;
        ROI::Rect const horizontal_rect = horizontal_region.kind == ROI::RegionKind::e_full ?
            full_rect(viewport.width, viewport.height) : horizontal_region.rect;
        ROI::Rect const vertical_rect = vertical_region.kind == ROI::RegionKind::e_full ?
            full_rect(viewport.width, viewport.height) : vertical_region.rect;

        REX::W32::D3D11_RECT const geometry_scissor{
            base_rect.left,
            base_rect.top,
            base_rect.right,
            base_rect.bottom
        };
        context->RSSetScissorRects(1, &geometry_scissor);

        context->OMSetRenderTargets(1, &mask_rtv, nullptr);
        context->ClearRenderTargetView(mask_rtv, Mask_Clear_Color);

        context->OMSetBlendState(states.opaque(), nullptr, 0xFFFFFFFF);
        m_geometry_pass.draw(device, context, view_proj, group);

        if (!m_outline_pass.draw(context, overlay_target, m_mask_rt.srv(), vp, draws[begin].target_index + 1, profile, horizontal_rect, vertical_rect, states))
            break;
        begin = end;
    }
}
MASK_NAMESPACE_END
