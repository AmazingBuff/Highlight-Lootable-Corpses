//
// Created by AmazingBuff on 2026/9/13.
//

#include "renderer.h"

#include "back_buffer_target.h"
#include "d3d11_util.h"
#include "outline_mask.h"
#include "present_hook.h"
#include "render_util.h"
#include "icon/icon_overlay.h"

#include "config/config.h"
#include "input/pulse_highlight.h"
#include "search/corpse_finder.h"
#include "search/searched_corpses.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>

#include <CommonStates.h>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    constexpr float Hold_Fraction = 0.10f;

    float pulse_alpha(std::uint32_t elapsed_ms, std::uint32_t duration_ms)
    {
        if (duration_ms == 0 || elapsed_ms >= duration_ms)
            return 0.0f;

        float const progress = static_cast<float>(elapsed_ms) / static_cast<float>(duration_ms);
        return progress <= Hold_Fraction ? 1.0f : std::clamp(1.0f - (progress - Hold_Fraction) / (1.0f - Hold_Fraction), 0.0f, 1.0f);
    }

    float corpse_alpha(Config const& cfg, float distance, float alpha)
    {
        float const fade_range = std::max(cfg.max_distance - cfg.fade_start_distance, 1.0f);
        float const f = distance <= cfg.fade_start_distance ? 1.0f : 1.0f - std::clamp((distance - cfg.fade_start_distance) / fade_range, 0.0f, 1.0f);
        float const fade = std::pow(f, cfg.fade_power);
        return std::clamp(cfg.min_opacity + fade * alpha * (1.0f - cfg.min_opacity), cfg.min_opacity, 1.0f);
    }

    RE::BSGraphics::ViewData const* update_view_data(RE::NiCamera* camera)
    {
        RE::BSGraphics::ViewData const* view_data = nullptr;
        if (RE::BSGraphics::State* state = RE::BSGraphics::State::GetSingleton())
        {
            RE::BSGraphics::State::RUNTIME_DATA& state_rt = state->GetRuntimeData();
            for (RE::BSGraphics::CameraStateData const& cam_data : state_rt.cameraDataCacheA)
            {
                if (cam_data.referenceCamera == camera)
                {
                    view_data = std::addressof(cam_data.GetCameraStateRuntimeData().camViewData);
                    break;
                }
            }
            if (!view_data && !state_rt.cameraDataCacheA.empty())
                view_data = std::addressof(state_rt.cameraDataCacheA.front().GetCameraStateRuntimeData().camViewData);
        }
        return view_data;
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

        void on_present(IDXGISwapChain* swap_chain)
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

                draw(swap_chain, device, context);
            }
        }

    private:
        OverlayDirector() : m_scan_in_flight(false), m_last_drawn_frame(std::numeric_limits<std::uint32_t>::max()), m_ready(false) {}

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

        void draw(IDXGISwapChain* swap_chain, ID3D11Device* device, ID3D11DeviceContext* context)
        {
            if (!m_ready)
                refresh(swap_chain, device);

            if (!update_back_buffer(swap_chain, device))
                return;

            D3D11_TEXTURE2D_DESC desc{};
            m_back_buffer->GetDesc(&desc);
            
            if (!m_states || !m_icon_overlay.ready())
                return;

            float w = static_cast<float>(desc.Width);
            float h = static_cast<float>(desc.Height);
            if (w <= 0.0f || h <= 0.0f)
            {
                RE::BSGraphics::ScreenSize const screen = RE::BSGraphics::Renderer::GetScreenSize();
                w = static_cast<float>(screen.width);
                h = static_cast<float>(screen.height);
            }
            if (w <= 0.0f || h <= 0.0f)
                return;

            D3D11StateCapture const capture(context);
            context->OMSetRenderTargets(1, &m_render_target, nullptr);
            context->OMSetBlendState(m_states->AlphaBlend(), nullptr, 0xFFFFFFFF);
            context->OMSetDepthStencilState(m_states->DepthNone(), 0);
            context->RSSetState(m_states->CullNone());

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
                    RE::NiCamera* camera = RE::Main::WorldRootCamera();
                    RE::BSGraphics::ViewData const* view_data = update_view_data(camera);

                    Color color;
                    color.decode(cfg.outline_color);
                    if (cfg.display_mode == Config::DisplayMode::e_icon)
                    {
                        m_icon_overlay.begin_frame(w, h);
                        for (CorpseScan::CorpseInfo const& corpse : corpses)
                        {
                            float px = 0.0f, py = 0.0f, d = 0.0f;
                            if (!world_to_screen(camera, view_data, corpse.anchor, w, h, px, py, d))
                                continue;

                            float const alpha = pulse * corpse_alpha(cfg, corpse.distance, color.a());
                            m_icon_overlay.add_circle(px, py, static_cast<float>(cfg.icon_radius), { color.r(), color.g(), color.b(), alpha });
                        }
                        m_icon_overlay.draw(context, m_render_target, m_states);
                        m_icon_overlay.end_frame();
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
                        OutlineMask::render(device, context, camera, desc.Width, desc.Height);
                    }
                }
            }
        }

        void refresh(IDXGISwapChain* swap_chain, ID3D11Device* device)
        {
            m_states = std::make_shared<DirectX::DX11::CommonStates>(device);
            m_icon_overlay.init(device);

            ID3D11Texture2D* buffer = nullptr;
            HRESULT const hr = swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer));
            if (FAILED(hr) || !buffer)
                return;

            if (buffer == m_back_buffer)
            {
                buffer->Release();
                m_ready = true;
                return;
            }

            if (m_render_target)
            {
                m_render_target->Release();
                m_render_target = nullptr;
            }
            if (m_back_buffer)
            {
                m_back_buffer->Release();
                m_back_buffer = nullptr;
            }

            m_back_buffer = buffer;
            HRESULT const rtv_hr = device->CreateRenderTargetView(m_back_buffer, nullptr, &m_render_target);
            if (FAILED(rtv_hr) || !m_render_target)
            {
                logger::error("Failed to create backbuffer RTV: {:X}", static_cast<unsigned int>(rtv_hr));
                return;
            }

            m_ready = true;
        }

        bool update_back_buffer(IDXGISwapChain* swap_chain, ID3D11Device* device)
        {
            ID3D11Texture2D* buffer = nullptr;
            HRESULT const hr = swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer));
            if (FAILED(hr) || !buffer)
                return false;

            if (buffer == m_back_buffer)
            {
                buffer->Release();
                return true;
            }

            if (m_render_target)
            {
                m_render_target->Release();
                m_render_target = nullptr;
            }
            if (m_back_buffer)
            {
                m_back_buffer->Release();
                m_back_buffer = nullptr;
            }

            m_back_buffer = buffer;

            HRESULT const rtv_hr = device->CreateRenderTargetView(m_back_buffer, nullptr, &m_render_target);
            if (FAILED(rtv_hr) || !m_render_target)
            {
                logger::error("Failed to create backbuffer RTV: {:X}", static_cast<unsigned int>(rtv_hr));
                return false;
            }
            return true;
        }

    private:
        // ---- 扫描调度（渲染线程计时，游戏线程执行）----
        std::chrono::steady_clock::time_point m_last_scan;
        std::atomic<bool> m_scan_in_flight;

        uint32_t m_last_drawn_frame;
        std::mutex m_draw_mutex;

        ID3D11RenderTargetView* m_render_target;
        ID3D11Texture2D* m_back_buffer;
        std::shared_ptr<DirectX::DX11::CommonStates> m_states;
        IconOverlay m_icon_overlay;
        
        bool m_ready;
    };

    void STDMETHODCALLTYPE present_callback(IDXGISwapChain* swap_chain)
    {
        OverlayDirector::instance().on_present(swap_chain);
    }
}

void Renderer::install()
{
    (void)PresentHook::install(&present_callback);
}

PLUGIN_NAMESPACE_END
