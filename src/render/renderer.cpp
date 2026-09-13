#include "renderer.h"
#include "outline_mask.h"
#include "config/config.h"
#include "search/corpse_finder.h"

#include <d3d11.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <CommonStates.h>
#include <d3dcompiler.h>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    // ---------------------------------------------------------------------------
    // 绘制资源（懒创建，渲染线程独占）
    // ---------------------------------------------------------------------------
    std::unique_ptr<DirectX::CommonStates> g_states;

    ID3D11RenderTargetView* g_back_buffer_rtv = nullptr;
    ID3D11Texture2D* g_back_buffer = nullptr;
    std::uint32_t g_back_w = 0;  // 后台缓冲真实尺寸（来自交换链纹理描述）
    std::uint32_t g_back_h = 0;
    DXGI_FORMAT g_back_buffer_format = DXGI_FORMAT_UNKNOWN;

    // ---------------------------------------------------------------------------
    // 扫描调度（渲染线程计时，游戏线程执行）
    // ---------------------------------------------------------------------------
    std::chrono::steady_clock::time_point g_last_scan;
    std::atomic<bool> g_scan_in_flight = false;

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
            return false;

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
            logger::warn(
                "Back buffer format is {} (expected B8G8R8A8_UNORM=87): HDR swap chain. "
                "R10G10B10A2's alpha channel is only 2-bit (4 levels), overlay opacity is quantized.",
                static_cast<int>(g_back_buffer_format));

        HRESULT const rtv_hr = a_device->CreateRenderTargetView(buffer, nullptr, &g_back_buffer_rtv);
        if (FAILED(rtv_hr) || !g_back_buffer_rtv)
        {
            logger::error("Failed to create backbuffer RTV: {:X}", static_cast<unsigned int>(rtv_hr));
            return false;
        }
        return true;
    }


    // ---------------------------------------------------------------------------
    // 自绘管线：自编译 VS/PS + 自建 InputLayout/顶点缓冲。
    // DirectXTK 的 BasicEffect/PrimitiveBatch 在本机环境（Skyrim 1.6.1170 +
    // ReShade/渲染链）下顶点颜色错乱（实测写入红绿蓝显示绿红红、box 变红），
    // 故整体绘制迁移到此管线。顶点位置在 CPU 端直接换算为 NDC。
    //
    // 混合约定（重要）：OM 绑定的 DirectXTK CommonStates::AlphaBlend() 是
    // (SrcBlend=ONE, DestBlend=INV_SRC_ALPHA) 的“预乘 alpha”混合，因此 PS 必须
    // 输出 rgb*a 的预乘颜色。此前 PS 直出直 alpha 颜色，rgb 以全强度（×ONE）叠加，
    // alpha 只控制背景透出比例，导致 alpha=0.5 的方块视觉上仍是实心——
    // 这就是“边框透明度不对/距离衰减无效”的根因。
    //
    // 另：本机后备缓冲为 R10G10B10A2_UNORM（HDR，DXGI 格式 24），alpha 仅 2 bit，
    // (1-a) 侧被量化为 4 级，半透明精度天然受限（见 ensure_back_buffer 的告警）。
    // ---------------------------------------------------------------------------
    struct UiVertex
    {
        float x, y, z;
        float r, g, b, a;
    };

    // 顶点缓冲容量：GPU 侧固定大小，CPU 侧提交前必须按此裁剪（否则 memcpy 越界写映射区）
    constexpr std::size_t Vertex_Buffer_Bytes = 1024 * 1024;
    constexpr std::size_t Max_Vertices = Vertex_Buffer_Bytes / sizeof(UiVertex);

    ID3D11VertexShader* g_ui_vs = nullptr;
    ID3D11PixelShader* g_ui_ps = nullptr;
    ID3D11InputLayout* g_ui_layout = nullptr;
    ID3D11Buffer* g_ui_vb = nullptr;
    bool g_ui_ready = false;
    bool g_ui_failed = false;          // 创建失败后不再每帧重试（重试会持续泄漏 D3D 对象）
    std::vector<UiVertex> g_ui_verts;  // 渲染线程独占：一帧内累积，flush 统一提交

    // 绘制互斥：日志实测 on_present 会被多个线程并发进入（Present hook 触发线程
    // 与游戏渲染线程交替），而绘制共享 g_ui_verts/g_ui_vb/backbuffer 等状态，
    // 并发下导致绘制错乱/不显示/透明度异常，故整个绘制段串行化。
    std::mutex g_draw_mutex;

    ID3DBlob* compile(char const* a_source, char const* a_target, char const* a_name)
    {
        if (!a_source || !a_target)
            return nullptr;

        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT const hr = D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr, nullptr, "main", a_target, 0, 0, &blob, &err);
        if (FAILED(hr))
        {
            logger::error(
                "Shader compile failed [{} {}] ({:X}): {}",
                a_name ? a_name : "?",
                a_target,
                static_cast<unsigned int>(hr),
                err ? static_cast<char const*>(err->GetBufferPointer()) : "no diagnostics");
            if (blob)
            {
                blob->Release();
                blob = nullptr;
            }
        }
        if (err)
            err->Release();

        return blob;
    }

    void release_ui_pipeline()
    {
        if (g_ui_vb)
        {
            g_ui_vb->Release();
            g_ui_vb = nullptr;
        }
        if (g_ui_layout)
        {
            g_ui_layout->Release();
            g_ui_layout = nullptr;
        }
        if (g_ui_ps)
        {
            g_ui_ps->Release();
            g_ui_ps = nullptr;
        }
        if (g_ui_vs)
        {
            g_ui_vs->Release();
            g_ui_vs = nullptr;
        }
    }

    void ensure_ui_pipeline(ID3D11Device* a_device)
    {
        if (g_ui_ready || g_ui_failed)
            return;

        char const* vs_src = R"(
            struct VS_IN
            {
                float3 pos : POSITION;
                float4 color : COLOR;
            };
            struct PS_IN
            {
                float4 pos : SV_Position;
                float4 color : COLOR;
            };

            PS_IN main(VS_IN input)
            {
                PS_IN output;
                output.pos = float4(input.pos, 1.0f);
                output.color = input.color;
                return output;
            }
        )";
        char const* ps_src = R"(
            struct PS_IN
            {
                float4 pos : SV_Position;
                float4 color : COLOR;
            };

            float4 main(PS_IN input) : SV_Target
            {
                // OM 的混合状态是 CommonStates::AlphaBlend()（ONE/INV_SRC_ALPHA，
                // 预乘 alpha 约定）：必须输出预乘颜色（rgb*a），否则 rgb 全强度
                // 叠加、alpha 失效（半透明方块显示为实心）。
                return float4(input.color.rgb * input.color.a, input.color.a);
            }
        )";

        ID3DBlob* vs_blob = compile(vs_src, "vs_5_0", "esp quad");
        ID3DBlob* ps_blob = compile(ps_src, "ps_5_0", "esp quad");
        if (!vs_blob || !ps_blob)
        {
            if (vs_blob)
                vs_blob->Release();
            if (ps_blob)
                ps_blob->Release();

            g_ui_failed = true;
            return;
        }
        a_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_ui_vs);
        a_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &g_ui_ps);

        constexpr D3D11_INPUT_ELEMENT_DESC Layout_Desc[] = {
            {
                .SemanticName = "POSITION",
                .SemanticIndex = 0,
                .Format = DXGI_FORMAT_R32G32B32_FLOAT,
                .InputSlot = 0, 
                .AlignedByteOffset = 0,
                .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
                .InstanceDataStepRate = 0
            },
            {
                .SemanticName = "COLOR",
                .SemanticIndex = 0,
                .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                .InputSlot = 0,
                .AlignedByteOffset = 12,
                .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
                .InstanceDataStepRate = 0
            },
        };
        a_device->CreateInputLayout(Layout_Desc, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &g_ui_layout);

        vs_blob->Release();
        ps_blob->Release();

        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = static_cast<UINT>(Vertex_Buffer_Bytes);  // Max_Vertices 个顶点，提交前按此裁剪
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        a_device->CreateBuffer(&bd, nullptr, &g_ui_vb);

        g_ui_ready = g_ui_vs && g_ui_ps && g_ui_layout && g_ui_vb;
        if (!g_ui_ready)
        {
            g_ui_failed = true;
            release_ui_pipeline();
            logger::error("UI pipeline creation failed, ESP rendering disabled");
            return;
        }
        logger::info("UI pipeline ready ({} vertices max)", Max_Vertices);
    }

    // icon 模式的实心小圆（契约 v13）：三角扇 16 段直接推入 g_ui_verts
    //（48 顶点/尸体，预算 Max_Vertices 充裕）。半径固定像素值，不做距离缩放。
    constexpr float Icon_Radius = 10.0f;  // 像素（契约 v14：6 → 10）
    constexpr int Icon_Segments = 16;     // 三角扇段数
    constexpr float Icon_Two_Pi = 6.283185307179586f;

    void draw_icon_circle(float a_cx, float a_cy, DirectX::XMFLOAT4 const& a_color)
    {
        if (g_back_w == 0 || g_back_h == 0)
            return;

        float const inv_w = 2.0f / static_cast<float>(g_back_w);
        float const inv_h = 2.0f / static_cast<float>(g_back_h);
        float const ndc_cx = a_cx * inv_w - 1.0f;
        float const ndc_cy = 1.0f - a_cy * inv_h;
        float const ndc_rx = Icon_Radius * inv_w;  // 像素半径分别换算，保持屏幕上为圆
        float const ndc_ry = Icon_Radius * inv_h;

        for (int k = 0; k < Icon_Segments; ++k)
        {
            float const a0 = Icon_Two_Pi * static_cast<float>(k) / static_cast<float>(Icon_Segments);
            float const a1 = Icon_Two_Pi * static_cast<float>(k + 1) / static_cast<float>(Icon_Segments);
            float const x0 = ndc_cx + std::cos(a0) * ndc_rx;
            float const y0 = ndc_cy + std::sin(a0) * ndc_ry;
            float const x1 = ndc_cx + std::cos(a1) * ndc_rx;
            float const y1 = ndc_cy + std::sin(a1) * ndc_ry;
            // UiVertex 预乘约定：color.a 传直 alpha，UI PS 输出 rgb*a
            g_ui_verts.emplace_back(ndc_cx, ndc_cy, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w);
            g_ui_verts.emplace_back(x0, y0, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w);
            g_ui_verts.emplace_back(x1, y1, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w);
        }
    }

    void ensure_draw_resources(ID3D11Device* a_device)
    {
        if (!g_states)
            g_states = std::make_unique<DirectX::CommonStates>(a_device);

        ensure_ui_pipeline(a_device);
    }

    // 把本帧累积的四边形统一提交（一次 Map + 一次 Draw）
    void flush_ui_quads(ID3D11DeviceContext* a_context)
    {
        if (!g_ui_ready || g_ui_verts.empty())
            return;

        // GPU 缓冲容量固定：超出部分整三角形丢弃，绝不能按实际顶点数 memcpy
        std::size_t count = std::min(g_ui_verts.size(), Max_Vertices);
        count -= count % 3;
        if (g_ui_verts.size() > Max_Vertices)
        {
            static bool s_overflow_reported = false;
            if (!s_overflow_reported)
            {
                s_overflow_reported = true;
                logger::warn("UI vertex buffer full: {} of {} vertices dropped this frame", g_ui_verts.size() - count, g_ui_verts.size());
            }
        }
        if (count == 0)
        {
            g_ui_verts.clear();
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(a_context->Map(g_ui_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            g_ui_verts.clear();
            return;
        }
        std::memcpy(mapped.pData, g_ui_verts.data(), count * sizeof(UiVertex));
        a_context->Unmap(g_ui_vb, 0);

        constexpr UINT stride = sizeof(UiVertex);
        constexpr UINT offset = 0;
        a_context->IASetInputLayout(g_ui_layout);
        a_context->IASetVertexBuffers(0, 1, &g_ui_vb, &stride, &offset);
        a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        a_context->VSSetShader(g_ui_vs, nullptr, 0);
        a_context->PSSetShader(g_ui_ps, nullptr, 0);
        a_context->Draw(static_cast<UINT>(count), 0);

        g_ui_verts.clear();
    }

    // 距离衰减：FadeStartDistance 内完全不透明；超过后按 FadePower 指数衰减，
    // 到 MaxDistance 处达到 MinOpacity 下限。
    // 前置条件：min_opacity ∈ [0,1]、max_distance > 0（Config::load 已规范化）。
    [[nodiscard]] float corpse_alpha(Config const& a_cfg, float a_distance)
    {
        float const fade_range = std::max(a_cfg.max_distance - a_cfg.fade_start_distance, 1.0f);
        float const f = a_distance <= a_cfg.fade_start_distance ? 1.0f : 1.0f - std::clamp((a_distance - a_cfg.fade_start_distance) / fade_range, 0.0f, 1.0f);
        float const fade = std::pow(f, a_cfg.fade_power);
        return std::clamp(a_cfg.min_opacity + fade * (1.0f - a_cfg.min_opacity), a_cfg.min_opacity, 1.0f);
    }

    void on_present(IDXGISwapChain* a_swapChain)
    {
        // 整个绘制段串行化（见 g_draw_mutex 注释：多线程并发进入 Present hook）
        std::lock_guard<std::mutex> const draw_lock(g_draw_mutex);


        // 定时派发尸体扫描任务到游戏线程；在途守卫保证同一时刻最多一个
        // 扫描任务排队或执行中，避免扫描堆积（exchange 置位成功才派发）。
        std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
        if (now - g_last_scan >= std::chrono::milliseconds(Setting::get_config().scan_interval_ms) &&
            !g_scan_in_flight.exchange(true))
        {
            g_last_scan = now;
            SKSE::GetTaskInterface()->AddTask([] {
                CorpseScan::search();
                g_scan_in_flight.store(false);
            });
        }

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

        static std::uint32_t s_last_drawn_frame = std::numeric_limits<std::uint32_t>::max();
        bool const skip_draw = (frame != 0) && (frame == s_last_drawn_frame);

        if (!skip_draw)
        {
            if (frame != 0)
                s_last_drawn_frame = frame;

            if (!ensure_back_buffer(a_swapChain, device))
                return;

            ensure_draw_resources(device);
            if (!g_states || !g_ui_ready)
                return;

            float w = static_cast<float>(g_back_w);
            float h = static_cast<float>(g_back_h);
            if (w <= 0.0f || h <= 0.0f)
            {
                // 后备：渲染器报告的屏幕尺寸
                RE::BSGraphics::ScreenSize const screen = RE::BSGraphics::Renderer::GetScreenSize();
                w = static_cast<float>(screen.width);
                h = static_cast<float>(screen.height);
            }
            if (w <= 0.0f || h <= 0.0f)
                return;

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

            // ---- 尸体 ESP 标记 ----
            Config const& cfg = Setting::get_config();
            if (cfg.enabled)
            {
                // 相机对象：世界根相机（Present 时仍是本帧数据），用于投影
                RE::NiCamera* world_cam = RE::Main::WorldRootCamera();

                // 备选：BSGraphics::State 相机缓存里的 viewProj 矩阵
                // （实测在 AE 上该矩阵读出的是坏值：_22=0、_43=0，故仅作兜底保留）
                RE::BSGraphics::State* state = RE::BSGraphics::State::GetSingleton();
                RE::BSGraphics::ViewData const* view_data = nullptr;
                if (state)
                {
                    RE::BSGraphics::State::RUNTIME_DATA& state_rt = state->GetRuntimeData();
                    for (RE::BSGraphics::CameraStateData const& cam_data : state_rt.cameraDataCacheA)
                    {
                        if (cam_data.referenceCamera == world_cam)
                        {
                            view_data = std::addressof(cam_data.GetCameraStateRuntimeData().camViewData);
                            break;
                        }
                    }
                    if (!view_data && !state_rt.cameraDataCacheA.empty())
                        view_data = std::addressof(state_rt.cameraDataCacheA.front().GetCameraStateRuntimeData().camViewData);
                }

                uint32_t const rgb = cfg.outline_color;
                float const cr = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
                float const cg = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
                float const cb = static_cast<float>(rgb & 0xFF) / 255.0f;

                std::vector<CorpseScan::CorpseInfo> const corpses = CorpseScan::snapshot();

                // ---- 投影函数：世界点 -> 屏幕像素（左上原点），成功返回 true ----
                // 优先用引擎 NiCamera::WorldPtToScreenPt3（返回左下原点归一化坐标），
                // 失败时兜底用 State 的 viewProj 矩阵。
                auto project_to_screen = [&](RE::NiPoint3 const& a_pt, float& a_px, float& a_py, float& a_depth) -> bool
                {
                    bool ok = false;
                    if (world_cam && world_cam->WorldPtToScreenPt3(a_pt, a_px, a_py, a_depth, 1e-5f))
                    {
                        // 相机 port 若是像素单位，先把输出归一化到 0..1
                        RE::NiRect<float> const& port = world_cam->GetRuntimeData2().port;
                        PortRect pr{};
                        std::memcpy(&pr, &port, sizeof(pr));
                        float const port_l = pr.left;
                        float const port_t = pr.top;
                        float const port_w = pr.right - pr.left;
                        float const port_h = pr.bottom - pr.top;
                        float nx = a_px, ny = a_py;
                        if (port_w > 10.0f)
                            nx = (a_px - port_l) / port_w;

                        if (port_h > 10.0f)
                            ny = (a_py - port_t) / port_h;

                        // 引擎函数输出为“左下原点”归一化坐标（TrueDirectionalMovement 同样处理），翻转为左上原点
                        a_px = nx * w;
                        a_py = (1.0f - ny) * h;
                        ok = a_depth > 0.0f;
                    }
                    else if (view_data && (view_data->viewProjMatrixUnjittered._11 != 0.0f || view_data->viewProjMat._11 != 0.0f))
                    {
                        Matrix const& viewProj = view_data->viewProjMatrixUnjittered._11 != 0.0f ? view_data->viewProjMatrixUnjittered : view_data->viewProjMat;
                        DirectX::XMVECTOR const clip = DirectX::XMVector4Transform(DirectX::XMVectorSet(a_pt.x, a_pt.y, a_pt.z, 1.0f), viewProj);
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

                // ---- icon 模式（契约 v14 R-03）：每 corpse 只投影固定世界锚点 ----
                // anchor 是固定世界点（bounds/worldBound 中心，见 corpse_finder.cpp），
                // 投影随视角平滑连续，无 v13 投影包围盒几何中心的视角摆动。silhouette/
                // outline 模式由 mask pass 负责显示，无逐 corpse 工作。
                if (cfg.display_mode == Config::DisplayMode::e_icon)
                {
                    for (CorpseScan::CorpseInfo const& corpse : corpses)
                    {
                        float px = 0.0f, py = 0.0f, d = 0.0f;
                        if (!project_to_screen(corpse.anchor, px, py, d))
                            continue;  // 相机后方/投影失败 → 跳过该 corpse

                        float const alpha = corpse_alpha(cfg, corpse.distance);
                        DirectX::XMFLOAT4 const color = {cr, cg, cb, alpha};
                        draw_icon_circle(px, py, color);
                    }
                }

                // ---- mask 渲染（穿墙剪影/描边带的输入）----
                // 目标由本帧尸体快照的 form_id 解析为引用。icon 模式下 mask 无消费者，
                // 整段跳过；silhouette/outline 模式由 outline_mask.cpp 按 display_mode
                // 选择消费 pass（内部填充叠加 / 外描边带）。
                if (!corpses.empty() && cfg.display_mode != Config::DisplayMode::e_icon)
                {
                    // R-01（契约 v15/v18）：目标携带距离衰减不透明度（corpse_alpha），
                    // 按目标序号填入消费 pass 的 per-frame alpha LUT。
                    std::vector<OutlineMaskTarget> mask_targets;
                    mask_targets.reserve(corpses.size());
                    for (CorpseScan::CorpseInfo const& corpse : corpses)
                    {
                        if (RE::TESForm* form = RE::TESForm::LookupByID(corpse.form_id))
                        {
                            if (RE::TESObjectREFR* ref = form->AsReference())
                                mask_targets.push_back({ ref, corpse_alpha(cfg, corpse.distance) });
                        }
                    }
                    OutlineMask::set_targets(mask_targets);
                    OutlineMask::render(device, context, world_cam, g_back_w, g_back_h);
                }
            }

            flush_ui_quads(context);

            // ---- 恢复游戏渲染状态 ----
            context->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
            context->OMSetBlendState(prev_blend, blend_factor, sample_mask);
            context->OMSetDepthStencilState(prev_depth, prev_stencil);
            context->RSSetState(prev_rs);

            if (prev_rtv)
                prev_rtv->Release();

            if (prev_dsv)
                prev_dsv->Release();

            if (prev_blend)
                prev_blend->Release();

            if (prev_depth)
                prev_depth->Release();

            if (prev_rs)
                prev_rs->Release();
        }  // if (!skip_draw)
    }

    // ---------------------------------------------------------------------------
    // IDXGISwapChain::Present vtable 钩子（vtable 第 8 槽位）
    // 运行时无关：不依赖 Address Library ID，任何 AE 版本都有效
    // ---------------------------------------------------------------------------
    using PresentFunc = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    PresentFunc g_original_present = nullptr;
    void** g_hooked_slot = nullptr;

    HRESULT STDMETHODCALLTYPE present_thunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
    {
        on_present(a_swapChain);
        return g_original_present(a_swapChain, a_syncInterval, a_flags);
    }
}

void Renderer::install()
{
    if (g_hooked_slot)
        return;  // 已安装

    RE::BSGraphics::Renderer* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
        return;

    RE::BSGraphics::RendererData& rt = renderer->GetRuntimeData();
    if (!rt.renderWindows[0].swapChain)
    {
        logger::warn("SwapChain not available yet, will retry on next game message");
        return;
    }

    IDXGISwapChain* swap_chain = reinterpret_cast<IDXGISwapChain*>(rt.renderWindows[0].swapChain);
    void** vtable = *reinterpret_cast<void***>(swap_chain);

    // IDXGISwapChain::Present 是虚函数表中第 8 个槽位
    g_hooked_slot = &vtable[8];
    g_original_present = reinterpret_cast<PresentFunc>(*g_hooked_slot);

    DWORD old_protect = 0;
    if (!VirtualProtect(g_hooked_slot, sizeof(void*), PAGE_READWRITE, &old_protect))
    {
        logger::error("VirtualProtect failed, cannot install Present hook");
        g_hooked_slot = nullptr;
        g_original_present = nullptr;
        return;
    }
    *g_hooked_slot = reinterpret_cast<void*>(&present_thunk);
    VirtualProtect(g_hooked_slot, sizeof(void*), old_protect, &old_protect);

    logger::info("Installed IDXGISwapChain::Present hook (swap chain={}, original={})", fmt::ptr(swap_chain), fmt::ptr(g_original_present));
}

PLUGIN_NAMESPACE_END
