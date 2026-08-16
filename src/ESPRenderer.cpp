#include "PCH.h"
#include "ESPRenderer.h"
#include "Config.h"
#include "CorpseFinder.h"
#include "Input.h"

namespace
{
    // ---------------------------------------------------------------------------
    // IDXGISwapChain::Present vtable 钩子（vtable 第 8 槽位）
    // 运行时无关：不依赖 Address Library ID，任何 AE 版本都有效
    // ---------------------------------------------------------------------------
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    PresentFn g_originalPresent = nullptr;
    void** g_hookedSlot = nullptr;

    HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
    {
        ESPRenderer::OnPresent(a_swapChain);
        return g_originalPresent(a_swapChain, a_syncInterval, a_flags);
    }

    // ---------------------------------------------------------------------------
    // 绘制资源（懒创建，渲染线程独占）
    // ---------------------------------------------------------------------------
    std::unique_ptr<DirectX::CommonStates> g_states;
    std::unique_ptr<DirectX::BasicEffect> g_effect;
    std::unique_ptr<DirectX::PrimitiveBatch<DirectX::VertexPositionColor>> g_batch;

    ID3D11RenderTargetView* g_backBufferRTV = nullptr;
    ID3D11Texture2D* g_backBuffer = nullptr;
    std::uint32_t g_backW = 0;  // 后台缓冲真实尺寸（来自交换链纹理描述）
    std::uint32_t g_backH = 0;

    // ---------------------------------------------------------------------------
    // 扫描调度（渲染线程计时，游戏线程执行）
    // ---------------------------------------------------------------------------
    std::chrono::steady_clock::time_point g_lastScan{};

    // NiRect<T> 成员为 protected，按固定布局（left, right, top, bottom）
    // memcpy 到同布局的本地 POD 读取相机 port，避免修改三方库。
    struct PortRect
    {
        float left, right, top, bottom;
    };
    static_assert(sizeof(PortRect) == sizeof(RE::NiRect<float>));

    bool EnsureBackBuffer(IDXGISwapChain* a_swapChain, ID3D11Device* a_device)
    {
        ID3D11Texture2D* buffer = nullptr;
        HRESULT const hr = a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer));
        if (FAILED(hr) || !buffer)
        {
            return false;
        }

        if (buffer == g_backBuffer)
        {
            buffer->Release();
            return true;
        }

        if (g_backBufferRTV)
        {
            g_backBufferRTV->Release();
            g_backBufferRTV = nullptr;
        }
        if (g_backBuffer)
        {
            g_backBuffer->Release();
            g_backBuffer = nullptr;
        }

        g_backBuffer = buffer;
        D3D11_TEXTURE2D_DESC desc{};
        buffer->GetDesc(&desc);
        g_backW = desc.Width;
        g_backH = desc.Height;
        HRESULT const rtvHr = a_device->CreateRenderTargetView(buffer, nullptr, &g_backBufferRTV);
        if (FAILED(rtvHr) || !g_backBufferRTV)
        {
            logger::error("Failed to create backbuffer RTV: {:X}", static_cast<unsigned int>(rtvHr));
            return false;
        }
        return true;
    }

    void EnsureDrawResources(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
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

    void DrawFilledRect(float a_x0, float a_y0, float a_x1, float a_y1, DirectX::XMFLOAT4 const& a_color)
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

    void DrawRectOutline(float a_x0, float a_y0, float a_x1, float a_y1, float a_thickness, DirectX::XMFLOAT4 const& a_color)
    {
        float const t = std::max(a_thickness, 1.0f);
        DrawFilledRect(a_x0, a_y0, a_x1, a_y0 + t, a_color);          // 上
        DrawFilledRect(a_x0, a_y1 - t, a_x1, a_y1, a_color);          // 下
        DrawFilledRect(a_x0, a_y0 + t, a_x0 + t, a_y1 - t, a_color);  // 左
        DrawFilledRect(a_x1 - t, a_y0 + t, a_x1, a_y1 - t, a_color);  // 右
    }

    // 屏幕空间两点间的粗线段（旋转四边形，用于 3D 线框盒的边）
    void DrawThickLine(DirectX::XMFLOAT2 const& a_p0, DirectX::XMFLOAT2 const& a_p1, float a_thickness, DirectX::XMFLOAT4 const& a_color)
    {
        float const t = std::max(a_thickness, 1.0f) * 0.5f;
        float const dx = a_p1.x - a_p0.x;
        float const dy = a_p1.y - a_p0.y;
        float const len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f)
        {
            DrawFilledRect(a_p0.x - t, a_p0.y - t, a_p0.x + t, a_p0.y + t, a_color);
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
    void Install()
    {
        if (g_hookedSlot)
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
        g_hookedSlot = &vtable[8];
        g_originalPresent = reinterpret_cast<PresentFn>(*g_hookedSlot);

        DWORD oldProtect = 0;
        if (!VirtualProtect(g_hookedSlot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            logger::error("VirtualProtect failed, cannot install Present hook");
            g_hookedSlot = nullptr;
            g_originalPresent = nullptr;
            return;
        }
        *g_hookedSlot = reinterpret_cast<void*>(&PresentThunk);
        VirtualProtect(g_hookedSlot, sizeof(void*), oldProtect, &oldProtect);

        logger::info("Installed IDXGISwapChain::Present hook (swapchain={}, original={})", fmt::ptr(swapChain), fmt::ptr(g_originalPresent));
    }

    void OnPresent(IDXGISwapChain* a_swapChain)
    {
        Input::Poll();

        // 定时派发尸体扫描任务到游戏线程
        auto const now = std::chrono::steady_clock::now();
        if (now - g_lastScan >= std::chrono::milliseconds(Config::Get().scanIntervalMs))
        {
            g_lastScan = now;
            SKSE::GetTaskInterface()->AddTask([]() { CorpseFinder::Scan(); });
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

        if (!EnsureBackBuffer(a_swapChain, device))
        {
            return;
        }

        EnsureDrawResources(device, context);
        if (!g_states || !g_effect || !g_batch)
        {
            return;
        }

        float w = static_cast<float>(g_backW);
        float h = static_cast<float>(g_backH);
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
        ID3D11RenderTargetView* prevRTV = nullptr;
        ID3D11DepthStencilView* prevDSV = nullptr;
        context->OMGetRenderTargets(1, &prevRTV, &prevDSV);
        context->OMSetRenderTargets(1, &g_backBufferRTV, nullptr);

        ID3D11BlendState* prevBlend = nullptr;
        float blendFactor[4]{};
        UINT sampleMask = 0;
        context->OMGetBlendState(&prevBlend, blendFactor, &sampleMask);

        ID3D11DepthStencilState* prevDepth = nullptr;
        UINT prevStencil = 0;
        context->OMGetDepthStencilState(&prevDepth, &prevStencil);

        ID3D11RasterizerState* prevRS = nullptr;
        context->RSGetState(&prevRS);

        context->OMSetBlendState(g_states->AlphaBlend(), nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(g_states->DepthNone(), 0);
        context->RSSetState(g_states->CullNone());

        g_effect->SetWorld(DirectX::XMMatrixIdentity());
        g_effect->SetView(DirectX::XMMatrixIdentity());
        g_effect->SetProjection(DirectX::XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f));
        g_effect->Apply(context);

        g_batch->Begin();

        // ---- 右上角开关指示点 ----
        if (Config::Get().showIndicator)
        {
            bool const enabled = Config::IsEnabled();
            auto const color = enabled ? DirectX::XMFLOAT4{ 0.0f, 1.0f, 0.35f, 1.0f } : DirectX::XMFLOAT4{ 0.45f, 0.45f, 0.45f, 0.8f };
            DrawFilledRect(w - 26.0f, 16.0f, w - 12.0f, 30.0f, color);
        }

        // ---- 尸体 ESP 标记 ----
        if (Config::IsEnabled())
        {
            auto const& cfg = Config::Get();
            bool const drawSomething = cfg.showOutline || cfg.showGlow || cfg.showCenterDot;
            if (drawSomething)
            {
                // 相机对象：世界根相机（引擎每帧更新其 worldToCam，Present 时仍是本帧数据）
                RE::NiCamera* worldCam = RE::Main::WorldRootCamera();

                // 备选：BSGraphics::State 相机缓存里的 viewProj 矩阵
                // （实测在 AE 上该矩阵读出的是坏值：_22=0、_43=0，故仅作兜底保留）
                auto* state = RE::BSGraphics::State::GetSingleton();
                RE::BSGraphics::ViewData const* viewData = nullptr;
                if (state)
                {
                    auto& stateRt = state->GetRuntimeData();
                    for (auto const& camData : stateRt.cameraDataCacheA)
                    {
                        if (camData.referenceCamera == worldCam)
                        {
                            viewData = std::addressof(camData.GetCameraStateRuntimeData().camViewData);
                            break;
                        }
                    }
                    if (!viewData && !stateRt.cameraDataCacheA.empty())
                    {
                        viewData = std::addressof(stateRt.cameraDataCacheA.front().GetCameraStateRuntimeData().camViewData);
                    }
                }

                static bool s_loggedProjectionSource = false;
                if (!s_loggedProjectionSource)
                {
                    s_loggedProjectionSource = true;
                    logger::info(
                        "Projection source: worldCam={} cameraDataCacheSize={} viewData={} viewProjDiag=(unj _11={:.3f} _22={:.3f} _43={:.3f})",
                        fmt::ptr(worldCam),
                        state ? static_cast<int>(state->GetRuntimeData().cameraDataCacheA.size()) : -1,
                        fmt::ptr(viewData),
                        viewData ? viewData->viewProjMatrixUnjittered._11 : 0.0f,
                        viewData ? viewData->viewProjMatrixUnjittered._22 : 0.0f,
                        viewData ? viewData->viewProjMatrixUnjittered._43 : 0.0f);
                }

                auto const rgb = cfg.outlineColor;
                float const cr = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
                float const cg = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
                float const cb = static_cast<float>(rgb & 0xFF) / 255.0f;

                for (auto const& corpse : CorpseFinder::Snapshot())
                {
                    // ---- 投影函数：世界点 -> 屏幕像素（左上原点），成功返回 true ----
                    // 优先用引擎 NiCamera::WorldPtToScreenPt3（返回左下原点归一化坐标），
                    // 失败时兜底用 State 的 viewProj 矩阵。
                    auto projectToScreen = [&](RE::NiPoint3 const& a_pt, float& a_px, float& a_py, float& a_depth) -> bool {
                        bool ok = false;
                        if (worldCam && worldCam->WorldPtToScreenPt3(a_pt, a_px, a_py, a_depth, 1e-5f))
                        {
                            // 相机 port 若是像素单位，先把输出归一化到 0..1
                            auto const port = worldCam->GetRuntimeData2().port;
                            PortRect pr;
                            std::memcpy(&pr, &port, sizeof(pr));
                            float const portL = pr.left;
                            float const portT = pr.top;
                            float const portW = pr.right - pr.left;
                            float const portH = pr.bottom - pr.top;
                            float nx = a_px, ny = a_py;
                            if (portW > 10.0f)
                            {
                                nx = (a_px - portL) / portW;
                            }
                            if (portH > 10.0f)
                            {
                                ny = (a_py - portT) / portH;
                            }
                            // 引擎函数输出为“左下原点”归一化坐标（TrueDirectionalMovement 同样处理），翻转为左上原点
                            a_px = nx * w;
                            a_py = (1.0f - ny) * h;
                            ok = a_depth > 0.0f;
                        } else if (viewData &&
                                   (viewData->viewProjMatrixUnjittered._11 != 0.0f || viewData->viewProjMat._11 != 0.0f))
                        {
                            auto const& viewProj = viewData->viewProjMatrixUnjittered._11 != 0.0f ? viewData->viewProjMatrixUnjittered : viewData->viewProjMat;
                            DirectX::XMVECTOR const clip = DirectX::XMVector4Transform(
                                DirectX::XMVectorSet(a_pt.x, a_pt.y, a_pt.z, 1.0f),
                                viewProj);
                            float const clipW = DirectX::XMVectorGetW(clip);
                            if (std::fabs(clipW) >= 1e-5f)
                            {
                                DirectX::XMVECTOR const ndc = DirectX::XMVectorDivide(clip, DirectX::XMVectorReplicate(clipW));
                                float const ndcX = DirectX::XMVectorGetX(ndc);
                                float const ndcY = DirectX::XMVectorGetY(ndc);
                                float const ndcZ = DirectX::XMVectorGetZ(ndc);
                                if (ndcX >= -1.0f && ndcX <= 1.0f && ndcY >= -1.0f && ndcY <= 1.0f && ndcZ >= 0.0f && ndcZ <= 1.0f)
                                {
                                    a_px = (ndcX * 0.5f + 0.5f) * w;
                                    a_py = (1.0f - ndcY) * 0.5f * h;
                                    a_depth = ndcZ;
                                    ok = true;
                                }
                            }
                        }
                        return ok;
                    };

                    // 尸体世界 AABB 是否有效
                    bool const hasAABB =
                        corpse.boundMin.x <= corpse.boundMax.x &&
                        corpse.boundMin.y <= corpse.boundMax.y &&
                        corpse.boundMin.z <= corpse.boundMax.z;

                    // 把一批世界点投影到屏幕，取屏幕包围矩形
                    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
                    float depthSum = 0.0f;
                    int depthCount = 0;
                    bool hasRect = false;

                    // OBB 线框盒：记录 8 个角各自的屏幕坐标
                    float projX[8]{}, projY[8]{};
                    bool obbOk[8]{};

                    auto addProjected = [&](RE::NiPoint3 const& a_pt, int a_idx = -1) {
                        float px = 0.0f, py = 0.0f, d = 0.0f;
                        if (projectToScreen(a_pt, px, py, d))
                        {
                            minX = std::min(minX, px);
                            maxX = std::max(maxX, px);
                            minY = std::min(minY, py);
                            maxY = std::max(maxY, py);
                            depthSum += d;
                            ++depthCount;
                            hasRect = true;
                            if (a_idx >= 0 && a_idx < 8)
                            {
                                projX[a_idx] = px;
                                projY[a_idx] = py;
                                obbOk[a_idx] = true;
                            }
                        }
                    };

                    bool obbAllOk = true;
                    if (corpse.hasOBB)
                    {
                        // 投影碰撞盒（OBB）的 8 个世界角点：屏幕矩形贴合碰撞盒的屏幕足迹
                        for (int i = 0; i < 8; ++i)
                        {
                            addProjected(corpse.obbCorners[i], i);
                        }
                        for (bool ok : obbOk)
                        {
                            obbAllOk = obbAllOk && ok;
                        }
                    } else if (hasAABB)
                    {
                        // 投影 AABB 的 8 个角，得到贴合尸体包围盒的屏幕矩形
                        RE::NiPoint3 const corners[8] = {
                            { corpse.boundMin.x, corpse.boundMin.y, corpse.boundMin.z },
                            { corpse.boundMax.x, corpse.boundMin.y, corpse.boundMin.z },
                            { corpse.boundMin.x, corpse.boundMax.y, corpse.boundMin.z },
                            { corpse.boundMax.x, corpse.boundMax.y, corpse.boundMin.z },
                            { corpse.boundMin.x, corpse.boundMin.y, corpse.boundMax.z },
                            { corpse.boundMax.x, corpse.boundMin.y, corpse.boundMax.z },
                            { corpse.boundMin.x, corpse.boundMax.y, corpse.boundMax.z },
                            { corpse.boundMax.x, corpse.boundMax.y, corpse.boundMax.z },
                        };
                        for (auto const& corner : corners)
                        {
                            addProjected(corner);
                        }
                    } else
                    {
                        // 兜底：没有 AABB 时用锚点 + 世界半径投影上下左右
                        RE::NiPoint3 camRight{ 1.0f, 0.0f, 0.0f };
                        RE::NiPoint3 camUp{ 0.0f, 1.0f, 0.0f };
                        if (worldCam)
                        {
                            camRight = worldCam->world.rotate.GetVectorX();
                            camUp = worldCam->world.rotate.GetVectorY();
                        }
                        addProjected(corpse.anchor);
                        addProjected(corpse.anchor + camRight * corpse.radius);
                        addProjected(corpse.anchor - camRight * corpse.radius);
                        addProjected(corpse.anchor + camUp * corpse.radius);
                        addProjected(corpse.anchor - camUp * corpse.radius);
                    }

                    if (!hasRect)
                    {
                        continue;
                    }

                    // 矩形与中心
                    float const sx = (minX + maxX) * 0.5f;
                    float const sy = (minY + maxY) * 0.5f;
                    float const boxW = maxX - minX;
                    float const boxH = maxY - minY;
                    // 保证最小可见尺寸（太远时不会缩成一个点）
                    constexpr float kMinBox = 5.0f;
                    float x0 = minX, y0 = minY, x1 = maxX, y1 = maxY;
                    if (boxW < kMinBox || boxH < kMinBox)
                    {
                        float const halfX = std::max(boxW * 0.5f, kMinBox * 0.5f);
                        float const halfY = std::max(boxH * 0.5f, kMinBox * 0.5f);
                        x0 = sx - halfX;
                        x1 = sx + halfX;
                        y0 = sy - halfY;
                        y1 = sy + halfY;
                    }

                    // 发光/中心点用的半径（取矩形面积的等效半径）
                    float const radiusPx = std::clamp(std::sqrt(std::max(boxW * boxH, 1.0f)) * 0.5f, kMinBox, 300.0f);

                    // 距离衰减：FadeStartDistance 内完全不透明；超过后按
                    // FadePower 指数衰减，到 MaxDistance 处达到 MinOpacity 下限。
                    // 远处尸体的 box 边框越来越"虚"，近处保持清晰。
                    float const fadeRange = std::max(cfg.maxDistance - cfg.fadeStartDistance, 1.0f);
                    float const f = corpse.distance <= cfg.fadeStartDistance ? 1.0f : 1.0f - std::clamp((corpse.distance - cfg.fadeStartDistance) / fadeRange, 0.0f, 1.0f);
                    float const fade = std::pow(f, cfg.fadePower);
                    float const alpha = std::clamp(
                        cfg.minOpacity + fade * (1.0f - cfg.minOpacity),
                        cfg.minOpacity,
                        1.0f);

                    float const half = radiusPx * 1.15f;

                    if (cfg.showGlow)
                    {
                        // 多层外扩发光（默认关闭，仅保留边框时可忽略）
                        for (int i = 4; i >= 1; --i)
                        {
                            float const grow = half * (1.0f + 0.25f * static_cast<float>(i));
                            float const ga = alpha * cfg.glowAlpha / static_cast<float>(i);
                            DrawFilledRect(
                                sx - grow, sy - grow, sx + grow, sy + grow,
                                DirectX::XMFLOAT4{ cr, cg, cb, ga });
                        }
                    }

                    // 有方向碰撞盒（OBB）且屏幕尺寸足够大时，画 12 条边的 3D 线框盒，
                    // 与尸体碰撞盒逐边重合；否则退化为 AABB 屏幕矩形。
                    bool const useWireframe =
                        corpse.hasOBB && obbAllOk && boxW >= kMinBox && boxH >= kMinBox;

                    if (cfg.showOutline)
                    {
                        if (useWireframe)
                        {
                            static constexpr std::int32_t kEdges[12][2] = {
                                { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },  // 底面
                                { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },  // 顶面
                                { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },  // 竖边
                            };
                            auto const col = DirectX::XMFLOAT4{ cr, cg, cb, alpha };
                            for (auto const& e : kEdges)
                            {
                                DrawThickLine(
                                    DirectX::XMFLOAT2{ projX[e[0]], projY[e[0]] },
                                    DirectX::XMFLOAT2{ projX[e[1]], projY[e[1]] },
                                    cfg.outlineThickness,
                                    col);
                            }
                        } else
                        {
                            DrawRectOutline(
                                x0, y0, x1, y1, cfg.outlineThickness,
                                DirectX::XMFLOAT4{ cr, cg, cb, alpha });
                        }
                    }

                    if (cfg.showCenterDot)
                    {
                        float const d = std::max(2.0f, radiusPx * 0.12f);
                        DrawFilledRect(
                            sx - d, sy - d, sx + d, sy + d,
                            DirectX::XMFLOAT4{ cr, cg, cb, std::min(1.0f, alpha + 0.2f) });
                    }
                }
            }
        }

        g_batch->End();

        // ---- 恢复游戏渲染状态 ----
        context->OMSetRenderTargets(1, &prevRTV, prevDSV);
        context->OMSetBlendState(prevBlend, blendFactor, sampleMask);
        context->OMSetDepthStencilState(prevDepth, prevStencil);
        context->RSSetState(prevRS);

        if (prevRTV)
        {
            prevRTV->Release();
        }
        if (prevDSV)
        {
            prevDSV->Release();
        }
        if (prevBlend)
        {
            prevBlend->Release();
        }
        if (prevDepth)
        {
            prevDepth->Release();
        }
        if (prevRS)
        {
            prevRS->Release();
        }
    }
}
