//
// Created by AmazingBuff on 2026/9/13.
//

#include "outline_mask.h"

#include "mask_depth.h"
#include "mask_geometry.h"
#include "mask_passes.h"

#include "config/config.h"
#include "render/dx11/common_states.h"
#include "render/dx11/d3d11_util.h"

PLUGIN_NAMESPACE_BEGIN

namespace
{
    [[nodiscard]] Mask::ROI::Region group_region(
        std::span<Mask::MaskDraw const> group,
        DirectX::XMFLOAT4X4 const& view_proj,
        Mask::MaskGeometryPass const& geometry_pass,
        uint32_t width,
        uint32_t height)
    {
        if (geometry_pass.upload_transposed())
            return Mask::ROI::full_region();

        std::vector<Mask::ROI::Sphere> spheres;
        spheres.reserve(group.size());
        for (Mask::MaskDraw const& draw : group)
        {
            if (!draw.node)
                return Mask::ROI::full_region();
            RE::NiBound const& bound = draw.node->worldBound;
            spheres.push_back({
                .center_x = static_cast<double>(bound.center.x),
                .center_y = static_cast<double>(bound.center.y),
                .center_z = static_cast<double>(bound.center.z),
                .radius = static_cast<double>(bound.radius),
            });
        }
        return Mask::ROI::make_region(spheres, view_proj,
            { static_cast<int32_t>(width), static_cast<int32_t>(height) });
    }

    [[nodiscard]] Mask::ROI::Rect full_rect(uint32_t width, uint32_t height) noexcept
    {
        return { 0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height) };
    }
}

OutlineMask& OutlineMask::instance()
{
    static OutlineMask s_instance;
    return s_instance;
}

void OutlineMask::render_impl(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context, RE::NiCamera* camera,
    REX::W32::ID3D11RenderTargetView* overlay_target, uint32_t width, uint32_t height, CommonStates const& states)
{
        if (!device || !context || !camera || !overlay_target || width == 0 || height == 0)
        return;

    std::vector<Mask::MaskTarget> const& targets = m_targets;
    Config const& cfg = Setting::instance().get_config();
    bool const silhouette = cfg.display_mode == Config::DisplayMode::e_silhouette;
    if (targets.empty() || (!silhouette && cfg.display_mode != Config::DisplayMode::e_outline))
        return;

    if (!m_mask_rt.matches(device, width, height))
    {
        m_geometry_pass.release();
        m_silhouette_pass.release();
        m_outline_pass.release();

        m_mask_rt.release();
        if (!m_mask_rt.init(device, width, height))
            return;
    }
    if (!m_geometry_pass.init(device))
        return;
    if (silhouette ? !m_silhouette_pass.init(device) : !m_outline_pass.init(device))
        return;

    std::vector<Mask::MaskDraw> draws;
    collect_mask_draws(targets, draws);
    if (draws.empty())
        return;

    DirectX::XMFLOAT4X4 view_proj{};
    float const (&world_to_cam)[4][4] = camera->GetRuntimeData().worldToCam;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            view_proj.m[row][col] = world_to_cam[row][col];
    m_geometry_pass.calibrate_upload_orientation(camera, view_proj, draws, width, height);
    RE::NiFrustum const& frustum = camera->GetRuntimeData2().viewFrustum;
    if (!Mask::make_private_depth_projection(view_proj, camera->GetNearPlane(), frustum.bOrtho))
    {
        static bool s_invalid_camera_logged = false;
        if (!s_invalid_camera_logged)
        {
            logger::warn("outline mask: invalid or orthographic camera; frame skipped");
            s_invalid_camera_logged = true;
        }
        return;
    }
    Mask::FullscreenPass& consumer = silhouette ? static_cast<Mask::FullscreenPass&>(m_silhouette_pass)
                                                : static_cast<Mask::FullscreenPass&>(m_outline_pass);
    if (!consumer.update_styles(device, context, targets))
    {
        static bool s_style_failure_logged = false;
        if (!s_style_failure_logged)
        {
            logger::warn("outline mask: style table upload failed; frame skipped");
            s_style_failure_logged = true;
        }
        return;
    }

    D3D11StateCapture capture(context);
    capture.capture();
    struct RestoreState
    {
        D3D11StateCapture& capture;
        ~RestoreState() { capture.restore(); }
    } restore{ capture };
    REX::W32::D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    REX::W32::ID3D11RenderTargetView* mask_rtv = m_mask_rt.rtv();
    auto const draw_mask = [&](std::span<Mask::MaskDraw const> group, bool clear_mask,
        REX::W32::ID3D11RasterizerState* rasterizer, REX::W32::D3D11_RECT const* scissor) {
        REX::W32::ID3D11ShaderResourceView* const empty_srvs[3]{};
        context->PSSetShaderResources(0, 3, empty_srvs);
        context->OMSetRenderTargets(1, &mask_rtv, silhouette ? m_mask_rt.dsv() : nullptr);
        if (clear_mask)
            context->ClearRenderTargetView(mask_rtv, Mask::Mask_Clear_Color);
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
        context->ClearRenderTargetView(mask_rtv, Mask::Mask_Clear_Color);
        Mask::Glow::KernelProfile const profile = Mask::Glow::make_kernel_profile(cfg.outline_thickness);
        Mask::ROI::Viewport const viewport{ static_cast<int32_t>(width), static_cast<int32_t>(height) };
        // Collection appends all meshes of a target contiguously, in target order.
        std::span<Mask::MaskDraw const> const all_draws(draws);
        for (size_t begin = 0; begin < draws.size();)
        {
            size_t end = begin + 1;
            while (end < draws.size() && draws[end].target_index == draws[begin].target_index)
                ++end;
            std::span<Mask::MaskDraw const> const group = all_draws.subspan(begin, end - begin);
            Mask::ROI::Region const base_region = group_region(group, view_proj, m_geometry_pass, width, height);
            if (base_region.kind == Mask::ROI::RegionKind::e_empty)
            {
                begin = end;
                continue;
            }
            Mask::ROI::Region const horizontal_region = Mask::ROI::expand(base_region, profile.radius, true, false, viewport);
            Mask::ROI::Region const vertical_region = Mask::ROI::expand(base_region, profile.radius, true, true, viewport);
            if (horizontal_region.kind == Mask::ROI::RegionKind::e_empty || vertical_region.kind == Mask::ROI::RegionKind::e_empty)
            {
                begin = end;
                continue;
            }
            Mask::ROI::Rect const base_rect = base_region.kind == Mask::ROI::RegionKind::e_full
                                                  ? full_rect(width, height)
                                                  : base_region.rect;
            Mask::ROI::Rect const horizontal_rect = horizontal_region.kind == Mask::ROI::RegionKind::e_full
                                                        ? full_rect(width, height)
                                                        : horizontal_region.rect;
            Mask::ROI::Rect const vertical_rect = vertical_region.kind == Mask::ROI::RegionKind::e_full
                                                      ? full_rect(width, height)
                                                      : vertical_region.rect;
            REX::W32::D3D11_RECT const geometry_scissor{
                base_rect.left, base_rect.top, base_rect.right, base_rect.bottom };
            draw_mask(group, false, m_outline_pass.cull_none_scissor(), &geometry_scissor);
            if (!m_outline_pass.draw(device, context, overlay_target, m_mask_rt.srv(), width, height,
                    draws[begin].target_index + 1, profile.thickness, horizontal_rect, vertical_rect, states))
                break;
            begin = end;
        }
    }
}

void OutlineMask::set_targets(std::vector<OutlineMaskTarget> const& targets)
{
    std::vector<Mask::MaskTarget> kept;
    kept.reserve(targets.size());
    for (OutlineMaskTarget const& target : targets)
    {
        if (target.ref)
            kept.emplace_back(RE::NiPointer<RE::TESObjectREFR>(target.ref), target.color);
    }

    m_targets = std::move(kept);
}

void OutlineMask::render(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context, RE::NiCamera* camera,
    REX::W32::ID3D11RenderTargetView* overlay_target, uint32_t width, uint32_t height, CommonStates const& states)
{
    // No exception may escape the Present callback boundary: any unexpected failure is logged and the frame is skipped
    try
    {
        render_impl(device, context, camera, overlay_target, width, height, states);
    }
    catch (std::exception const& e)
    {
        logger::error("outline mask: frame skipped ({})", e.what());
    }
    catch (...)
    {
        logger::error("outline mask: frame skipped (unexpected error)");
    }
}

OutlineMask::OutlineMask() = default;

OutlineMask::~OutlineMask() = default;

PLUGIN_NAMESPACE_END
