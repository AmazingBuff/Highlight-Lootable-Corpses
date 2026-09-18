//
// Created by AmazingBuff on 2026/9/13.
//

#include "icon_overlay.h"

#include "render/shader_sources.h"

#include "render/dx11/common_states.h"
#include "render/dx11/d3d11_util.h"

PLUGIN_NAMESPACE_BEGIN

namespace
{
    constexpr size_t Max_Vertex_Count = Icon_Marker_Vertex_Count * Max_Corpse_Count;
}

IconOverlay::IconOverlay() :
    m_vertex_shader(nullptr),
    m_pixel_shader(nullptr),
    m_input_layout(nullptr),
    m_vertex_buffer(nullptr),
    m_ready(false),
    m_width(0.0f),
    m_height(0.0f) {}

bool IconOverlay::init(REX::W32::ID3D11Device* device)
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

bool IconOverlay::create_pipeline(REX::W32::ID3D11Device* device)
{
    REX::W32::ID3DBlob* vs_blob = compile_shader(render_shaders::IconOverlay, "vs_main", "vs_5_0", "ui overlay", "ui overlay");
    REX::W32::ID3DBlob* ps_blob = compile_shader(render_shaders::IconOverlay, "ps_main", "ps_5_0", "ui overlay", "ui overlay");
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

    static constexpr REX::W32::D3D11_INPUT_ELEMENT_DESC s_layout_desc[] = {
        {
            .semanticName = "POSITION",
            .semanticIndex = 0,
            .format = REX::W32::DXGI_FORMAT_R32G32B32_FLOAT,
            .inputSlot = 0,
            .alignedByteOffset = 0,
            .inputSlotClass = REX::W32::D3D11_INPUT_PER_VERTEX_DATA,
            .instanceDataStepRate = 0
        },
        {
            .semanticName = "COLOR",
            .semanticIndex = 0,
            .format = REX::W32::DXGI_FORMAT_R32G32B32A32_FLOAT,
            .inputSlot = 0,
            .alignedByteOffset = 12,
            .inputSlotClass = REX::W32::D3D11_INPUT_PER_VERTEX_DATA,
            .instanceDataStepRate = 0
        },
    };
    device->CreateInputLayout(s_layout_desc, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &m_input_layout);

    vs_blob->Release();
    ps_blob->Release();

    REX::W32::D3D11_BUFFER_DESC bd = {};
    bd.usage = REX::W32::D3D11_USAGE_DYNAMIC;
    bd.byteWidth = static_cast<uint32_t>(Max_Vertex_Count * sizeof(IconVertex));
    bd.bindFlags = REX::W32::D3D11_BIND_VERTEX_BUFFER;
    bd.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
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

void IconOverlay::draw(REX::W32::ID3D11DeviceContext* context, REX::W32::ID3D11RenderTargetView* target, CommonStates const& states)
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

    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!REX::W32::SUCCESS(context->Map(m_vertex_buffer, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        m_vertices.clear();
        return;
    }
    std::memcpy(mapped.data, m_vertices.data(), count * sizeof(IconVertex));
    context->Unmap(m_vertex_buffer, 0);

    static constexpr uint32_t s_stride = sizeof(IconVertex);
    static constexpr uint32_t s_offset = 0;
    // CommonStates is the local REX::W32-typed mirror; its getters return the REX state pointers directly.
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(states.alpha_blend(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(states.depth_none(), 0);
    context->RSSetState(states.cull_none());
    REX::W32::D3D11_VIEWPORT const viewport{ 0.0f, 0.0f, m_width, m_height, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(m_input_layout);
    context->IASetVertexBuffers(0, 1, &m_vertex_buffer, &s_stride, &s_offset);
    context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertex_shader, nullptr, 0);
    context->PSSetShader(m_pixel_shader, nullptr, 0);
    context->Draw(static_cast<uint32_t>(count), 0);

    capture.restore();
    m_vertices.clear();
}

void IconOverlay::end_frame()
{
    m_vertices.clear();
}

PLUGIN_NAMESPACE_END
