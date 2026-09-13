//
// Created by AmazingBuff on 2026/9/13.
//

#include "ui_overlay.h"

#include "d3d11_util.h"
#include "render/shader_sources.h"

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstring>

PLUGIN_NAMESPACE_BEGIN

void UiOverlay::ensure(ID3D11Device* a_device)
{
    if (m_ready || m_failed || !a_device)
        return;
    m_ready = create_pipeline(a_device);
}

void UiOverlay::begin_frame(float a_width, float a_height)
{
    m_width = a_width;
    m_height = a_height;
}

void UiOverlay::add_vertex_ndc(float a_ndc_x, float a_ndc_y, DirectX::XMFLOAT4 const& a_color)
{
    m_vertices.emplace_back(a_ndc_x, a_ndc_y, 0.5f, a_color.x, a_color.y, a_color.z, a_color.w);
}

void UiOverlay::add_triangle(float a_x0, float a_y0, float a_x1, float a_y1, float a_x2, float a_y2, DirectX::XMFLOAT4 const& a_color)
{
    if (m_width <= 0.0f || m_height <= 0.0f)
        return;

    float const inv_w = 2.0f / m_width;
    float const inv_h = 2.0f / m_height;
    add_vertex_ndc(a_x0 * inv_w - 1.0f, 1.0f - a_y0 * inv_h, a_color);
    add_vertex_ndc(a_x1 * inv_w - 1.0f, 1.0f - a_y1 * inv_h, a_color);
    add_vertex_ndc(a_x2 * inv_w - 1.0f, 1.0f - a_y2 * inv_h, a_color);
}

void UiOverlay::add_circle(float a_center_x, float a_center_y, float a_radius_px, DirectX::XMFLOAT4 const& a_color)
{
    if (m_width <= 0.0f || m_height <= 0.0f)
        return;

    float const inv_w = 2.0f / m_width;
    float const inv_h = 2.0f / m_height;
    float const ndc_cx = a_center_x * inv_w - 1.0f;
    float const ndc_cy = 1.0f - a_center_y * inv_h;
    float const ndc_rx = a_radius_px * inv_w;
    float const ndc_ry = a_radius_px * inv_h;

    constexpr float Two_Pi = 6.283185307179586f;
    constexpr int Segments = 16;
    for (int k = 0; k < Segments; ++k)
    {
        float const a0 = Two_Pi * static_cast<float>(k) / static_cast<float>(Segments);
        float const a1 = Two_Pi * static_cast<float>(k + 1) / static_cast<float>(Segments);
        float const x0 = ndc_cx + std::cos(a0) * ndc_rx;
        float const y0 = ndc_cy + std::sin(a0) * ndc_ry;
        float const x1 = ndc_cx + std::cos(a1) * ndc_rx;
        float const y1 = ndc_cy + std::sin(a1) * ndc_ry;
        add_vertex_ndc(ndc_cx, ndc_cy, a_color);
        add_vertex_ndc(x0, y0, a_color);
        add_vertex_ndc(x1, y1, a_color);
    }
}

bool UiOverlay::create_pipeline(ID3D11Device* a_device)
{
    ID3DBlob* vs_blob = compile_shader(render_shaders::UiOverlay, "vs_main", "vs_5_0", "ui overlay", "ui overlay");
    ID3DBlob* ps_blob = compile_shader(render_shaders::UiOverlay, "ps_main", "ps_5_0", "ui overlay", "ui overlay");
    if (!vs_blob || !ps_blob)
    {
        if (vs_blob)
            vs_blob->Release();
        if (ps_blob)
            ps_blob->Release();

        m_failed = true;
        return false;
    }

    a_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vertex_shader);
    a_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_pixel_shader);

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
    a_device->CreateInputLayout(Layout_Desc, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &m_input_layout);

    vs_blob->Release();
    ps_blob->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = static_cast<UINT>(Vertex_Buffer_Bytes);  // Max_Vertices 个顶点，提交前按此裁剪
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    a_device->CreateBuffer(&bd, nullptr, &m_vertex_buffer);

    if (m_vertex_shader && m_pixel_shader && m_input_layout && m_vertex_buffer)
    {
        logger::info("UI overlay pipeline ready ({} vertices max)", Max_Vertices);
        return true;
    }

    m_failed = true;
    release_pipeline();
    logger::error("UI overlay pipeline creation failed, ESP rendering disabled");
    return false;
}

void UiOverlay::release_pipeline()
{
    if (m_vertex_buffer)
    {
        m_vertex_buffer->Release();
        m_vertex_buffer = nullptr;
    }
    if (m_input_layout)
    {
        m_input_layout->Release();
        m_input_layout = nullptr;
    }
    if (m_pixel_shader)
    {
        m_pixel_shader->Release();
        m_pixel_shader = nullptr;
    }
    if (m_vertex_shader)
    {
        m_vertex_shader->Release();
        m_vertex_shader = nullptr;
    }
}

void UiOverlay::flush(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, OverlayStates const& a_states)
{
    if (!m_ready || m_vertices.empty())
        return;

    // GPU 缓冲容量固定：超出部分整三角形丢弃，绝不能按实际顶点数 memcpy
    std::size_t count = std::min(m_vertices.size(), Max_Vertices);
    count -= count % 3;
    if (m_vertices.size() > Max_Vertices && !m_overflow_reported)
    {
        m_overflow_reported = true;
        logger::warn("UI vertex buffer full: {} of {} vertices dropped this frame", m_vertices.size() - count, m_vertices.size());
    }
    if (count == 0)
    {
        m_vertices.clear();
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(a_context->Map(m_vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        m_vertices.clear();
        return;
    }
    std::memcpy(mapped.pData, m_vertices.data(), count * sizeof(UiVertex));
    a_context->Unmap(m_vertex_buffer, 0);

    constexpr UINT stride = sizeof(UiVertex);
    constexpr UINT offset = 0;
    a_context->OMSetRenderTargets(1, &a_target, nullptr);
    a_context->OMSetBlendState(a_states.alpha_blend(), nullptr, 0xFFFFFFFF);
    a_context->OMSetDepthStencilState(a_states.depth_none(), 0);
    a_context->RSSetState(a_states.cull_none());
    a_context->IASetInputLayout(m_input_layout);
    a_context->IASetVertexBuffers(0, 1, &m_vertex_buffer, &stride, &offset);
    a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a_context->VSSetShader(m_vertex_shader, nullptr, 0);
    a_context->PSSetShader(m_pixel_shader, nullptr, 0);
    a_context->Draw(static_cast<UINT>(count), 0);

    m_vertices.clear();
}

PLUGIN_NAMESPACE_END
