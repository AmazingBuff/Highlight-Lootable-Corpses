//
// Created by AmazingBuff on 2026/9/13.
//

#include "renderer.h"

#include "back_buffer_target.h"
#include "d3d11_util.h"
#include "outline_mask.h"
#include "present_hook.h"
#include "screen_projector.h"
#include "ui_overlay.h"

#include "config/config.h"
#include "input/pulse_highlight.h"
#include "search/corpse_finder.h"
#include "search/searched_corpses.h"

#include <RE/Skyrim.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    // icon 模式的实心小圆：半径固定像素值，不做距离缩放。
    constexpr float Icon_Radius = 10.0f;

    // 脉冲渐隐系数：脉冲期起始 10% 保持完全不透明（"highlight"段），其余时间线性
    // 渐隐到 0（"渐渐变淡直至消失"）。elapsed 超过 duration 后返回 0。
    float pulse_alpha(std::uint32_t a_elapsed_ms, std::uint32_t a_duration_ms)
    {
        if (a_duration_ms == 0 || a_elapsed_ms >= a_duration_ms)
            return 0.0f;

        float const progress = static_cast<float>(a_elapsed_ms) / static_cast<float>(a_duration_ms);
        constexpr float Hold_Fraction = 0.10f;
        return progress <= Hold_Fraction ? 1.0f : std::clamp(1.0f - (progress - Hold_Fraction) / (1.0f - Hold_Fraction), 0.0f, 1.0f);
    }

    // 距离衰减：FadeStartDistance 内完全不透明；超过后按 FadePower 指数衰减，
    // 到 MaxDistance 处达到 MinOpacity 下限。
    // 前置条件：min_opacity ∈ [0,1]、max_distance > 0（Config::load 已规范化）。
    float corpse_alpha(Config const& a_cfg, float a_distance)
    {
        float const fade_range = std::max(a_cfg.max_distance - a_cfg.fade_start_distance, 1.0f);
        float const f = a_distance <= a_cfg.fade_start_distance ? 1.0f : 1.0f - std::clamp((a_distance - a_cfg.fade_start_distance) / fade_range, 0.0f, 1.0f);
        float const fade = std::pow(f, a_cfg.fade_power);
        return std::clamp(a_cfg.min_opacity + fade * (1.0f - a_cfg.min_opacity), a_cfg.min_opacity, 1.0f);
    }

    // ---------------------------------------------------------------------------
    // 渲染协调器（composition root）：扫描调度、帧去重、状态管理与模式派发。
    // 绘制互斥：日志实测 on_present 会被多个线程并发进入（Present hook 触发线程
    // 与游戏渲染线程交替），整个绘制段串行化。
    // ---------------------------------------------------------------------------
    class OverlayDirector
    {
    public:
        static OverlayDirector& instance()
        {
            static OverlayDirector s_instance;
            return s_instance;
        }

        void on_present(IDXGISwapChain* a_swap_chain)
        {
            std::lock_guard<std::mutex> const draw_lock(m_draw_mutex);

            schedule_scan();

            RE::BSGraphics::Renderer* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!renderer)
                return;

            RE::BSGraphics::RendererData& rt = renderer->GetRuntimeData();
            ID3D11Device* device = reinterpret_cast<ID3D11Device*>(rt.forwarder);
            ID3D11DeviceContext* context = reinterpret_cast<ID3D11DeviceContext*>(rt.context);
            if (!device || !context)
                return;

            RE::BSGraphics::State* bs_state = RE::BSGraphics::State::GetSingleton();
            std::uint32_t const frame = bs_state ? bs_state->GetFrameCount() : 0;

            bool const skip_draw = (frame != 0) && (frame == m_last_drawn_frame);
            if (!skip_draw)
            {
                if (frame != 0)
                    m_last_drawn_frame = frame;

                draw_esp(a_swap_chain, device, context);
            }
        }

    private:
        OverlayDirector() = default;

        // 定时派发尸体扫描任务到游戏线程；在途守卫保证同一时刻最多一个扫描任务
        // 排队或执行中，避免扫描堆积（exchange 置位成功才派发）。
        void schedule_scan()
        {
            std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
            if (now - m_last_scan >= std::chrono::milliseconds(Setting::get_config().scan_interval_ms) &&
                !m_scan_in_flight.exchange(true))
            {
                m_last_scan = now;
                SKSE::GetTaskInterface()->AddTask([this] {
                    CorpseScan::search();
                    m_scan_in_flight.store(false);
                });
            }
        }

        void draw_esp(IDXGISwapChain* a_swap_chain, ID3D11Device* a_device, ID3D11DeviceContext* a_context)
        {
            if (!m_back_buffer.ensure(a_swap_chain, a_device))
                return;

            m_states.ensure(a_device);
            m_ui.ensure(a_device);
            if (!m_states.ready() || !m_ui.ready())
                return;

            float w = static_cast<float>(m_back_buffer.width());
            float h = static_cast<float>(m_back_buffer.height());
            if (w <= 0.0f || h <= 0.0f)
            {
                // 后备：渲染器报告的屏幕尺寸
                RE::BSGraphics::ScreenSize const screen = RE::BSGraphics::Renderer::GetScreenSize();
                w = static_cast<float>(screen.width);
                h = static_cast<float>(screen.height);
            }
            if (w <= 0.0f || h <= 0.0f)
                return;

            // ---- 保存游戏渲染状态；随后绑定我们的绘制状态 ----
            D3D11StateCapture const capture(a_context);
            ID3D11RenderTargetView* back_rtv = m_back_buffer.rtv();
            a_context->OMSetRenderTargets(1, &back_rtv, nullptr);
            a_context->OMSetBlendState(m_states.alpha_blend(), nullptr, 0xFFFFFFFF);
            a_context->OMSetDepthStencilState(m_states.depth_none(), 0);
            a_context->RSSetState(m_states.cull_none());

            Config const& cfg = Setting::get_config();

            // 消退模式：enable 是脉冲模式的前提——enable=true 时显示只由脉冲驱动
            //（在途画渐隐，过期不画），热键触发脉冲；enable=false 一切不画。
            bool const pulse_mode = cfg.hotkey_mode == Config::HotkeyMode::e_pulse;
            bool const pulse_active = pulse_mode && cfg.enabled && PulseHighlight::active();
            if (pulse_mode && !pulse_active)
                return;  // 脉冲模式无在途脉冲，无可见路径

            if (pulse_active || cfg.enabled)
            {
                m_projector.refresh();
                RgbColor const color = RgbColor::decode(cfg.outline_color);
                std::vector<CorpseScan::CorpseInfo> const corpses = CorpseScan::snapshot();

                // 逐 corpse 的显示系数（0 = 本帧不画）。常亮模式下恒 1（走既有
                // corpse_alpha 距离衰减）；脉冲模式下：
                //  - highlight 的目标即 snapshot 中的尸体，不做二次过滤；
                //  - 渐隐途中尸体被玩家搜索（MarkCorpse 标记）→ 立即取消该尸体的
                //    highlight（系数置 0）；
                //  - 时长取 trigger 时快照值，中途改配置只影响下次脉冲。
                auto corpse_display_alpha = [&](CorpseScan::CorpseInfo const& a_corpse) -> float {
                    if (!pulse_active)
                        return 1.0f;

                    RE::TESForm* const form = RE::TESForm::LookupByID(a_corpse.form_id);
                    RE::TESObjectREFR* const ref = form ? form->AsReference() : nullptr;
                    if (ref && MarkCorpse::contains(ref))
                        return 0.0f;

                    std::uint32_t const duration = PulseHighlight::duration_ms();
                    std::uint32_t const elapsed = PulseHighlight::elapsed_ms();
                    return pulse_alpha(elapsed, duration);
                };

                // ---- icon 模式：每 corpse 只投影固定世界锚点（bounds/worldBound 中心），
                // 投影随视角平滑连续。silhouette/outline 模式由 mask pass 负责显示，
                // 无逐 corpse 工作。----
                if (cfg.display_mode == Config::DisplayMode::e_icon)
                {
                    m_ui.begin_frame(w, h);
                    for (CorpseScan::CorpseInfo const& corpse : corpses)
                    {
                        float const pulse = corpse_display_alpha(corpse);
                        if (pulse <= 0.0f)
                            continue;

                        float px = 0.0f, py = 0.0f, d = 0.0f;
                        if (!m_projector.world_to_screen(corpse.anchor, w, h, px, py, d))
                            continue;  // 相机后方/投影失败 → 跳过该 corpse

                        float const alpha = pulse * corpse_alpha(cfg, corpse.distance);
                        m_ui.add_circle(px, py, Icon_Radius, { color.r, color.g, color.b, alpha });
                    }
                }
                // ---- mask 渲染（穿墙剪影/描边带的输入）：目标由本帧尸体快照的
                // form_id 解析为引用。icon 模式下 mask 无消费者，整段跳过。----
                else if (!corpses.empty())
                {
                    // 目标携带距离衰减不透明度（corpse_alpha），按目标序号填入消费
                    // pass 的 per-frame alpha LUT。脉冲模式叠加渐隐系数，渐隐途中
                    // 被搜索的尸体系数为 0，不进目标列表（立即取消 highlight）。
                    std::vector<OutlineMaskTarget> mask_targets;
                    mask_targets.reserve(corpses.size());
                    for (CorpseScan::CorpseInfo const& corpse : corpses)
                    {
                        float const pulse = corpse_display_alpha(corpse);
                        if (pulse <= 0.0f)
                            continue;

                        if (RE::TESForm* form = RE::TESForm::LookupByID(corpse.form_id))
                        {
                            if (RE::TESObjectREFR* ref = form->AsReference())
                                mask_targets.push_back({ ref, pulse * corpse_alpha(cfg, corpse.distance) });
                        }
                    }
                    OutlineMask::set_targets(mask_targets);
                    OutlineMask::render(a_device, a_context, m_projector.camera(), m_back_buffer.width(), m_back_buffer.height());
                }
            }

            m_ui.flush(a_context, m_back_buffer.rtv(), m_states);
        }

        // ---- 扫描调度（渲染线程计时，游戏线程执行）----
        std::chrono::steady_clock::time_point m_last_scan;
        std::atomic<bool> m_scan_in_flight{ false };

        std::uint32_t m_last_drawn_frame = std::numeric_limits<std::uint32_t>::max();
        std::mutex m_draw_mutex;

        BackBufferTarget m_back_buffer;
        OverlayStates m_states;
        UiOverlay m_ui;
        ScreenProjector m_projector;
    };

    void STDMETHODCALLTYPE present_callback(IDXGISwapChain* a_swap_chain)
    {
        OverlayDirector::instance().on_present(a_swap_chain);
    }
}

void Renderer::install()
{
    (void)PresentHook::install(&present_callback);
}

PLUGIN_NAMESPACE_END
