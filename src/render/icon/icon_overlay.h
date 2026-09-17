//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "icon_geometry.h"

#include "Plugin.h"

#include <CommonStates.h>

#include <DirectXMath.h>

#include <memory>
#include <vector>

struct ID3D11Buffer;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11InputLayout;
struct ID3D11PixelShader;
struct ID3D11RenderTargetView;
struct ID3D11VertexShader;

PLUGIN_NAMESPACE_BEGIN

class IconOverlay
{
public:
    IconOverlay();
    ~IconOverlay() = default;

    bool init(ID3D11Device* device);
    void begin_frame(float width, float height);
    void add_marker(IconMarker const& marker, DirectX::XMFLOAT3 const& color);
    void draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, std::shared_ptr<DirectX::DX11::CommonStates> const& states);
    void end_frame();
private:
    bool create_pipeline(ID3D11Device* a_device);
    void release_pipeline();
private:
    ID3D11VertexShader* m_vertex_shader;
    ID3D11PixelShader* m_pixel_shader;
    ID3D11InputLayout* m_input_layout;
    ID3D11Buffer* m_vertex_buffer;
    bool m_ready;

    float m_width;
    float m_height;

    std::vector<IconVertex> m_vertices;
};

PLUGIN_NAMESPACE_END
