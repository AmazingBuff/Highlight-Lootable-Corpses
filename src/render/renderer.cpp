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
#include "render_util.h"

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
    constexpr float Hold_Fraction = 0.10f;

    float pulse_alpha(std::uint32_t a_elapsed_ms, std::uint32_t a_duration_ms)
    {
        if (a_duration_ms == 0 || a_elapsed_ms >= a_duration_ms)
            return 0.0f;

        float const progress = static_cast<float>(a_elapsed_ms) / static_cast<float>(a_duration_ms);
        return progress <= Hold_Fraction ? 1.0f : std::clamp(1.0f - (progress - Hold_Fraction) / (1.0f - Hold_Fraction), 0.0f, 1.0f);
    }

    float corpse_alpha(Config const& a_cfg, float a_distance, float a_alpha)
    {
        float const fade_range = std::max(a_cfg.max_distance - a_cfg.fade_start_distance, 1.0f);
        float const f = a_distance <= a_cfg.fade_start_distance ? 1.0f : 1.0f - std::clamp((a_distance - a_cfg.fade_start_distance) / fade_range, 0.0f, 1.0f);
        float const fade = std::pow(f, a_cfg.fade_power);
        return std::clamp(a_cfg.min_opacity + fade * a_alpha * (1.0f - a_cfg.min_opacity), a_cfg.min_opacity, 1.0f);
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
        OverlayDirector() : m_scan_in_flight(false), m_last_drawn_frame(std::numeric_limits<std::uint32_t>::max()) {}

        // 定时派发尸体扫描任务到游戏线程；在途守卫保证同一时刻最多一个扫描任务
        // 排队或执行中，避免扫描堆积（exchange 置位成功才派发）。
        void schedule_scan()
        {
            std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
            if (now - m_last_scan >= std::chrono::milliseconds(Setting::get_config().scan_interval_ms) &&
                !m_scan_in_flight.exchange(true))
            {
                m_last_scan = now;
                SKSE::GetTaskInterface()->AddTask([this]
                {
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
                RE::BSGraphics::ScreenSize const screen = RE::BSGraphics::Renderer::GetScreenSize();
                w = static_cast<float>(screen.width);
                h = static_cast<float>(screen.height);
            }
            if (w <= 0.0f || h <= 0.0f)
                return;

            D3D11StateCapture const capture(a_context);
            ID3D11RenderTargetView* back_rtv = m_back_buffer.rtv();
            a_context->OMSetRenderTargets(1, &back_rtv, nullptr);
            a_context->OMSetBlendState(m_states.alpha_blend(), nullptr, 0xFFFFFFFF);
            a_context->OMSetDepthStencilState(m_states.depth_none(), 0);
            a_context->RSSetState(m_states.cull_none());

            Config const& cfg = Setting::get_config();

            bool const pulse_mode = cfg.hotkey_mode == Config::HotkeyMode::e_pulse;
            bool const pulse_active = pulse_mode && cfg.enabled && PulseHighlight::active();
            if (pulse_mode && !pulse_active)
                return;

            if (pulse_active || cfg.enabled)
            {
                std::vector<CorpseScan::CorpseInfo> const corpses = CorpseScan::snapshot();
                float const pulse = pulse_active ? pulse_alpha(PulseHighlight::elapsed_ms(), PulseHighlight::duration_ms()) : 1.0f;
                if (pulse > 0.f && !corpses.empty())
                {
                    m_projector.refresh();
                    Color color;
                    color.decode(cfg.outline_color);
                    if (cfg.display_mode == Config::DisplayMode::e_icon)
                    {
                        m_ui.begin_frame(w, h);
                        for (CorpseScan::CorpseInfo const& corpse : corpses)
                        {
                            float px = 0.0f, py = 0.0f, d = 0.0f;
                            if (!m_projector.world_to_screen(corpse.anchor, w, h, px, py, d))
                                continue;

                            float const alpha = pulse * corpse_alpha(cfg, corpse.distance, color.a());
                            m_ui.add_circle(px, py, Icon_Radius, { color.r(), color.g(), color.b(), alpha });
                        }
                    }
                    else
                    {
                        std::vector<OutlineMaskTarget> mask_targets;
                        mask_targets.reserve(corpses.size());
                        for (CorpseScan::CorpseInfo const& corpse : corpses)
                        {
                            if (RE::TESForm* form = RE::TESForm::LookupByID(corpse.form_id))
                            {
                                if (RE::TESObjectREFR* ref = form->AsReference())
                                    mask_targets.emplace_back(ref, pulse * corpse_alpha(cfg, corpse.distance, color.a()));
                            }
                        }
                        OutlineMask::set_targets(mask_targets);
                        OutlineMask::render(a_device, a_context, m_projector.camera(), m_back_buffer.width(), m_back_buffer.height());
                    }
                }
            }

            m_ui.flush(a_context, m_back_buffer.rtv(), m_states);
        }

        // ---- 扫描调度（渲染线程计时，游戏线程执行）----
        std::chrono::steady_clock::time_point m_last_scan;
        std::atomic<bool> m_scan_in_flight;

        uint32_t m_last_drawn_frame;
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
