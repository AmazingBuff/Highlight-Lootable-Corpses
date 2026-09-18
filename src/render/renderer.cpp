//
// Created by AmazingBuff on 2026/9/13.
//

#include "renderer.h"

#include "present_hook.h"
#include "render_util.h"

#include "config/config.h"
#include "icon/icon_layout.h"
#include "icon/icon_overlay.h"
#include "mask/mask_overlay.h"
#include "render/dx11/common_states.h"
#include "search/corpse_finder.h"
#include "ui/pulse_timer.h"

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

    RE::BSGraphics::ViewData const* update_view_data(RE::NiCamera const* camera)
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
        if (project(camera, point, width, height, px, py, depth))
            return Icon::icon_screen_tip(px, py, tip);
        if (!view_data)
            return false;
        Matrix const& matrix = view_data->viewProjMatrixUnjittered._11 != 0.0f ? view_data->viewProjMatrixUnjittered : view_data->viewProjMat;
        DirectX::XMFLOAT4 clip{};
        DirectX::XMStoreFloat4(&clip, DirectX::XMVector4Transform(DirectX::XMVectorSet(point.x, point.y, point.z, 1.0f), matrix));
        return Icon::icon_clip_tip(clip, width, height, tip);
    }

    class OverlayDirector
    {
    public:
        static OverlayDirector& instance()
        {
            static OverlayDirector s_instance;
            return s_instance;
        }

        void on_present(REX::W32::IDXGISwapChain* swap_chain)
        {
            std::lock_guard<std::mutex> const draw_lock(m_draw_mutex);

            m_icon_overlay.end_frame();
            schedule_scan();

            RE::BSGraphics::Renderer* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!renderer)
                return;

            RE::BSGraphics::RendererData& rt = renderer->GetRuntimeData();
            REX::W32::ID3D11Device* device = rt.forwarder;
            REX::W32::ID3D11DeviceContext* context = rt.context;
            if (!device || !context)
                return;

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
        OverlayDirector() :
            m_scan_in_flight(false),
            m_last_drawn_frame(std::numeric_limits<uint32_t>::max()),
            m_back_buffer(nullptr),
            m_render_target(nullptr),
            m_ready(false) {}

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

        void draw(REX::W32::IDXGISwapChain* swap_chain, REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context)
        {
            if (!init(swap_chain, device) || !update_back_buffer(swap_chain, device))
                return;

            REX::W32::D3D11_TEXTURE2D_DESC desc{};
            m_back_buffer->GetDesc(&desc);

            uint32_t w = desc.width;
            uint32_t h = desc.height;
            if (w <= 0 || h <= 0)
            {
                RE::BSGraphics::ScreenSize const screen = RE::BSGraphics::Renderer::GetScreenSize();
                w = screen.width;
                h = screen.height;
            }
            if (w <= 0 || h <= 0)
                return;

            Config const& cfg = Setting::instance().get_config();
            bool const pulse_mode = cfg.hotkey_mode == Config::HotkeyMode::e_pulse;
            bool const pulse_active = pulse_mode && cfg.enabled && PulseTimer::instance().active();
            if (pulse_mode && !pulse_active)
                return;

            if (pulse_active || cfg.enabled)
            {
                std::vector<CorpseScan::CorpseInfo> corpses = CorpseScan::instance().snapshot();
                float const pulse = pulse_active ? pulse_alpha(PulseTimer::instance().progress()) : 1.0f;
                if (pulse > 0.f && !corpses.empty())
                {
                    RE::NiCamera* camera = RE::Main::WorldRootCamera();

                    // ---- CPU-side frustum culling: the range scan (max_distance) ignores direction
                    // while the camera frustum only covers the screen direction; the overlay of a
                    // corpse outside the view is invisible anyway (mask geometry is clipped by the
                    // GPU and the icon's world_to_screen rejects it), so discarding it early saves
                    // this frame's 3D traversal, layout calibration and all the draws. The
                    // intersection test uses the engine's own NiCamera::PointInFrustum - a bounding
                    // sphere (anchor + radius, produced during the scan from collision boxes or the
                    // geometry fallback) intersecting the frustum counts as visible, the same
                    // semantics as the engine's own culling; the mask does not change the scene, so
                    // this culling is purely a performance optimization.
                    // When the camera is missing (main-menu state, for example) culling is skipped
                    // and the existing behaviour is kept. ----
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

                        std::vector<Icon::IconCandidate> candidates;
                        candidates.reserve(corpses.size());
                        for (CorpseScan::CorpseInfo const& corpse : corpses)
                        {
                            DirectX::XMFLOAT3 const top = Icon::icon_anchor(render_cast(corpse.bound_min), render_cast(corpse.bound_max));
                            DirectX::XMFLOAT2 tip{};
                            if (!project_icon_tip(camera, view_data, top, static_cast<float>(w), static_cast<float>(h), tip))
                                continue;

                            float const alpha = pulse * corpse_alpha(cfg, corpse.distance, color.a());
                            candidates.emplace_back(corpse.form_id, render_cast(corpse.anchor), tip, corpse.distance, alpha);
                        }
                        std::vector<Icon::IconMarker> const markers = Icon::icon_marker(candidates, static_cast<float>(cfg.icon_radius), cfg.max_distance, Max_Corpse_Count);
                        for (Icon::IconMarker const& marker : std::views::reverse(markers))
                            m_icon_overlay.add_marker(marker, { color.r(), color.g(), color.b() });

                        m_icon_overlay.draw(context, m_render_target, *m_states);
                        m_icon_overlay.end_frame();
                    }
                    else
                    {
                        if (!m_mask_overlay.begin_frame(device, w, h))
                        {
                            m_mask_overlay.end_frame();
                            return;
                        }

                        std::vector<Mask::MaskTarget> mask_targets;
                        mask_targets.reserve(corpses.size());
                        for (CorpseScan::CorpseInfo const& corpse : corpses)
                        {
                            if (RE::TESForm* form = RE::TESForm::LookupByID(corpse.form_id))
                            {
                                if (RE::TESObjectREFR* ref = form->AsReference())
                                    mask_targets.emplace_back(RE::NiPointer<RE::TESObjectREFR>(ref), DirectX::XMFLOAT4{ color.r(), color.g(), color.b(), pulse * corpse_alpha(cfg, corpse.distance, color.a())});
                            }
                        }

                        m_mask_overlay.draw(device, context, camera, m_render_target, desc.width, desc.height, mask_targets, *m_states);
                        m_mask_overlay.end_frame();
                    }
                }
            }
        }

        bool init(REX::W32::IDXGISwapChain* swap_chain, REX::W32::ID3D11Device* device)
        {
            if (m_ready)
                return true;

            // CommonStates is the local REX::W32-typed mirror; it takes the REX device pointer directly.
            m_states = std::make_unique<CommonStates>(device);
            if (!m_states || !m_states->valid() || !m_icon_overlay.init(device) || !m_mask_overlay.init(device))
                return false;

            // The REX mirror's GetBuffer takes the REX IID constant directly (same GUID value as __uuidof).
            REX::W32::ID3D11Texture2D* buffer = nullptr;
            REX::W32::HRESULT const hr = swap_chain->GetBuffer(0, REX::W32::IID_ID3D11Texture2D, reinterpret_cast<void**>(&buffer));
            if (!REX::W32::SUCCESS(hr) || !buffer)
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
            REX::W32::HRESULT const rtv_hr = device->CreateRenderTargetView(m_back_buffer, nullptr, &m_render_target);
            if (!REX::W32::SUCCESS(rtv_hr) || !m_render_target)
            {
                logger::error("Failed to create backbuffer RTV: {:X}", static_cast<unsigned int>(rtv_hr));
                return false;
            }

            m_ready = true;
            return m_ready;
        }

        bool update_back_buffer(REX::W32::IDXGISwapChain* swap_chain, REX::W32::ID3D11Device* device)
        {
            REX::W32::ID3D11Texture2D* buffer = nullptr;
            REX::W32::HRESULT const hr = swap_chain->GetBuffer(0, REX::W32::IID_ID3D11Texture2D, reinterpret_cast<void**>(&buffer));
            if (!REX::W32::SUCCESS(hr) || !buffer)
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

            REX::W32::HRESULT const rtv_hr = device->CreateRenderTargetView(m_back_buffer, nullptr, &m_render_target);
            if (!REX::W32::SUCCESS(rtv_hr) || !m_render_target)
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

        REX::W32::ID3D11Texture2D* m_back_buffer;
        REX::W32::ID3D11RenderTargetView* m_render_target;

        std::unique_ptr<CommonStates> m_states;
        Icon::IconOverlay m_icon_overlay;
        Mask::MaskOverlay m_mask_overlay;

        bool m_ready;
    };

    void __stdcall present_callback(REX::W32::IDXGISwapChain* swap_chain)
    {
        OverlayDirector::instance().on_present(swap_chain);
    }
}

void Renderer::install()
{
    (void)PresentHook::instance().install(&present_callback);
}

PLUGIN_NAMESPACE_END
