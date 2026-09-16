//
// Created by AmazingBuff on 2026/9/13.
//

#include "icon_overlay.h"

#include "../dx11/d3d11_util.h"
#include "render/shader_sources.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include <d3d11.h>
#include <CommonStates.h>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    constexpr uint32_t Circle_Segments = 16;
    constexpr uint32_t Max_Vertex_Count = 3 * Circle_Segments * Max_Corpse_Count;
}

bool IconOverlay::init(ID3D11Device* device)
{
    if (!m_ready)
        m_ready = create_pipeline(device);
    return m_ready;
}

void IconOverlay::begin_frame(float width, float height)
{
    m_width = width;
    m_height = height;
}

void IconOverlay::add_vertex_ndc(float ndc_x, float ndc_y, DirectX::XMFLOAT4 const& color)
{
    m_vertices.emplace_back(DirectX::XMFLOAT3{ndc_x, ndc_y, 0.5f}, color);
}

void IconOverlay::add_triangle(float x0, float y0, float x1, float y1, float x2, float y2, DirectX::XMFLOAT4 const& color)
{
    if (m_width <= 0.0f || m_height <= 0.0f)
        return;

    float const inv_w = 2.0f / m_width;
    float const inv_h = 2.0f / m_height;
    add_vertex_ndc(x0 * inv_w - 1.0f, 1.0f - y0 * inv_h, color);
    add_vertex_ndc(x1 * inv_w - 1.0f, 1.0f - y1 * inv_h, color);
    add_vertex_ndc(x2 * inv_w - 1.0f, 1.0f - y2 * inv_h, color);
}

void IconOverlay::add_circle(float center_x, float center_y, float radius_px, DirectX::XMFLOAT4 const& color)
{
    if (m_width <= 0.0f || m_height <= 0.0f)
        return;

    float const inv_w = 2.0f / m_width;
    float const inv_h = 2.0f / m_height;
    float const ndc_cx = center_x * inv_w - 1.0f;
    float const ndc_cy = 1.0f - center_y * inv_h;
    float const ndc_rx = radius_px * inv_w;
    float const ndc_ry = radius_px * inv_h;

    constexpr float Two_Pi = 2.f * std::numbers::pi_v<float>;
    for (uint32_t k = 0; k < Circle_Segments; ++k)
    {
        float const a0 = Two_Pi * static_cast<float>(k) / static_cast<float>(Circle_Segments);
        float const a1 = Two_Pi * static_cast<float>(k + 1) / static_cast<float>(Circle_Segments);
        float const x0 = ndc_cx + std::cos(a0) * ndc_rx;
        float const y0 = ndc_cy + std::sin(a0) * ndc_ry;
        float const x1 = ndc_cx + std::cos(a1) * ndc_rx;
        float const y1 = ndc_cy + std::sin(a1) * ndc_ry;
        add_vertex_ndc(ndc_cx, ndc_cy, color);
        add_vertex_ndc(x0, y0, color);
        add_vertex_ndc(x1, y1, color);
    }
}

bool IconOverlay::create_pipeline(ID3D11Device* device)
{
    ID3DBlob* vs_blob = compile_shader(render_shaders::IconOverlay, "vs_main", "vs_5_0", "ui overlay", "ui overlay");
    ID3DBlob* ps_blob = compile_shader(render_shaders::IconOverlay, "ps_main", "ps_5_0", "ui overlay", "ui overlay");
    if (!vs_blob || !ps_blob)
    {
        if (vs_blob)
            vs_blob->Release();
        if (ps_blob)
            ps_blob->Release();

        return false;
    }

    device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vertex_shader);
    device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_pixel_shader);

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
    device->CreateInputLayout(Layout_Desc, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &m_input_layout);

    vs_blob->Release();
    ps_blob->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = Max_Vertex_Count * sizeof(IconVertex);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&bd, nullptr, &m_vertex_buffer);

    if (m_vertex_shader && m_pixel_shader && m_input_layout && m_vertex_buffer)
    {
        logger::info("Icon overlay pipeline ready ({} vertices max)", Max_Vertex_Count);
        return true;
    }

    release_pipeline();
    logger::error("Icon overlay pipeline creation failed, ESP rendering disabled");
    return false;
}

void IconOverlay::release_pipeline()
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

void IconOverlay::draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, std::shared_ptr<DirectX::DX11::CommonStates> const& states)
{
    if (!m_ready || m_vertices.empty())
        return;

    std::size_t count = std::min<size_t>(m_vertices.size(), Max_Vertex_Count);
    count -= count % 3;
    if (m_vertices.size() > Max_Vertex_Count)
        logger::warn("Icon vertex buffer full: {} of {} vertices dropped this frame", m_vertices.size() - count, m_vertices.size());

    if (count == 0)
    {
        m_vertices.clear();
        return;
    }

    D3D11StateCapture capture(context);
    capture.capture();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(m_vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        m_vertices.clear();
        return;
    }
    std::memcpy(mapped.pData, m_vertices.data(), count * sizeof(IconVertex));
    context->Unmap(m_vertex_buffer, 0);

    constexpr UINT stride = sizeof(IconVertex);
    constexpr UINT offset = 0;
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(states->AlphaBlend(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(states->DepthNone(), 0);
    context->RSSetState(states->CullNone());
    context->IASetInputLayout(m_input_layout);
    context->IASetVertexBuffers(0, 1, &m_vertex_buffer, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertex_shader, nullptr, 0);
    context->PSSetShader(m_pixel_shader, nullptr, 0);
    context->Draw(static_cast<UINT>(count), 0);

    capture.restore();
}

void IconOverlay::end_frame()
{
    m_vertices.clear();
}

PLUGIN_NAMESPACE_END
