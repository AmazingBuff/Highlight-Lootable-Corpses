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
private:
    ID3D11DeviceContext* m_ref_context;

    ID3D11RenderTargetView* m_render_target;
    ID3D11DepthStencilView* m_depth_stencil;
    ID3D11BlendState* m_blend;
    float m_blend_factor[4];
    std::uint32_t m_sample_mask;
    ID3D11DepthStencilState* m_depth;
    std::uint32_t m_stencil_ref;
    ID3D11RasterizerState* m_rasterizer;
    std::uint32_t m_viewport_count;
    D3D11_VIEWPORT m_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11InputLayout* m_input_layout;
    D3D11_PRIMITIVE_TOPOLOGY m_topology;
    ID3D11Buffer* m_vertex_buffer;
    std::uint32_t m_vertex_stride;
    std::uint32_t m_vertex_offset;
    ID3D11Buffer* m_index_buffer;
    DXGI_FORMAT m_index_format;
    std::uint32_t m_index_offset;
    ID3D11VertexShader* m_vertex_shader;
    ID3D11ClassInstance* m_vertex_instances[8];
    std::uint32_t m_vertex_instance_count;
    ID3D11PixelShader* m_pixel_shader;
    ID3D11ClassInstance* m_pixel_instances[8];
    std::uint32_t m_pixel_instance_count;
    ID3D11Buffer* m_vertex_cbs[2];
    ID3D11ShaderResourceView* m_pixel_srvs[2];
    ID3D11SamplerState* m_pixel_sampler;
};

PLUGIN_NAMESPACE_END
