#include "pch.h"
#include "esp_renderer.h"
#include "config.h"
#include "corpse_finder.h"
#include "input.h"

namespace
{
    // ---------------------------------------------------------------------------
    // IDXGISwapChain::Present vtable 钩子（vtable 第 8 槽位）
    // 运行时无关：不依赖 Address Library ID，任何 AE 版本都有效
    // ---------------------------------------------------------------------------
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    PresentFn g_original_present = nullptr;
    void** g_hooked_slot = nullptr;

    HRESULT STDMETHODCALLTYPE present_thunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
    {
        ESPRenderer::on_present(a_swapChain);
        return g_original_present(a_swapChain, a_syncInterval, a_flags);
    }

    // ---------------------------------------------------------------------------
    // 绘制资源（懒创建，渲染线程独占）
    // ---------------------------------------------------------------------------
    std::unique_ptr<DirectX::CommonStates> g_states;
    std::unique_ptr<DirectX::BasicEffect> g_effect;
    std::unique_ptr<DirectX::PrimitiveBatch<DirectX::VertexPositionColor>> g_batch;

    ID3D11RenderTargetView* g_back_buffer_rtv = nullptr;
    ID3D11Texture2D* g_back_buffer = nullptr;
    std::uint32_t g_back_w = 0;  // 后台缓冲真实尺寸（来自交换链纹理描述）
    std::uint32_t g_back_h = 0;
    DXGI_FORMAT g_back_buffer_format = DXGI_FORMAT_UNKNOWN;

    // sRGB 编码值 → 线性值。后台缓冲为 *_SRGB 格式时，DX11 输出合并阶段会把
    // 写入值再做 线性→sRGB 编码；为让 box 显示颜色 == 配置值（与 UI 色块一致），
    // 需预先把 sRGB 值还原为线性值，写入后硬件再编码回原值。
    [[nodiscard]] float SrgbToLinear(float a_c) noexcept
    {
        return a_c <= 0.04045f ? a_c / 12.92f : std::pow((a_c + 0.055f) / 1.055f, 2.4f);
    }

    [[nodiscard]] bool IsSrgbBackBuffer() noexcept
    {
        switch (g_back_buffer_format)
        {
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return true;
        default:
            return false;
        }
    }

    // ---------------------------------------------------------------------------
    // 扫描调度（渲染线程计时，游戏线程执行）
    // ---------------------------------------------------------------------------
    std::chrono::steady_clock::time_point g_last_scan{};

    // NiRect<T> 成员为 protected，按固定布局（left, right, top, bottom）
    // memcpy 到同布局的本地 POD 读取相机 port，避免修改三方库。
    struct PortRect
    {
        float left, right, top, bottom;
    };
    static_assert(sizeof(PortRect) == sizeof(RE::NiRect<float>));

    bool ensure_back_buffer(IDXGISwapChain* a_swapChain, ID3D11Device* a_device)
    {
        ID3D11Texture2D* buffer = nullptr;
        HRESULT const hr = a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer));
        if (FAILED(hr) || !buffer)
        {
            return false;
        }

        if (buffer == g_back_buffer)
        {
            buffer->Release();
            return true;
        }

        if (g_back_buffer_rtv)
        {
            g_back_buffer_rtv->Release();
            g_back_buffer_rtv = nullptr;
        }
        if (g_back_buffer)
        {
            g_back_buffer->Release();
            g_back_buffer = nullptr;
        }

        g_back_buffer = buffer;
        D3D11_TEXTURE2D_DESC desc{};
        buffer->GetDesc(&desc);
        g_back_w = desc.Width;
        g_back_h = desc.Height;
        g_back_buffer_format = desc.Format;
        if (g_back_buffer_format != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            logger::info("Back buffer format: {}", static_cast<int>(g_back_buffer_format));
        }
        HRESULT const rtv_hr = a_device->CreateRenderTargetView(buffer, nullptr, &g_back_buffer_rtv);
        if (FAILED(rtv_hr) || !g_back_buffer_rtv)
        {
            logger::error("Failed to create backbuffer RTV: {:X}", static_cast<unsigned int>(rtv_hr));
            return false;
        }
        return true;
    }

    void ensure_draw_resources(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
    {
        if (!g_states)
        {
            g_states = std::make_unique<DirectX::CommonStates>(a_device);
        }
        if (!g_effect)
        {
            g_effect = std::make_unique<DirectX::BasicEffect>(a_device);
            g_effect->SetVertexColorEnabled(true);
        }
        if (!g_batch)
        {
            g_batch = std::make_unique<DirectX::PrimitiveBatch<DirectX::VertexPositionColor>>(a_context);
        }
    }

    void draw_filled_rect(float a_x0, float a_y0, float a_x1, float a_y1, DirectX::XMFLOAT4 const& a_color)
    {
        DirectX::VertexPositionColor const vertices[6] = {
            { DirectX::XMFLOAT3(a_x0, a_y0, 0.5f), a_color },
            { DirectX::XMFLOAT3(a_x1, a_y0, 0.5f), a_color },
            { DirectX::XMFLOAT3(a_x1, a_y1, 0.5f), a_color },
            { DirectX::XMFLOAT3(a_x0, a_y0, 0.5f), a_color },
            { DirectX::XMFLOAT3(a_x1, a_y1, 0.5f), a_color },
            { DirectX::XMFLOAT3(a_x0, a_y1, 0.5f), a_color },
        };
        g_batch->Draw(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, vertices, 6);
    }

    void draw_rect_outline(float a_x0, float a_y0, float a_x1, float a_y1, float a_thickness, DirectX::XMFLOAT4 const& a_color)
    {
        float const t = std::max(a_thickness, 1.0f);
        draw_filled_rect(a_x0, a_y0, a_x1, a_y0 + t, a_color);          // 上
        draw_filled_rect(a_x0, a_y1 - t, a_x1, a_y1, a_color);          // 下
        draw_filled_rect(a_x0, a_y0 + t, a_x0 + t, a_y1 - t, a_color);  // 左
        draw_filled_rect(a_x1 - t, a_y0 + t, a_x1, a_y1 - t, a_color);  // 右
    }

    // 屏幕空间两点间的粗线段（旋转四边形，用于 3D 线框盒的边）
    void draw_thick_line(DirectX::XMFLOAT2 const& a_p0, DirectX::XMFLOAT2 const& a_p1, float a_thickness, DirectX::XMFLOAT4 const& a_color)
    {
        float const t = std::max(a_thickness, 1.0f) * 0.5f;
        float const dx = a_p1.x - a_p0.x;
        float const dy = a_p1.y - a_p0.y;
        float const len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f)
        {
            draw_filled_rect(a_p0.x - t, a_p0.y - t, a_p0.x + t, a_p0.y + t, a_color);
            return;
        }
        float const nx = -dy / len * t;
        float const ny = dx / len * t;
        DirectX::VertexPositionColor const vertices[6] = {
            { DirectX::XMFLOAT3(a_p0.x + nx, a_p0.y + ny, 0.5f), a_color },
            { DirectX::XMFLOAT3(a_p0.x - nx, a_p0.y - ny, 0.5f), a_color },
            { DirectX::XMFLOAT3(a_p1.x - nx, a_p1.y - ny, 0.5f), a_color },
            { DirectX::XMFLOAT3(a_p0.x + nx, a_p0.y + ny, 0.5f), a_color },
            { DirectX::XMFLOAT3(a_p1.x - nx, a_p1.y - ny, 0.5f), a_color },
            { DirectX::XMFLOAT3(a_p1.x + nx, a_p1.y + ny, 0.5f), a_color },
        };
        g_batch->Draw(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, vertices, 6);
    }
}

namespace ESPRenderer
{
    void install()
    {
        if (g_hooked_slot)
        {
            return;  // 已安装
        }

        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer)
        {
            return;
        }

        auto& rt = renderer->GetRuntimeData();
        if (!rt.renderWindows || !rt.renderWindows[0].swapChain)
        {
            logger::warn("SwapChain not available yet, will retry on next game message");
            return;
        }

        auto* swapChain = reinterpret_cast<IDXGISwapChain*>(rt.renderWindows[0].swapChain);
        void** vtable = *reinterpret_cast<void***>(swapChain);

        // IDXGISwapChain::Present 是虚函数表中第 8 个槽位
        g_hooked_slot = &vtable[8];
        g_original_present = reinterpret_cast<PresentFn>(*g_hooked_slot);

        DWORD oldProtect = 0;
        if (!VirtualProtect(g_hooked_slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            logger::error("VirtualProtect failed, cannot install Present hook");
            g_hooked_slot = nullptr;
            g_original_present = nullptr;
            return;
        }
        *g_hooked_slot = reinterpret_cast<void*>(&present_thunk);
        VirtualProtect(g_hooked_slot, sizeof(void*), oldProtect, &oldProtect);

        logger::info("Installed IDXGISwapChain::Present hook (swapchain={}, original={})", fmt::ptr(swapChain), fmt::ptr(g_original_present));
    }

    void on_present(IDXGISwapChain* a_swapChain)
    {
        Input::poll();

        // 定时派发尸体扫描任务到游戏线程
        auto const now = std::chrono::steady_clock::now();
        if (now - g_last_scan >= std::chrono::milliseconds(Config::get().scan_interval_ms))
        {
            g_last_scan = now;
            SKSE::GetTaskInterface()->AddTask([]() { CorpseFinder::scan(); });
        }

        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer)
        {
            return;
        }

        auto& rt = renderer->GetRuntimeData();
        auto* device = reinterpret_cast<ID3D11Device*>(rt.forwarder);
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(rt.context);
        if (!device || !context)
        {
            return;
        }

        if (!ensure_back_buffer(a_swapChain, device))
        {
            return;
        }

        ensure_draw_resources(device, context);
        if (!g_states || !g_effect || !g_batch)
        {
            return;
        }

        float w = static_cast<float>(g_back_w);
        float h = static_cast<float>(g_back_h);
        if (w <= 0.0f || h <= 0.0f)
        {
            // 后备：渲染器报告的屏幕尺寸
            auto const screen = RE::BSGraphics::Renderer::GetScreenSize();
            w = static_cast<float>(screen.width);
            h = static_cast<float>(screen.height);
        }
        if (w <= 0.0f || h <= 0.0f)
        {
            return;
        }

        // ---- 保存游戏渲染状态，设置我们的绘制状态 ----
        ID3D11RenderTargetView* prev_rtv = nullptr;
        ID3D11DepthStencilView* prev_dsv = nullptr;
        context->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);
        context->OMSetRenderTargets(1, &g_back_buffer_rtv, nullptr);

        ID3D11BlendState* prev_blend = nullptr;
        float blend_factor[4]{};
        UINT sample_mask = 0;
        context->OMGetBlendState(&prev_blend, blend_factor, &sample_mask);

        ID3D11DepthStencilState* prev_depth = nullptr;
        UINT prev_stencil = 0;
        context->OMGetDepthStencilState(&prev_depth, &prev_stencil);

        ID3D11RasterizerState* prev_rs = nullptr;
        context->RSGetState(&prev_rs);

        context->OMSetBlendState(g_states->AlphaBlend(), nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(g_states->DepthNone(), 0);
        context->RSSetState(g_states->CullNone());

        g_effect->SetWorld(DirectX::XMMatrixIdentity());
        g_effect->SetView(DirectX::XMMatrixIdentity());
        g_effect->SetProjection(DirectX::XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f));
        g_effect->Apply(context);

        g_batch->Begin();

        // ---- 尸体 ESP 标记 ----
        if (Config::is_enabled())
        {
            auto const& cfg = Config::get();
            if (cfg.show_outline)
            {
                // 相机对象：世界根相机（引擎每帧更新其 worldToCam，Present 时仍是本帧数据）
                RE::NiCamera* world_cam = RE::Main::WorldRootCamera();

                // 备选：BSGraphics::State 相机缓存里的 viewProj 矩阵
                // （实测在 AE 上该矩阵读出的是坏值：_22=0、_43=0，故仅作兜底保留）
                auto* state = RE::BSGraphics::State::GetSingleton();
                RE::BSGraphics::ViewData const* view_data = nullptr;
                if (state)
                {
                    auto& state_rt = state->GetRuntimeData();
                    for (auto const& cam_data : state_rt.cameraDataCacheA)
                    {
                        if (cam_data.referenceCamera == world_cam)
                        {
                            view_data = std::addressof(cam_data.GetCameraStateRuntimeData().camViewData);
                            break;
                        }
                    }
                    if (!view_data && !state_rt.cameraDataCacheA.empty())
                    {
                        view_data = std::addressof(state_rt.cameraDataCacheA.front().GetCameraStateRuntimeData().camViewData);
                    }
                }

                static bool s_loggedProjectionSource = false;
                if (!s_loggedProjectionSource)
                {
                    s_loggedProjectionSource = true;
                    logger::info(
                        "Projection source: world_cam={} cameraDataCacheSize={} view_data={} viewProjDiag=(unj _11={:.3f} _22={:.3f} _43={:.3f})",
                        fmt::ptr(world_cam),
                        state ? static_cast<int>(state->GetRuntimeData().cameraDataCacheA.size()) : -1,
                        fmt::ptr(view_data),
                        view_data ? view_data->viewProjMatrixUnjittered._11 : 0.0f,
                        view_data ? view_data->viewProjMatrixUnjittered._22 : 0.0f,
                        view_data ? view_data->viewProjMatrixUnjittered._43 : 0.0f);
                }

                auto const rgb = cfg.outline_color;
                float const cr = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
                float const cg = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
                float const cb = static_cast<float>(rgb & 0xFF) / 255.0f;
                // sRGB 后台缓冲：预还原为线性值，保证显示颜色与配置值/UI 色块一致
                bool const srgb_back = IsSrgbBackBuffer();
                auto const to_output = [&](float r, float g, float b, float a) {
                    return DirectX::XMFLOAT4{
                        srgb_back ? SrgbToLinear(r) : r,
                        srgb_back ? SrgbToLinear(g) : g,
                        srgb_back ? SrgbToLinear(b) : b,
                        a
                    };
                };

                for (auto const& corpse : CorpseFinder::snapshot())
                {
                    // ---- 投影函数：世界点 -> 屏幕像素（左上原点），成功返回 true ----
                    // 优先用引擎 NiCamera::WorldPtToScreenPt3（返回左下原点归一化坐标），
                    // 失败时兜底用 State 的 viewProj 矩阵。
                    auto project_to_screen = [&](RE::NiPoint3 const& a_pt, float& a_px, float& a_py, float& a_depth) -> bool {
                        bool ok = false;
                        if (world_cam && world_cam->WorldPtToScreenPt3(a_pt, a_px, a_py, a_depth, 1e-5f))
                        {
                            // 相机 port 若是像素单位，先把输出归一化到 0..1
                            auto const port = world_cam->GetRuntimeData2().port;
                            PortRect pr;
                            std::memcpy(&pr, &port, sizeof(pr));
                            float const port_l = pr.left;
                            float const port_t = pr.top;
                            float const port_w = pr.right - pr.left;
                            float const port_h = pr.bottom - pr.top;
                            float nx = a_px, ny = a_py;
                            if (port_w > 10.0f)
                            {
                                nx = (a_px - port_l) / port_w;
                            }
                            if (port_h > 10.0f)
                            {
                                ny = (a_py - port_t) / port_h;
                            }
                            // 引擎函数输出为“左下原点”归一化坐标（TrueDirectionalMovement 同样处理），翻转为左上原点
                            a_px = nx * w;
                            a_py = (1.0f - ny) * h;
                            ok = a_depth > 0.0f;
                        } else if (view_data &&
                                   (view_data->viewProjMatrixUnjittered._11 != 0.0f || view_data->viewProjMat._11 != 0.0f))
                        {
                            auto const& viewProj = view_data->viewProjMatrixUnjittered._11 != 0.0f ? view_data->viewProjMatrixUnjittered : view_data->viewProjMat;
                            DirectX::XMVECTOR const clip = DirectX::XMVector4Transform(
                                DirectX::XMVectorSet(a_pt.x, a_pt.y, a_pt.z, 1.0f),
                                viewProj);
                            float const clip_w = DirectX::XMVectorGetW(clip);
                            if (std::fabs(clip_w) >= 1e-5f)
                            {
                                DirectX::XMVECTOR const ndc = DirectX::XMVectorDivide(clip, DirectX::XMVectorReplicate(clip_w));
                                float const ndc_x = DirectX::XMVectorGetX(ndc);
                                float const ndc_y = DirectX::XMVectorGetY(ndc);
                                float const ndc_z = DirectX::XMVectorGetZ(ndc);
                                if (ndc_x >= -1.0f && ndc_x <= 1.0f && ndc_y >= -1.0f && ndc_y <= 1.0f && ndc_z >= 0.0f && ndc_z <= 1.0f)
                                {
                                    a_px = (ndc_x * 0.5f + 0.5f) * w;
                                    a_py = (1.0f - ndc_y) * 0.5f * h;
                                    a_depth = ndc_z;
                                    ok = true;
                                }
                            }
                        }
                        return ok;
                    };

                    // 尸体世界 AABB 是否有效
                    bool const hasAABB =
                        corpse.bound_min.x <= corpse.bound_max.x &&
                        corpse.bound_min.y <= corpse.bound_max.y &&
                        corpse.bound_min.z <= corpse.bound_max.z;

                    // 把一批世界点投影到屏幕，取屏幕包围矩形
                    float min_x = 1e30f, max_x = -1e30f, min_y = 1e30f, max_y = -1e30f;
                    float depth_sum = 0.0f;
                    int depth_count = 0;
                    bool has_rect = false;

                    // OBB 线框盒：记录 8 个角各自的屏幕坐标
                    float proj_x[8]{}, proj_y[8]{};
                    bool obb_ok[8]{};

                    auto add_projected = [&](RE::NiPoint3 const& a_pt, int a_idx = -1) {
                        float px = 0.0f, py = 0.0f, d = 0.0f;
                        if (project_to_screen(a_pt, px, py, d))
                        {
                            min_x = std::min(min_x, px);
                            max_x = std::max(max_x, px);
                            min_y = std::min(min_y, py);
                            max_y = std::max(max_y, py);
                            depth_sum += d;
                            ++depth_count;
                            has_rect = true;
                            if (a_idx >= 0 && a_idx < 8)
                            {
                                proj_x[a_idx] = px;
                                proj_y[a_idx] = py;
                                obb_ok[a_idx] = true;
                            }
                        }
                    };

                    bool obb_all_ok = true;
                    if (corpse.has_obb)
                    {
                        // 投影碰撞盒（OBB）的 8 个世界角点：屏幕矩形贴合碰撞盒的屏幕足迹
                        for (int i = 0; i < 8; ++i)
                        {
                            add_projected(corpse.obb_corners[i], i);
                        }
                        for (bool ok : obb_ok)
                        {
                            obb_all_ok = obb_all_ok && ok;
                        }
                    } else if (hasAABB)
                    {
                        // 投影 AABB 的 8 个角，得到贴合尸体包围盒的屏幕矩形
                        RE::NiPoint3 const corners[8] = {
                            { corpse.bound_min.x, corpse.bound_min.y, corpse.bound_min.z },
                            { corpse.bound_max.x, corpse.bound_min.y, corpse.bound_min.z },
                            { corpse.bound_min.x, corpse.bound_max.y, corpse.bound_min.z },
                            { corpse.bound_max.x, corpse.bound_max.y, corpse.bound_min.z },
                            { corpse.bound_min.x, corpse.bound_min.y, corpse.bound_max.z },
                            { corpse.bound_max.x, corpse.bound_min.y, corpse.bound_max.z },
                            { corpse.bound_min.x, corpse.bound_max.y, corpse.bound_max.z },
                            { corpse.bound_max.x, corpse.bound_max.y, corpse.bound_max.z },
                        };
                        for (auto const& corner : corners)
                        {
                            add_projected(corner);
                        }
                    } else
                    {
                        // 兜底：没有 AABB 时用锚点 + 世界半径投影上下左右
                        RE::NiPoint3 camRight{ 1.0f, 0.0f, 0.0f };
                        RE::NiPoint3 camUp{ 0.0f, 1.0f, 0.0f };
                        if (world_cam)
                        {
                            camRight = world_cam->world.rotate.GetVectorX();
                            camUp = world_cam->world.rotate.GetVectorY();
                        }
                        add_projected(corpse.anchor);
                        add_projected(corpse.anchor + camRight * corpse.radius);
                        add_projected(corpse.anchor - camRight * corpse.radius);
                        add_projected(corpse.anchor + camUp * corpse.radius);
                        add_projected(corpse.anchor - camUp * corpse.radius);
                    }

                    if (!has_rect)
                    {
                        continue;
                    }

                    // 矩形与中心
                    float const sx = (min_x + max_x) * 0.5f;
                    float const sy = (min_y + max_y) * 0.5f;
                    float const box_w = max_x - min_x;
                    float const box_h = max_y - min_y;
                    // 保证最小可见尺寸（太远时不会缩成一个点）
                    constexpr float kMinBox = 5.0f;
                    float x0 = min_x, y0 = min_y, x1 = max_x, y1 = max_y;
                    if (box_w < kMinBox || box_h < kMinBox)
                    {
                        float const half_x = std::max(box_w * 0.5f, kMinBox * 0.5f);
                        float const half_y = std::max(box_h * 0.5f, kMinBox * 0.5f);
                        x0 = sx - half_x;
                        x1 = sx + half_x;
                        y0 = sy - half_y;
                        y1 = sy + half_y;
                    }

                    // 距离衰减：FadeStartDistance 内完全不透明；超过后按
                    // FadePower 指数衰减，到 MaxDistance 处达到 MinOpacity 下限。
                    // 远处尸体的 box 边框越来越"虚"，近处保持清晰。
                    float const fade_range = std::max(cfg.max_distance - cfg.fade_start_distance, 1.0f);
                    float const f = corpse.distance <= cfg.fade_start_distance ? 1.0f : 1.0f - std::clamp((corpse.distance - cfg.fade_start_distance) / fade_range, 0.0f, 1.0f);
                    float const fade = std::pow(f, cfg.fade_power);
                    float const alpha = std::clamp(
                        cfg.min_opacity + fade * (1.0f - cfg.min_opacity),
                        cfg.min_opacity,
                        1.0f);

                    // 有方向碰撞盒（OBB）且屏幕尺寸足够大时，画 12 条边的 3D 线框盒，
                    // 与尸体碰撞盒逐边重合；否则退化为 AABB 屏幕矩形。
                    bool const use_wireframe =
                        corpse.has_obb && obb_all_ok && box_w >= kMinBox && box_h >= kMinBox;

                    if (cfg.show_outline)
                    {
                        if (use_wireframe)
                        {
                            static constexpr std::int32_t kEdges[12][2] = {
                                { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },  // 底面
                                { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },  // 顶面
                                { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },  // 竖边
                            };
                            auto const col = to_output(cr, cg, cb, alpha);
                            for (auto const& e : kEdges)
                            {
                                draw_thick_line(
                                    DirectX::XMFLOAT2{ proj_x[e[0]], proj_y[e[0]] },
                                    DirectX::XMFLOAT2{ proj_x[e[1]], proj_y[e[1]] },
                                    cfg.outline_thickness,
                                    col);
                            }
                        } else
                        {
                            draw_rect_outline(
                                x0, y0, x1, y1, cfg.outline_thickness,
                                to_output(cr, cg, cb, alpha));
                        }
                    }
                }
            }
        }

        g_batch->End();

        // ---- 恢复游戏渲染状态 ----
        context->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
        context->OMSetBlendState(prev_blend, blend_factor, sample_mask);
        context->OMSetDepthStencilState(prev_depth, prev_stencil);
        context->RSSetState(prev_rs);

        if (prev_rtv)
        {
            prev_rtv->Release();
        }
        if (prev_dsv)
        {
            prev_dsv->Release();
        }
        if (prev_blend)
        {
            prev_blend->Release();
        }
        if (prev_depth)
        {
            prev_depth->Release();
        }
        if (prev_rs)
        {
            prev_rs->Release();
        }
    }
}
