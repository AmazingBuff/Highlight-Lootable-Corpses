//
// Created by AmazingBuff on 2026/9/13.
//

#include "outline_mask.h"

#include "config/config.h"
#include "d3d11_util.h"
#include "mask_geometry.h"
#include "mask_passes.h"

#include <RE/Skyrim.h>

#include <algorithm>
#include <mutex>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    // ---------------------------------------------------------------------------
    // 门面状态（渲染线程独占；目标列表受互斥锁保护）
    // ---------------------------------------------------------------------------
    std::mutex g_target_mutex;
    std::vector<MaskTarget> g_targets;

    MaskRenderTarget g_mask_rt;
    MaskGeometryPass g_geometry_pass;
    SilhouettePass g_silhouette_pass;
    OutlinePass g_outline_pass;

    void render_impl(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::NiCamera* a_camera, std::uint32_t a_width, std::uint32_t a_height)
    {
        if (!a_device || !a_context || !a_camera || a_width == 0 || a_height == 0)
            return;

        std::vector<MaskTarget> targets;
        {
            std::lock_guard<std::mutex> const lock(g_target_mutex);
            targets = g_targets;
        }

        if (targets.empty())
        {
            // 无目标：清掉旧 mask，避免消费 pass 读到陈旧内容
            if (g_mask_rt.rtv())
                a_context->ClearRenderTargetView(g_mask_rt.rtv(), Mask_Clear_Color);
            return;
        }

        // ---- RT 重建（尺寸或设备变化）时：InputLayout 缓存一并清空（与原
        // release_mask_target 行为一致）；设备变化时管线对象全部重建（与原
        // release_pipeline 行为一致）。----
        if (!g_mask_rt.matches(a_device, a_width, a_height))
        {
            if (g_mask_rt.device() && g_mask_rt.device() != a_device)
            {
                g_geometry_pass.release();
                g_silhouette_pass.release();
                g_outline_pass.release();
            }
            // InputLayout 缓存（设备对象）随 RT 重建一并清空（原 release_mask_target 行为）
            g_geometry_pass.release_layouts();
        }
        if (!g_mask_rt.ensure(a_device, a_width, a_height) || !g_geometry_pass.ensure(a_device))
            return;

        // 消费 pass 对象惰性创建（失败只禁对应叠加并一次性 WARN，不影响 mask 渲染；
        // 原实现在 ensure_mask_pipeline 内一次性创建，语义相同）。
        (void)g_silhouette_pass.ensure(a_device);
        (void)g_outline_pass.ensure(a_device);

        std::vector<MaskDraw> draws;
        collect_mask_draws(targets, draws);
        if (draws.empty())
        {
            a_context->ClearRenderTargetView(g_mask_rt.rtv(), Mask_Clear_Color);
            return;
        }

        MaskMat4 const view_proj = MaskMat4::from_world_to_cam_raw(a_camera->GetRuntimeData().worldToCam);
        g_geometry_pass.calibrate_upload_orientation(a_camera, view_proj, draws, a_width, a_height);

        // ---- 保存游戏渲染状态（本类触及的完整管线段）----
        D3D11StateCapture const capture(a_context);

        // ---- mask pass：一次清屏，逐 draw 绘制全部目标；每个 draw 经 per-draw CB
        // 携带尸体索引，mask PS 写入 B 通道（MAX 混合在重叠区取较高索引）----
        ID3D11RenderTargetView* mask_rtv = g_mask_rt.rtv();
        a_context->OMSetRenderTargets(1, &mask_rtv, nullptr);
        a_context->ClearRenderTargetView(mask_rtv, Mask_Clear_Color);
        a_context->OMSetBlendState(g_geometry_pass.mask_write_blend(), nullptr, 0xFFFFFFFF);
        a_context->OMSetDepthStencilState(g_geometry_pass.depth_none(), 0);
        a_context->RSSetState(g_geometry_pass.cull_none());

        D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(g_mask_rt.width()), static_cast<float>(g_mask_rt.height()), 0.0f, 1.0f };
        a_context->RSSetViewports(1, &vp);

        g_geometry_pass.render(a_device, a_context, view_proj, draws);

        // ---- 显示模式门控：silhouette=内部填充叠加（draw_silhouette）、outline=外
        // 描边带（draw_outline），各只调用一次；icon 模式两 pass 均不画（防御：正常
        // 路径 renderer 在 icon 模式不会调用本类）。叠加目标是 Present 时刻的渲染
        // 目标。----
        // per-frame alpha LUT（与目标快照同序，其余槽位 0），消费 PS 以 mask B 通道
        // 的尸体索引查表——单次消费无复合，重叠像素取较高索引尸体的 alpha（常量），
        // 每具尸体各部位颜色一致。
        float alpha_lut[Alpha_Lut_Floats]{};
        std::size_t const lut_count = std::min<std::size_t>(targets.size(), Alpha_Lut_Floats);
        for (std::size_t i = 0; i < lut_count; ++i)
            alpha_lut[i] = targets[i].opacity;

        Config const& cfg = Setting::get_config();
        RgbColor const color = RgbColor::decode(cfg.outline_color);
        ID3D11RenderTargetView* overlay_target = capture.render_target();
        bool drew_consumer = false;
        if (cfg.display_mode == Config::DisplayMode::e_silhouette && g_silhouette_pass.ready())
        {
            g_silhouette_pass.update_alpha_lut(a_context, alpha_lut);
            drew_consumer = g_silhouette_pass.draw(a_context, overlay_target, g_mask_rt.srv(), a_width, a_height, color,
                g_geometry_pass.depth_none(), g_geometry_pass.cull_none());
        }
        else if (cfg.display_mode == Config::DisplayMode::e_outline && g_outline_pass.ready())
        {
            g_outline_pass.update_alpha_lut(a_context, alpha_lut);
            drew_consumer = g_outline_pass.draw(a_context, overlay_target, g_mask_rt.srv(), a_width, a_height, color, cfg.outline_thickness,
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
    }
}

void OutlineMask::set_targets(std::vector<OutlineMaskTarget> const& a_targets)
{
    std::vector<MaskTarget> kept;
    kept.reserve(a_targets.size());
    for (OutlineMaskTarget const& target : a_targets)
    {
        if (target.ref)
            kept.push_back(MaskTarget{ RE::NiPointer<RE::TESObjectREFR>(target.ref), target.opacity });  // NiPointer 构造即保活
    }

    std::lock_guard<std::mutex> const lock(g_target_mutex);
    g_targets = std::move(kept);
}

void OutlineMask::render(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::NiCamera* a_camera, std::uint32_t a_width, std::uint32_t a_height)
{
    // Present 回调边界内禁止异常外泄：任何未预期失败记日志并跳过本帧
    try
    {
        render_impl(a_device, a_context, a_camera, a_width, a_height);
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
