//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <d3d11.h>

#include <cstdint>

PLUGIN_NAMESPACE_BEGIN
[[nodiscard]] ID3DBlob* compile_shader(char const* source, char const* entry, char const* target, char const* name, char const* log_prefix);
void update_constant_buffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, void const* data, std::size_t bytes);

class D3D11StateCapture
{
public:
    explicit D3D11StateCapture(ID3D11DeviceContext* context);
    ~D3D11StateCapture();

    D3D11StateCapture(D3D11StateCapture const&) = delete;
    D3D11StateCapture(D3D11StateCapture&&) = delete;
    D3D11StateCapture& operator=(D3D11StateCapture const&) = delete;
    D3D11StateCapture& operator=(D3D11StateCapture&&) = delete;

    void capture();
    void restore() const;
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
