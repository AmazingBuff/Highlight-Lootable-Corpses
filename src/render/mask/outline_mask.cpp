//
// Created by AmazingBuff on 2026/9/13.
//

#include "outline_mask.h"

#include "config/config.h"
#include "render/dx11/d3d11_util.h"
#include "mask_geometry.h"
#include "mask_passes.h"

#include <algorithm>

PLUGIN_NAMESPACE_BEGIN

namespace
{

    // ---------------------------------------------------------------------------
    // 门面状态（渲染线程独占；目标列表受互斥锁保护）
    // ---------------------------------------------------------------------------
    std::vector<Mask::MaskTarget> g_targets;

    Mask::RenderTarget g_mask_rt;
    Mask::MaskGeometryPass g_geometry_pass;
    Mask::SilhouettePass g_silhouette_pass;
    Mask::OutlinePass g_outline_pass;

    void render_impl(ID3D11Device* device, ID3D11DeviceContext* context, RE::NiCamera* camera, std::uint32_t width, std::uint32_t height)
    {
        if (!device || !context || !camera || width == 0 || height == 0)
            return;

        const std::vector<Mask::MaskTarget> targets = g_targets;
        if (targets.empty())
        {
            if (g_mask_rt.rtv())
                context->ClearRenderTargetView(g_mask_rt.rtv(), Mask::Mask_Clear_Color);
            return;
        }

        // ---- RT 重建（尺寸或设备变化）时：InputLayout 缓存一并清空（与原
        // release_mask_target 行为一致）；设备变化时管线对象全部重建（与原
        // release_pipeline 行为一致）。----
        if (!g_mask_rt.matches(device, width, height))
        {
            if (g_mask_rt.device() && g_mask_rt.device() != device)
            {
                g_geometry_pass.release();
                g_silhouette_pass.release();
                g_outline_pass.release();
            }
            // InputLayout 缓存（设备对象）随 RT 重建一并清空（原 release_mask_target 行为）
            g_geometry_pass.release_layouts();

            if (!g_mask_rt.init(device, width, height))
                return;
        }

        if (!g_geometry_pass.init(device))
            return;

        // 消费 pass 对象惰性创建（失败只禁对应叠加并一次性 WARN，不影响 mask 渲染；
        // 原实现在 ensure_mask_pipeline 内一次性创建，语义相同）。
        (void)g_silhouette_pass.ensure(device);
        (void)g_outline_pass.ensure(device);

        std::vector<Mask::MaskDraw> draws;
        collect_mask_draws(targets, draws);
        if (draws.empty())
        {
            context->ClearRenderTargetView(g_mask_rt.rtv(), Mask::Mask_Clear_Color);
            return;
        }

        DirectX::XMFLOAT4X4 view_proj{};
        float const (&world_to_cam)[4][4] = camera->GetRuntimeData().worldToCam;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                view_proj.m[row][col] = world_to_cam[row][col];
        g_geometry_pass.calibrate_upload_orientation(camera, view_proj, draws, width, height);

        // ---- 保存游戏渲染状态（本类触及的完整管线段）----
        D3D11StateCapture capture(context);
        capture.capture();

        // ---- mask pass：一次清屏，逐 draw 绘制全部目标；每个 draw 经 per-draw CB
        // 携带尸体索引，mask PS 写入 B 通道（MAX 混合在重叠区取较高索引）----
        ID3D11RenderTargetView* mask_rtv = g_mask_rt.rtv();
        context->OMSetRenderTargets(1, &mask_rtv, nullptr);
        context->ClearRenderTargetView(mask_rtv, Mask::Mask_Clear_Color);
        context->OMSetBlendState(g_geometry_pass.mask_write_blend(), nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(g_geometry_pass.depth_none(), 0);
        context->RSSetState(g_geometry_pass.cull_none());

        D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(g_mask_rt.width()), static_cast<float>(g_mask_rt.height()), 0.0f, 1.0f };
        context->RSSetViewports(1, &vp);

        g_geometry_pass.render(device, context, view_proj, draws);

        // ---- 显示模式门控：silhouette=内部填充叠加（draw_silhouette）、outline=外
        // 描边带（draw_outline），各只调用一次；icon 模式两 pass 均不画（防御：正常
        // 路径 renderer 在 icon 模式不会调用本类）。叠加目标是 Present 时刻的渲染
        // 目标。----
        // per-frame alpha LUT（与目标快照同序，其余槽位 0），消费 PS 以 mask B 通道
        // 的尸体索引查表——单次消费无复合，重叠像素取较高索引尸体的 alpha（常量），
        // 每具尸体各部位颜色一致。
        float alpha_lut[Mask::Alpha_Lut_Floats]{};
        std::size_t const lut_count = std::min<std::size_t>(targets.size(), Mask::Alpha_Lut_Floats);
        for (std::size_t i = 0; i < lut_count; ++i)
            alpha_lut[i] = targets[i].opacity;

        Config const& cfg = Setting::get_config();
        Color color;
        color.decode(cfg.outline_color);
        ID3D11RenderTargetView* overlay_target = capture.render_target();
        bool drew_consumer = false;
        if (cfg.display_mode == Config::DisplayMode::e_silhouette && g_silhouette_pass.ready())
        {
            g_silhouette_pass.update_alpha_lut(context, alpha_lut);
            drew_consumer = g_silhouette_pass.draw(context, overlay_target, g_mask_rt.srv(), width, height, color,
                g_geometry_pass.depth_none(), g_geometry_pass.cull_none());
        }
        else if (cfg.display_mode == Config::DisplayMode::e_outline && g_outline_pass.ready())
        {
            g_outline_pass.update_alpha_lut(context, alpha_lut);
            drew_consumer = g_outline_pass.draw(context, overlay_target, g_mask_rt.srv(), width, height, color, cfg.outline_thickness,
                g_geometry_pass.depth_none(), g_geometry_pass.cull_none());
        }
        static bool s_consumer_skip_logged = false;
        if (!drew_consumer && !s_consumer_skip_logged)
        {
            s_consumer_skip_logged = true;
            logger::warn(
                "outline mask: consumer pass not drawn: mode={} silhouette_ready={} outline_ready={} (targets={})",
                static_cast<int>(cfg.display_mode), g_silhouette_pass.ready(), g_outline_pass.ready(), targets.size());
        }

        capture.restore();
    }
}

void OutlineMask::set_targets(std::vector<OutlineMaskTarget> const& targets)
{
    std::vector<Mask::MaskTarget> kept;
    kept.reserve(targets.size());
    for (OutlineMaskTarget const& target : targets)
    {
        if (target.ref)
            kept.push_back(Mask::MaskTarget{ RE::NiPointer<RE::TESObjectREFR>(target.ref), target.opacity });  // NiPointer 构造即保活
    }

    g_targets = std::move(kept);
}

void OutlineMask::render(ID3D11Device* device, ID3D11DeviceContext* context, RE::NiCamera* camera, std::uint32_t width, std::uint32_t height)
{
    // Present 回调边界内禁止异常外泄：任何未预期失败记日志并跳过本帧
    try
    {
        render_impl(device, context, camera, width, height);
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

PLUGIN_NAMESPACE_END
