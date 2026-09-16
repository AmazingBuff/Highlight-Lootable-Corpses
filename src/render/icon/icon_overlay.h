//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <DirectXMath.h>
#include <CommonStates.h>

struct ID3D11Buffer;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11InputLayout;
struct ID3D11PixelShader;
struct ID3D11RenderTargetView;
struct ID3D11VertexShader;

PLUGIN_NAMESPACE_BEGIN

struct IconVertex
{
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT4 color;
};

class IconOverlay
{
public:
    IconOverlay() = default;
    ~IconOverlay() = default;

    bool init(ID3D11Device* device);
    void begin_frame(float width, float height);
    void add_circle(float center_x, float center_y, float radius_px, DirectX::XMFLOAT4 const& color);
    void add_triangle(float x0, float y0, float x1, float y1, float x2, float y2, DirectX::XMFLOAT4 const& a_color);
    void draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, std::shared_ptr<DirectX::DX11::CommonStates> const& states);
    void end_frame();
private:
    bool create_pipeline(ID3D11Device* a_device);
    void release_pipeline();
    void add_vertex_ndc(float a_ndc_x, float a_ndc_y, DirectX::XMFLOAT4 const& a_color);
private:
    ID3D11VertexShader* m_vertex_shader = nullptr;
    ID3D11PixelShader* m_pixel_shader = nullptr;
    ID3D11InputLayout* m_input_layout = nullptr;
    ID3D11Buffer* m_vertex_buffer = nullptr;
    bool m_ready = false;

    float m_width = 0.0f;
    float m_height = 0.0f;

    std::vector<IconVertex> m_vertices;
};

PLUGIN_NAMESPACE_END
