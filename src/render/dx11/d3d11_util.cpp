//
// Created by AmazingBuff on 2026/9/13.
//

#include "d3d11_util.h"

#include <REX/W32/D3DCOMPILER.h>

PLUGIN_NAMESPACE_BEGIN

REX::W32::ID3DBlob* compile_shader(char const* source, char const* entry, char const* target, char const* name, char const* log_prefix)
{
    if (!source || !entry || !target)
        return nullptr;

    REX::W32::ID3DBlob* blob = nullptr;
    REX::W32::ID3DBlob* err = nullptr;
    REX::W32::HRESULT const hr = REX::W32::D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &err);
    if (!REX::W32::SUCCESS(hr))
    {
        logger::error(
            "{} shader compile failed [{} {}] ({:X}): {}",
            log_prefix ? log_prefix : "?",
            name ? name : "?",
            target,
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

D3D11StateCapture::D3D11StateCapture(REX::W32::ID3D11DeviceContext* context) :
    m_ref_context(context),
    m_render_target(nullptr),
    m_depth_stencil(nullptr),
    m_blend(nullptr),
    m_blend_factor{},
    m_sample_mask(0),
    m_depth(nullptr),
    m_stencil_ref(0),
    m_rasterizer(nullptr),
    m_viewport_count(0),
    m_viewports{},
    m_scissor_count(0),
    m_scissor_rects{},
    m_input_layout(nullptr),
    m_topology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED),
    m_vertex_buffer(nullptr),
    m_vertex_stride(0),
    m_vertex_offset(0),
    m_index_buffer(nullptr),
    m_index_format(REX::W32::DXGI_FORMAT_UNKNOWN),
    m_index_offset(0),
    m_vertex_shader(nullptr),
    m_vertex_instances{},
    m_vertex_instance_count(8),
    m_pixel_shader(nullptr),
    m_pixel_instances{},
    m_pixel_instance_count(8),
    m_vertex_cbs{},
    m_pixel_cbs{},
    m_pixel_srvs{},
    m_pixel_sampler(nullptr) {}

D3D11StateCapture::~D3D11StateCapture()
{
    if (m_render_target)
        m_render_target->Release();
    if (m_depth_stencil)
        m_depth_stencil->Release();
    if (m_blend)
        m_blend->Release();
    if (m_depth)
        m_depth->Release();
    if (m_rasterizer)
        m_rasterizer->Release();
    if (m_input_layout)
        m_input_layout->Release();
    if (m_vertex_buffer)
        m_vertex_buffer->Release();
    if (m_index_buffer)
        m_index_buffer->Release();
    if (m_vertex_shader)
        m_vertex_shader->Release();
    if (m_pixel_shader)
        m_pixel_shader->Release();
    for (REX::W32::ID3D11ClassInstance* instance : m_vertex_instances)
    {
        if (instance)
            instance->Release();
    }
    for (REX::W32::ID3D11ClassInstance* instance : m_pixel_instances)
    {
        if (instance)
            instance->Release();
    }
    for (REX::W32::ID3D11Buffer* cb : m_vertex_cbs)
    {
        if (cb)
            cb->Release();
    }
    for (REX::W32::ID3D11Buffer* cb : m_pixel_cbs)
    {
        if (cb)
            cb->Release();
    }
    for (REX::W32::ID3D11ShaderResourceView* srv : m_pixel_srvs)
    {
        if (srv)
            srv->Release();
    }
    if (m_pixel_sampler)
        m_pixel_sampler->Release();
}

void D3D11StateCapture::capture()
{
    m_ref_context->OMGetRenderTargets(1, &m_render_target, &m_depth_stencil);
    m_ref_context->OMGetBlendState(&m_blend, m_blend_factor, &m_sample_mask);
    m_ref_context->OMGetDepthStencilState(&m_depth, &m_stencil_ref);
    m_ref_context->RSGetState(&m_rasterizer);

    m_viewport_count = REX::W32::D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    m_ref_context->RSGetViewports(&m_viewport_count, m_viewports);
    m_scissor_count = REX::W32::D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    m_ref_context->RSGetScissorRects(&m_scissor_count, m_scissor_rects);

    m_ref_context->IAGetInputLayout(&m_input_layout);
    m_ref_context->IAGetPrimitiveTopology(&m_topology);
    m_ref_context->IAGetVertexBuffers(0, 1, &m_vertex_buffer, &m_vertex_stride, &m_vertex_offset);
    m_ref_context->IAGetIndexBuffer(&m_index_buffer, &m_index_format, &m_index_offset);

    m_ref_context->VSGetShader(&m_vertex_shader, m_vertex_instances, &m_vertex_instance_count);
    m_ref_context->PSGetShader(&m_pixel_shader, m_pixel_instances, &m_pixel_instance_count);
    m_ref_context->VSGetConstantBuffers(0, 2, m_vertex_cbs);
    m_ref_context->PSGetConstantBuffers(0, 2, m_pixel_cbs);
    m_ref_context->PSGetShaderResources(0, 3, m_pixel_srvs);
    m_ref_context->PSGetSamplers(0, 1, &m_pixel_sampler);
}

void D3D11StateCapture::restore() const
{
    m_ref_context->OMSetRenderTargets(1, &m_render_target, m_depth_stencil);
    m_ref_context->OMSetBlendState(m_blend, m_blend_factor, m_sample_mask);
    m_ref_context->OMSetDepthStencilState(m_depth, m_stencil_ref);
    m_ref_context->RSSetState(m_rasterizer);
    m_ref_context->RSSetViewports(m_viewport_count, m_viewports);
    m_ref_context->RSSetScissorRects(m_scissor_count, m_scissor_rects);
    m_ref_context->IASetInputLayout(m_input_layout);
    m_ref_context->IASetPrimitiveTopology(m_topology);
    m_ref_context->IASetVertexBuffers(0, 1, &m_vertex_buffer, &m_vertex_stride, &m_vertex_offset);
    m_ref_context->IASetIndexBuffer(m_index_buffer, m_index_format, m_index_offset);
    m_ref_context->VSSetShader(m_vertex_shader, m_vertex_instances, m_vertex_instance_count);
    m_ref_context->PSSetShader(m_pixel_shader, m_pixel_instances, m_pixel_instance_count);
    m_ref_context->VSSetConstantBuffers(0, 2, m_vertex_cbs);
    m_ref_context->PSSetConstantBuffers(0, 2, m_pixel_cbs);
    m_ref_context->PSSetShaderResources(0, 3, m_pixel_srvs);
    m_ref_context->PSSetSamplers(0, 1, &m_pixel_sampler);
}

PLUGIN_NAMESPACE_END
