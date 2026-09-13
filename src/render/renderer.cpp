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

    // 累积一个屏幕空间四边形（像素坐标）到本帧顶点列表（TRIANGLELIST：2 三角形 6 顶点）
    void draw_quad(float a_x0, float a_y0, float a_x1, float a_y1, DirectX::XMFLOAT4 const& a_color)
    {
        if (g_back_w == 0 || g_back_h == 0)
            return;

        float const ndc_x0 = a_x0 * 2.0f / static_cast<float>(g_back_w) - 1.0f;
        float const ndc_x1 = a_x1 * 2.0f / static_cast<float>(g_back_w) - 1.0f;
        float const ndc_y0 = 1.0f - a_y0 * 2.0f / static_cast<float>(g_back_h);
        float const ndc_y1 = 1.0f - a_y1 * 2.0f / static_cast<float>(g_back_h);

        g_ui_verts.emplace_back(ndc_x0, ndc_y0, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v0
        g_ui_verts.emplace_back(ndc_x1, ndc_y0, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v1
        g_ui_verts.emplace_back(ndc_x1, ndc_y1, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v2
        g_ui_verts.emplace_back(ndc_x0, ndc_y0, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v0
        g_ui_verts.emplace_back(ndc_x1, ndc_y1, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v2
        g_ui_verts.emplace_back(ndc_x0, ndc_y1, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v3
    }

    void ensure_draw_resources(ID3D11Device* a_device)
    {
        if (!g_states)
            g_states = std::make_unique<DirectX::CommonStates>(a_device);

        ensure_ui_pipeline(a_device);
    }

    // 任意四点凸四边形（屏幕像素坐标），TRIANGLELIST 展开
    void draw_quad_corners(float a_x0, float a_y0, float a_x1, float a_y1, float a_x2, float a_y2, float a_x3, float a_y3, DirectX::XMFLOAT4 const& a_color)
    {
        if (g_back_w == 0 || g_back_h == 0)
            return;

        float const inv_w = 2.0f / static_cast<float>(g_back_w);
        float const inv_h = 2.0f / static_cast<float>(g_back_h);
        auto const to_ndc = [&](float px, float py) {
            return std::pair{ px * inv_w - 1.0f, 1.0f - py * inv_h };
        };
        auto const [nx0, ny0] = to_ndc(a_x0, a_y0);
        auto const [nx1, ny1] = to_ndc(a_x1, a_y1);
        auto const [nx2, ny2] = to_ndc(a_x2, a_y2);
        auto const [nx3, ny3] = to_ndc(a_x3, a_y3);

        g_ui_verts.emplace_back(nx0, ny0, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v0
        g_ui_verts.emplace_back(nx1, ny1, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v1
        g_ui_verts.emplace_back(nx2, ny2, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v2
        g_ui_verts.emplace_back(nx0, ny0, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v0
        g_ui_verts.emplace_back(nx2, ny2, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v2
        g_ui_verts.emplace_back(nx3, ny3, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w); // v3
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

    void draw_filled_rect(float a_x0, float a_y0, float a_x1, float a_y1, DirectX::XMFLOAT4 const& a_color)
    {
        draw_quad(a_x0, a_y0, a_x1, a_y1, a_color);
    }

    void draw_rect_outline(float a_x0, float a_y0, float a_x1, float a_y1, float a_thickness, DirectX::XMFLOAT4 const& a_color)
    {
        // 线宽超过盒子半宽/半高时上下左右四条会反转并互相重叠，alpha 被混合两次（显示为实心）
        float const half_extent = std::min(a_x1 - a_x0, a_y1 - a_y0) * 0.5f;
        float const t = std::max(std::min(a_thickness, half_extent), 1.0f);
        draw_filled_rect(a_x0, a_y0, a_x1, a_y0 + t, a_color);          // 上
        draw_filled_rect(a_x0, a_y1 - t, a_x1, a_y1, a_color);          // 下
        draw_filled_rect(a_x0, a_y0 + t, a_x0 + t, a_y1 - t, a_color);  // 左
        draw_filled_rect(a_x1 - t, a_y0 + t, a_x1, a_y1 - t, a_color);  // 右
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
        // 旋转四边形（凸环绕顺序：p0+n, p0-n, p1-n, p1+n；此前 p1 两点顺序颠倒
        // 导致 Z 形自交，两三角形重叠区 alpha 被混合两次，边框看似不透明）
        draw_quad_corners(
            a_p0.x + nx, a_p0.y + ny,
            a_p0.x - nx, a_p0.y - ny,
            a_p1.x - nx, a_p1.y - ny,
            a_p1.x + nx, a_p1.y + ny,
            a_color);
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
                for (CorpseScan::CorpseInfo const& corpse : corpses)
                {
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

                    auto add_projected = [&](RE::NiPoint3 const& a_pt, int a_idx = -1)
                    {
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
                            add_projected(corpse.obb_corners[i], i);

                        for (bool ok : obb_ok)
                            obb_all_ok = obb_all_ok && ok;
                    }
                    else if (hasAABB)
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
                        for (RE::NiPoint3 const& corner : corners)
                            add_projected(corner);
                    }
                    else
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
                        continue;

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

                    float const alpha = corpse_alpha(cfg, corpse.distance);

                    // 有方向碰撞盒（OBB）且屏幕尺寸足够大时，画 12 条边的 3D 线框盒，
                    // 与尸体碰撞盒逐边重合；否则退化为 AABB 屏幕矩形。
                    bool const use_wireframe = corpse.has_obb && obb_all_ok && box_w >= kMinBox && box_h >= kMinBox;

                    DirectX::XMFLOAT4 const color = {cr, cg, cb, alpha};

                    if (use_wireframe)
                    {
                        static constexpr std::int32_t kEdges[12][2] = {
                            { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },  // 底面
                            { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },  // 顶面
                            { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },  // 竖边
                        };
                        for (auto const& [e0, e1] : kEdges)
                        {
                            draw_thick_line(
                                DirectX::XMFLOAT2{ proj_x[e0], proj_y[e0] },
                                DirectX::XMFLOAT2{ proj_x[e1], proj_y[e1] },
                                cfg.outline_thickness,
                                color);
                        }
                    }
                    else
                        draw_rect_outline(x0, y0, x1, y1, cfg.outline_thickness, color);
                }

                // ---- 描边 mask（穿墙剪影，后续边缘检测 pass 的输入）----
                // 目标由本帧尸体快照的 form_id 解析为引用；仅当调试开关开启时
                // 渲染并叠加（mask 目前无其他消费者）。调试叠加画在本帧画面之上，
                // 现有线框绘制路径的行为与顺序不变。
                if (!corpses.empty())
                {
                    std::vector<RE::TESObjectREFR*> mask_targets;
                    mask_targets.reserve(corpses.size());
                    for (CorpseScan::CorpseInfo const& corpse : corpses)
                    {
                        if (RE::TESForm* form = RE::TESForm::LookupByID(corpse.form_id))
                            if (RE::TESObjectREFR* ref = form->AsReference())
                                mask_targets.push_back(ref);
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
