//
// Created by AmazingBuff on 2026/9/13.
//

#include "icon_overlay.h"

#include "render/dx11/d3d11_util.h"
#include "render/shader_sources.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <d3d11.h>
#include <CommonStates.h>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    constexpr size_t Max_Vertex_Count = Icon_Marker_Vertex_Count * Max_Corpse_Count;
}

IconOverlay::IconOverlay()
    : m_vertex_shader(nullptr)
    , m_pixel_shader(nullptr)
    , m_input_layout(nullptr)
    , m_vertex_buffer(nullptr)
    , m_ready(false)
    , m_width(0.0f)
    , m_height(0.0f) {}

bool IconOverlay::init(ID3D11Device* device)
{
    if (!m_ready)
        m_ready = create_pipeline(device);
    return m_ready;
}

void IconOverlay::begin_frame(float width, float height)
{
    m_vertices.clear();
    m_width = width;
    m_height = height;
}

void IconOverlay::add_marker(IconMarker const& marker, DirectX::XMFLOAT3 const& color)
{
    IconGeometry const geometry = icon_geometry(marker, color, m_width, m_height);
    if (m_vertices.size() + geometry.count <= Max_Vertex_Count)
        m_vertices.insert(m_vertices.end(), geometry.vertices.begin(), geometry.vertices.begin() + geometry.count);
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

    static constexpr D3D11_INPUT_ELEMENT_DESC s_layout_desc[] = {
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
    device->CreateInputLayout(s_layout_desc, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &m_input_layout);

    vs_blob->Release();
    ps_blob->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = static_cast<UINT>(Max_Vertex_Count * sizeof(IconVertex));
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

    size_t count = std::min<size_t>(m_vertices.size(), Max_Vertex_Count);
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

    static constexpr UINT s_stride = sizeof(IconVertex);
    static constexpr UINT s_offset = 0;
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(states->AlphaBlend(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(states->DepthNone(), 0);
    context->RSSetState(states->CullNone());
    D3D11_VIEWPORT const viewport{ 0.0f, 0.0f, m_width, m_height, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(m_input_layout);
    context->IASetVertexBuffers(0, 1, &m_vertex_buffer, &s_stride, &s_offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertex_shader, nullptr, 0);
    context->PSSetShader(m_pixel_shader, nullptr, 0);
    context->Draw(static_cast<UINT>(count), 0);

    capture.restore();
    m_vertices.clear();
}

void IconOverlay::end_frame()
{
    m_vertices.clear();
}

PLUGIN_NAMESPACE_END
