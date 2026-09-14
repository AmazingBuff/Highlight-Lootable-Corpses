//
// Created by AmazingBuff on 2026/9/13.
//

#include "d3d11_util.h"

#include <d3dcompiler.h>

#include <cstring>

PLUGIN_NAMESPACE_BEGIN

ID3DBlob* compile_shader(char const* a_source, char const* a_entry, char const* a_target, char const* a_name, char const* a_log_prefix)
{
    if (!a_source || !a_entry || !a_target)
        return nullptr;

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT const hr = D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr, nullptr, a_entry, a_target, 0, 0, &blob, &err);
    if (FAILED(hr))
    {
        logger::error(
            "{} shader compile failed [{} {}] ({:X}): {}",
            a_log_prefix ? a_log_prefix : "?",
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

void update_constant_buffer(ID3D11DeviceContext* a_context, ID3D11Buffer* a_buffer, void const* a_data, std::size_t a_bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(a_context->Map(a_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    std::memcpy(mapped.pData, a_data, a_bytes);
    a_context->Unmap(a_buffer, 0);
}

void OverlayStates::ensure(ID3D11Device* a_device)
{
    if (!m_states && a_device)
        m_states = std::make_unique<DirectX::CommonStates>(a_device);
}

D3D11StateCapture::D3D11StateCapture(ID3D11DeviceContext* a_context) : m_ref_context(a_context)
{
    m_ref_context->OMGetRenderTargets(1, &m_render_target, &m_depth_stencil);
    m_ref_context->OMGetBlendState(&m_blend, m_blend_factor, &m_sample_mask);
    m_ref_context->OMGetDepthStencilState(&m_depth, &m_stencil_ref);
    m_ref_context->RSGetState(&m_rasterizer);

    m_viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    m_ref_context->RSGetViewports(&m_viewport_count, m_viewports);

    m_ref_context->IAGetInputLayout(&m_input_layout);
    m_ref_context->IAGetPrimitiveTopology(&m_topology);
    m_ref_context->IAGetVertexBuffers(0, 1, &m_vertex_buffer, &m_vertex_stride, &m_vertex_offset);
    m_ref_context->IAGetIndexBuffer(&m_index_buffer, &m_index_format, &m_index_offset);

    m_ref_context->VSGetShader(&m_vertex_shader, m_vertex_instances, &m_vertex_instance_count);
    m_ref_context->PSGetShader(&m_pixel_shader, m_pixel_instances, &m_pixel_instance_count);
    m_ref_context->VSGetConstantBuffers(0, 2, m_vertex_cbs);
    m_ref_context->PSGetShaderResources(0, 1, &m_pixel_srv);
    m_ref_context->PSGetSamplers(0, 1, &m_pixel_sampler);
}

D3D11StateCapture::~D3D11StateCapture()
{
    m_ref_context->OMSetRenderTargets(1, &m_render_target, m_depth_stencil);
    m_ref_context->OMSetBlendState(m_blend, m_blend_factor, m_sample_mask);
    m_ref_context->OMSetDepthStencilState(m_depth, m_stencil_ref);
    m_ref_context->RSSetState(m_rasterizer);
    m_ref_context->RSSetViewports(m_viewport_count, m_viewports);
    m_ref_context->IASetInputLayout(m_input_layout);
    m_ref_context->IASetPrimitiveTopology(m_topology);
    m_ref_context->IASetVertexBuffers(0, 1, &m_vertex_buffer, &m_vertex_stride, &m_vertex_offset);
    m_ref_context->IASetIndexBuffer(m_index_buffer, m_index_format, m_index_offset);
    m_ref_context->VSSetShader(m_vertex_shader, m_vertex_instances, m_vertex_instance_count);
    m_ref_context->PSSetShader(m_pixel_shader, m_pixel_instances, m_pixel_instance_count);
    m_ref_context->VSSetConstantBuffers(0, 2, m_vertex_cbs);
    m_ref_context->PSSetShaderResources(0, 1, &m_pixel_srv);
    m_ref_context->PSSetSamplers(0, 1, &m_pixel_sampler);

    // Get 系列在引擎默认/隐式状态被绑定时返回 nullptr，Release 前必须判空
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
    for (ID3D11ClassInstance* instance : m_vertex_instances)
    {
        if (instance)
            instance->Release();
    }
    for (ID3D11ClassInstance* instance : m_pixel_instances)
    {
        if (instance)
            instance->Release();
    }
    for (ID3D11Buffer* cb : m_vertex_cbs)
    {
        if (cb)
            cb->Release();
    }
    if (m_pixel_srv)
        m_pixel_srv->Release();
    if (m_pixel_sampler)
        m_pixel_sampler->Release();
}

PLUGIN_NAMESPACE_END
