//
// Created by AmazingBuff on 2026/9/13.
//

#include "renderer.h"

#include "present_hook.h"
#include "render_util.h"

#include "config/config.h"
#include "dx11/d3d11_util.h"
#include "icon/icon_overlay.h"
#include "mask/outline_mask.h"
#include "search/corpse_finder.h"
#include "search/searched_corpses.h"
#include "ui/pulse_timer.h"

#include "Plugin.h"

#include <CommonStates.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <ranges>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    constexpr float Hold_Fraction = 0.10f;

    float pulse_alpha(float progress)
    {
        return progress <= Hold_Fraction ? 1.0f : std::clamp(1.0f - (progress - Hold_Fraction) / (1.0f - Hold_Fraction), 0.0f, 1.0f);
    }

    float corpse_alpha(Config const& cfg, float distance, float alpha)
    {
        float const fade_range = std::max(cfg.max_distance - cfg.fade_start_distance, 1.0f);
        float const f = distance <= cfg.fade_start_distance ? 1.0f : 1.0f - std::clamp((distance - cfg.fade_start_distance) / fade_range, 0.0f, 1.0f);
        float const fade = std::pow(f, cfg.fade_power);
        return std::clamp(cfg.min_opacity + fade * alpha * (1.0f - cfg.min_opacity), cfg.min_opacity, 1.0f);
    }

    RE::BSGraphics::ViewData const* update_view_data(const RE::NiCamera* camera)
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

    bool project_icon_tip(RE::NiCamera* camera, RE::BSGraphics::ViewData const* view_data,
        DirectX::XMFLOAT3 const& point, float width, float height, DirectX::XMFLOAT2& tip)
    {
        float px = 0.0f, py = 0.0f, depth = 0.0f;
        if (project(camera, { point.x, point.y, point.z }, width, height, px, py, depth))
            return icon_screen_tip(px, py, depth, width, height, tip);
        if (!view_data)
            return false;
        DirectX::SimpleMath::Matrix const& matrix = view_data->viewProjMatrixUnjittered._11 != 0.0f ?
            view_data->viewProjMatrixUnjittered : view_data->viewProjMat;
        DirectX::XMFLOAT4 clip;
        DirectX::XMStoreFloat4(&clip, DirectX::XMVector4Transform(DirectX::XMVectorSet(point.x, point.y, point.z, 1.0f), matrix));
        return icon_clip_tip(clip, width, height, tip);
    }

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

            m_icon_overlay.end_frame();
            schedule_scan();

            RE::BSGraphics::Renderer* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!renderer)
            {
                m_icon_layout.reset();
                return;
            }

            RE::BSGraphics::RendererData& rt = renderer->GetRuntimeData();
            ID3D11Device* device = reinterpret_cast<ID3D11Device*>(rt.forwarder);
            ID3D11DeviceContext* context = reinterpret_cast<ID3D11DeviceContext*>(rt.context);
            if (!device || !context)
            {
                m_icon_layout.reset();
                return;
            }

            RE::BSGraphics::State* bs_state = RE::BSGraphics::State::GetSingleton();
            uint32_t const frame = bs_state ? bs_state->GetFrameCount() : 0;

            bool const skip_draw = (frame != 0) && (frame == m_last_drawn_frame);
            if (!skip_draw)
            {
                if (frame != 0)
                    m_last_drawn_frame = frame;

                draw(swap_chain, device, context);
            }
        }

    private:
        OverlayDirector()
            : m_scan_in_flight(false)
            , m_last_drawn_frame(std::numeric_limits<uint32_t>::max())
            , m_back_buffer(nullptr)
            , m_render_target(nullptr)
            , m_ready(false) {}

        void schedule_scan()
        {
            std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
            if (now - m_last_scan >= std::chrono::milliseconds(Setting::instance().get_config().scan_interval_ms) &&
                !m_scan_in_flight.exchange(true))
            {
                m_last_scan = now;
                SKSE::GetTaskInterface()->AddTask([this]
                {
                    CorpseScan::instance().search();
                    m_scan_in_flight.store(false);
                });
            }
        }

        void draw(IDXGISwapChain* swap_chain, ID3D11Device* device, ID3D11DeviceContext* context)
        {
            if (!init(swap_chain, device) || !update_back_buffer(swap_chain, device))
            {
                m_icon_layout.reset();
                return;
            }

            D3D11_TEXTURE2D_DESC desc{};
            m_back_buffer->GetDesc(&desc);

            float w = static_cast<float>(desc.Width);
            float h = static_cast<float>(desc.Height);
            if (w <= 0.0f || h <= 0.0f)
            {
                RE::BSGraphics::ScreenSize const screen = RE::BSGraphics::Renderer::GetScreenSize();
                w = static_cast<float>(screen.width);
                h = static_cast<float>(screen.height);
            }
            if (w <= 0.0f || h <= 0.0f)
            {
                m_icon_layout.reset();
                return;
            }

            Config const& cfg = Setting::instance().get_config();

            if (!cfg.enabled || cfg.display_mode != Config::DisplayMode::e_icon)
                m_icon_layout.reset();

            bool const pulse_mode = cfg.hotkey_mode == Config::HotkeyMode::e_pulse;
            bool const pulse_active = pulse_mode && cfg.enabled && PulseTimer::instance().active();
            if (pulse_mode && !pulse_active)
            {
                m_icon_layout.reset();
                return;
            }

            if (pulse_active || cfg.enabled)
            {
                std::vector<CorpseScan::CorpseInfo> corpses = CorpseScan::instance().snapshot();
                float const pulse = pulse_active ? pulse_alpha(PulseTimer::instance().progress()) : 1.0f;
                if (pulse <= 0.0f || corpses.empty())
                    m_icon_layout.reset();
                if (pulse > 0.f && !corpses.empty())
                {
                    RE::NiCamera* camera = RE::Main::WorldRootCamera();

                    // ---- CPU 端视锥剔除：范围扫描（max_distance）不区分朝向，而相机视锥
                    // 只覆盖屏幕方向；视野外尸体的叠加本就不可见（mask 几何会被 GPU 裁剪、
                    // icon 的 world_to_screen 会拒绝），提前丢弃可省掉本帧的 3D 遍历、布局
                    // 标定与全部 draw。相交判据用引擎原生 NiCamera::PointInFrustum——
                    // 包围球（anchor + radius，扫描期由碰撞盒/几何兜底得出）与视锥相交即
                    // 视为可见，与引擎自身剔除同语义；mask 不改场景，剔除纯属性能优化。
                    // 相机缺失（如主菜单态）时跳过剔除，保持既有行为。----
                    if (camera && cfg.display_mode != Config::DisplayMode::e_icon)
                    {
                        std::erase_if(corpses, [camera](CorpseScan::CorpseInfo const& corpse) {
                            return !camera->PointInFrustum(corpse.anchor, corpse.radius);
                        });
                    }

                    RE::BSGraphics::ViewData const* view_data = update_view_data(camera);

                    Color color;
                    color.decode(cfg.outline_color);
                    if (cfg.display_mode == Config::DisplayMode::e_icon)
                    {
                        m_icon_overlay.begin_frame(w, h);
                        std::vector<IconCandidate> candidates;
                        candidates.reserve(corpses.size());
                        for (CorpseScan::CorpseInfo const& corpse : corpses)
                        {
                            DirectX::XMFLOAT3 const anchor{ corpse.anchor.x, corpse.anchor.y, corpse.anchor.z };
                            DirectX::XMFLOAT3 const top = icon_anchor(anchor,
                                { corpse.bound_min.x, corpse.bound_min.y, corpse.bound_min.z },
                                { corpse.bound_max.x, corpse.bound_max.y, corpse.bound_max.z });
                            if (!std::isfinite(top.x) || !std::isfinite(top.y) || !std::isfinite(top.z))
                                continue;
                            DirectX::XMFLOAT2 tip{};
                            if (!project_icon_tip(camera, view_data, top, w, h, tip))
                                continue;

                            float const alpha = pulse * corpse_alpha(cfg, corpse.distance, color.a());
                            candidates.push_back({ corpse.form_id, anchor, tip, corpse.distance, alpha });
                        }
                        std::vector<IconMarker> const markers = m_icon_layout.update(candidates,
                            static_cast<float>(cfg.icon_radius), cfg.max_distance, w, h, Max_Corpse_Count);
                        for (const IconMarker& marker : std::views::reverse(markers))
                            m_icon_overlay.add_marker(marker, { color.r(), color.g(), color.b() });
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
                                    mask_targets.emplace_back(ref, DirectX::XMFLOAT4{ color.r(), color.g(), color.b(), pulse * corpse_alpha(cfg, corpse.distance, color.a())});
                            }
                        }
                        OutlineMask::instance().set_targets(mask_targets);
                        OutlineMask::instance().render(device, context, camera, m_render_target, desc.Width, desc.Height);
                    }
                }
            }
        }

        bool init(IDXGISwapChain* swap_chain, ID3D11Device* device)
        {
            if (m_ready)
                return true;

            m_states = std::make_shared<DirectX::DX11::CommonStates>(device);
            if (!m_states || !m_icon_overlay.init(device))
                return false;

            ID3D11Texture2D* buffer = nullptr;
            HRESULT const hr = swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer));
            if (FAILED(hr) || !buffer)
                return false;

            if (buffer == m_back_buffer)
            {
                buffer->Release();
                m_ready = true;
                return false;
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

            m_ready = true;
            return m_ready;
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
        std::chrono::steady_clock::time_point m_last_scan;
        std::atomic<bool> m_scan_in_flight;

        uint32_t m_last_drawn_frame;
        std::mutex m_draw_mutex;

        ID3D11Texture2D* m_back_buffer;
        ID3D11RenderTargetView* m_render_target;

        std::shared_ptr<DirectX::DX11::CommonStates> m_states;
        IconOverlay m_icon_overlay;
        IconLayout m_icon_layout;
        
        bool m_ready;
    };

    void STDMETHODCALLTYPE present_callback(IDXGISwapChain* swap_chain)
    {
        OverlayDirector::instance().on_present(swap_chain);
    }
}

void Renderer::install()
{
    (void)PresentHook::instance().install(&present_callback);
}

PLUGIN_NAMESPACE_END
