//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <d3d11.h>

#include <memory>
#include <cstdint>

#include <CommonStates.h>

PLUGIN_NAMESPACE_BEGIN

// D3DCompile 薄封装：失败时按 a_log_prefix 记日志并返回 nullptr；正/误 blob 均正确释放。
[[nodiscard]] ID3DBlob* compile_shader(
    char const* a_source,
    char const* a_entry,
    char const* a_target,
    char const* a_name,
    char const* a_log_prefix);

// DYNAMIC 常量缓冲整块上传（Map WRITE_DISCARD）。
void update_constant_buffer(ID3D11DeviceContext* a_context, ID3D11Buffer* a_buffer, void const* a_data, std::size_t a_bytes);

// DirectXTK CommonStates 封装：UI 覆盖层共用的预乘 alpha / 无深度 / 无剔除状态。
// 惰性创建且创建后不随设备重建（与既有行为一致：设备对象进程级复用）。
// 注意：mask 管线不能复用这里的 CullNone 光栅化状态——mask 需要 DepthClipEnable=FALSE。
class OverlayStates
{
public:
    void ensure(ID3D11Device* a_device);

    [[nodiscard]] bool ready() const { return m_states != nullptr; }
    [[nodiscard]] ID3D11BlendState* alpha_blend() const { return m_states->AlphaBlend(); }
    [[nodiscard]] ID3D11DepthStencilState* depth_none() const { return m_states->DepthNone(); }
    [[nodiscard]] ID3D11RasterizerState* cull_none() const { return m_states->CullNone(); }

private:
    std::unique_ptr<DirectX::CommonStates> m_states;
};

// RAII 捕获/恢复 ImmediateContext 的完整管线状态（OM/RS/IA/VS/PS/CB/SRV/Sampler/视口）。
// 覆盖层绘制把自有管线绑定包在一段 capture 作用域内，退出后游戏状态原样。
// 覆盖集刻意不含 PS 常量缓冲槽（消费 pass 各自在全屏绘制前后就地保存/恢复 b0/b1）。
// Get 系列在引擎默认/隐式状态被绑定时返回 nullptr，Release 前必须判空——
// 空指针虚调用 Release 即访问违例（且无法被 try/catch 捕获）。
class D3D11StateCapture
{
public:
    explicit D3D11StateCapture(ID3D11DeviceContext* a_context);
    ~D3D11StateCapture();

    D3D11StateCapture(D3D11StateCapture const&) = delete;
    D3D11StateCapture(D3D11StateCapture&&) = delete;
    D3D11StateCapture& operator=(D3D11StateCapture const&) = delete;
    D3D11StateCapture& operator=(D3D11StateCapture&&) = delete;

    // 捕获时刻的 OM 渲染目标（无绑定时不为 nullptr，调用方判空）。
    [[nodiscard]] ID3D11RenderTargetView* render_target() const { return m_render_target; }

private:
    ID3D11DeviceContext* m_ref_context = nullptr;

    ID3D11RenderTargetView* m_render_target = nullptr;
    ID3D11DepthStencilView* m_depth_stencil = nullptr;
    ID3D11BlendState* m_blend = nullptr;
    float m_blend_factor[4]{};
    std::uint32_t m_sample_mask = 0;
    ID3D11DepthStencilState* m_depth = nullptr;
    std::uint32_t m_stencil_ref = 0;
    ID3D11RasterizerState* m_rasterizer = nullptr;
    std::uint32_t m_viewport_count = 0;
    D3D11_VIEWPORT m_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11InputLayout* m_input_layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* m_vertex_buffer = nullptr;
    std::uint32_t m_vertex_stride = 0;
    std::uint32_t m_vertex_offset = 0;
    ID3D11Buffer* m_index_buffer = nullptr;
    DXGI_FORMAT m_index_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t m_index_offset = 0;
    ID3D11VertexShader* m_vertex_shader = nullptr;
    ID3D11ClassInstance* m_vertex_instances[8]{};
    std::uint32_t m_vertex_instance_count = 8;
    ID3D11PixelShader* m_pixel_shader = nullptr;
    ID3D11ClassInstance* m_pixel_instances[8]{};
    std::uint32_t m_pixel_instance_count = 8;
    ID3D11Buffer* m_vertex_cbs[2]{};
    ID3D11ShaderResourceView* m_pixel_srv = nullptr;
    ID3D11SamplerState* m_pixel_sampler = nullptr;
};

PLUGIN_NAMESPACE_END
