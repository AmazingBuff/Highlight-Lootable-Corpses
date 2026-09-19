//
// Created by AmazingBuff on 2026/9/13.
//

#include "icon_overlay.h"

#include "render/shader_manager.h"

#include "render/dx11/common_states.h"
#include "render/dx11/d3d11_util.h"

ICON_NAMESPACE_BEGIN

IconOverlay::IconOverlay() :
    m_ref_vertex_shader(nullptr),
    m_ref_pixel_shader(nullptr),
    m_input_layout(nullptr),
    m_vertex_buffer(nullptr),
    m_ready(false),
    m_width(0),
    m_height(0) {}

bool IconOverlay::init(REX::W32::ID3D11Device* device)
{
    if (!m_ready)
        m_ready = create_pipeline(device);
    return m_ready;
}

void IconOverlay::begin_frame(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
}

bool IconOverlay::create_pipeline(REX::W32::ID3D11Device* device)
{
    // Both shaders and the VS bytecode are precompiled and owned by the ShaderManager.
    ShaderManager& shaders = ShaderManager::instance();
    REX::W32::ID3DBlob* vs_blob = shaders.icon_vs_blob();
    m_ref_vertex_shader = shaders.icon_vs();
    m_ref_pixel_shader = shaders.icon_ps();
    if (!vs_blob || !m_ref_vertex_shader || !m_ref_pixel_shader)
        return false;

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

    REX::W32::D3D11_BUFFER_DESC bd = {};
    bd.usage = REX::W32::D3D11_USAGE_DYNAMIC;
    bd.byteWidth = static_cast<uint32_t>(Icon_Max_Vertex_Count * sizeof(IconVertex));
    bd.bindFlags = REX::W32::D3D11_BIND_VERTEX_BUFFER;
    bd.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&bd, nullptr, &m_vertex_buffer);

    if (m_ref_vertex_shader && m_ref_pixel_shader && m_input_layout && m_vertex_buffer)
    {
        logger::info("Icon overlay pipeline ready ({} vertices max)", Icon_Max_Vertex_Count);
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
    // Both shaders are owned by the ShaderManager - only the borrowing pointers are cleared.
    m_ref_pixel_shader = nullptr;
    m_ref_vertex_shader = nullptr;
}

void IconOverlay::draw(REX::W32::ID3D11DeviceContext* context, REX::W32::ID3D11RenderTargetView* target, std::vector<IconVertex> const& vertices, CommonStates const& states) const
{
    if (vertices.empty())
        return;

    D3D11StateCapture capture(context);
    capture.capture();

    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!REX::W32::SUCCESS(context->Map(m_vertex_buffer, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    std::memcpy(mapped.data, vertices.data(), vertices.size() * sizeof(IconVertex));
    context->Unmap(m_vertex_buffer, 0);

    static constexpr uint32_t s_stride = sizeof(IconVertex);
    static constexpr uint32_t s_offset = 0;
    // CommonStates is the local REX::W32-typed mirror; its getters return the REX state pointers directly.
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(states.non_premultiplied(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(states.depth_none(), 0);
    context->RSSetState(states.cull_none());

    REX::W32::D3D11_VIEWPORT const viewport{
        .topLeftX = 0.0f,
        .topLeftY = 0.0f,
        .width = static_cast<float>(m_width),
        .height = static_cast<float>(m_height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(m_input_layout);
    context->IASetVertexBuffers(0, 1, &m_vertex_buffer, &s_stride, &s_offset);
    context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_ref_vertex_shader, nullptr, 0);
    context->PSSetShader(m_ref_pixel_shader, nullptr, 0);
    context->Draw(static_cast<uint32_t>(vertices.size()), 0);

    capture.restore();
}

void IconOverlay::end_frame()
{
}

ICON_NAMESPACE_END
