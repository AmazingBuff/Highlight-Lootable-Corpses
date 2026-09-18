//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "icon_types.h"

PLUGIN_NAMESPACE_BEGIN
class CommonStates;
PLUGIN_NAMESPACE_END

ICON_NAMESPACE_BEGIN

class IconOverlay
{
public:
    IconOverlay();
    ~IconOverlay() = default;

    bool init(REX::W32::ID3D11Device* device);
    void begin_frame(uint32_t width, uint32_t height);
    void add_marker(IconMarker const& marker, DirectX::XMFLOAT3 const& color);
    void draw(REX::W32::ID3D11DeviceContext* context, REX::W32::ID3D11RenderTargetView* target, CommonStates const& states);
    void end_frame();
private:
    bool create_pipeline(REX::W32::ID3D11Device* a_device);
    void release_pipeline();
private:
    REX::W32::ID3D11VertexShader* m_vertex_shader;
    REX::W32::ID3D11PixelShader* m_pixel_shader;
    REX::W32::ID3D11InputLayout* m_input_layout;
    REX::W32::ID3D11Buffer* m_vertex_buffer;
    bool m_ready;

    uint32_t m_width;
    uint32_t m_height;

    std::vector<IconVertex> m_vertices;
};

ICON_NAMESPACE_END
