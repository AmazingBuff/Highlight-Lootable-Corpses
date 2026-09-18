//
// Created by AmazingBuff on 2026/09/17.
//

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#define MASK_NAMESPACE_BEGIN namespace mask_test {
#define MASK_NAMESPACE_END }
#include "render/mask/mask_depth.h"

namespace mask_test
{
using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT4;
using DirectX::XMFLOAT4X4;

void check(bool success, char const* message)
{
    if (!success)
        throw std::runtime_error(message);
}

void checked(HRESULT hr, char const* message)
{
    if (FAILED(hr))
    {
        std::cerr << message << ": HRESULT " << std::hex << hr << std::dec << '\n';
        throw std::runtime_error(message);
    }
}

ComPtr<ID3DBlob> compile(std::filesystem::path const& path, char const* entry, char const* profile)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT const hr = D3DCompileFromFile(path.c_str(), nullptr, nullptr, entry, profile,
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (errors)
        std::cerr << static_cast<char const*>(errors->GetBufferPointer());
    checked(hr, entry);
    return blob;
}

struct Vertex
{
    float x, y, z;
    float weights[4]{ 1, 0, 0, 0 };
    std::uint32_t indices[4]{};
};

struct DrawConstants
{
    XMFLOAT4X4 projection;
    std::uint32_t id;
    float pad[3]{};
};
static_assert(sizeof(DrawConstants) == 80);

class Fixture
{
public:
    explicit Fixture(std::filesystem::path const& root, UINT size = 32) : m_size(size)
    {
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
            nullptr, 0, D3D11_SDK_VERSION, &m_device, &level, &m_context);
        if (FAILED(hr))
        {
            checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &m_device, &level, &m_context), "WARP device");
            std::cout << "LIMIT: D3D11 debug layer unavailable\n";
        }
        m_device.As(&m_info);
        std::filesystem::path const shaders = root / "src/render/shaders";
        ComPtr<ID3DBlob> const static_blob = compile(shaders / "mask_geometry.hlsl", "vs_static_main", "vs_5_0");
        ComPtr<ID3DBlob> const skin_blob = compile(shaders / "mask_geometry.hlsl", "vs_skinned_main", "vs_5_0");
        ComPtr<ID3DBlob> const mask_blob = compile(shaders / "mask_geometry.hlsl", "ps_main", "ps_5_0");
        ComPtr<ID3DBlob> const full_blob = compile(shaders / "mask_composite.hlsl", "vs_main", "vs_5_0");
        ComPtr<ID3DBlob> const fill_blob = compile(shaders / "mask_composite.hlsl", "ps_silhouette_main", "ps_5_0");
        ComPtr<ID3DBlob> const outline_blob = compile(shaders / "mask_composite.hlsl", "ps_outline_main", "ps_5_0");
        checked(m_device->CreateVertexShader(static_blob->GetBufferPointer(), static_blob->GetBufferSize(), nullptr, &m_static), "static VS");
        checked(m_device->CreateVertexShader(skin_blob->GetBufferPointer(), skin_blob->GetBufferSize(), nullptr, &m_skin), "skin VS");
        checked(m_device->CreateVertexShader(full_blob->GetBufferPointer(), full_blob->GetBufferSize(), nullptr, &m_full), "fullscreen VS");
        checked(m_device->CreatePixelShader(mask_blob->GetBufferPointer(), mask_blob->GetBufferSize(), nullptr, &m_mask_ps), "mask PS");
        checked(m_device->CreatePixelShader(fill_blob->GetBufferPointer(), fill_blob->GetBufferSize(), nullptr, &m_fill_ps), "fill PS");
        checked(m_device->CreatePixelShader(outline_blob->GetBufferPointer(), outline_blob->GetBufferSize(), nullptr, &m_outline_ps), "outline PS");
        D3D11_INPUT_ELEMENT_DESC const elements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };
        checked(m_device->CreateInputLayout(elements, 1, static_blob->GetBufferPointer(), static_blob->GetBufferSize(), &m_layout), "static layout");
        checked(m_device->CreateInputLayout(elements, 3, skin_blob->GetBufferPointer(), skin_blob->GetBufferSize(), &m_skin_layout), "skin layout");
        m_draw_cb = buffer(sizeof(DrawConstants), D3D11_BIND_CONSTANT_BUFFER);
        m_composite_cb = buffer(16, D3D11_BIND_CONSTANT_BUFFER);
        m_palette_cb = buffer(128 * 64, D3D11_BIND_CONSTANT_BUFFER);
        std::array<XMFLOAT4X4, 128> palette;
        for (XMFLOAT4X4& matrix : palette)
            DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixIdentity());
        m_context->UpdateSubresource(m_palette_cb.Get(), 0, nullptr, palette.data(), 0, 0);
        m_vb = buffer(6 * sizeof(Vertex), D3D11_BIND_VERTEX_BUFFER);
        m_id = texture(DXGI_FORMAT_R32_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        m_color = texture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
        m_depth = texture(DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL);
        checked(m_device->CreateRenderTargetView(m_id.Get(), nullptr, &m_id_rtv), "ID RTV");
        checked(m_device->CreateShaderResourceView(m_id.Get(), nullptr, &m_id_srv), "ID SRV");
        checked(m_device->CreateRenderTargetView(m_color.Get(), nullptr, &m_color_rtv), "color RTV");
        checked(m_device->CreateDepthStencilView(m_depth.Get(), nullptr, &m_dsv), "DSV");
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_GREATER;
        checked(m_device->CreateDepthStencilState(&depth, &m_depth_on), "depth state");
        depth.DepthEnable = FALSE;
        checked(m_device->CreateDepthStencilState(&depth, &m_depth_off), "no depth state");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        checked(m_device->CreateRasterizerState(&raster, &m_raster), "raster state");
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        checked(m_device->CreateBlendState(&blend, &m_no_blend), "ID blend state");
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        checked(m_device->CreateBlendState(&blend, &m_blend), "premul blend state");
        D3D11_VIEWPORT const vp{ 0, 0, float(m_size), float(m_size), 0, 1 };
        m_context->RSSetViewports(1, &vp);
        m_context->RSSetState(m_raster.Get());
        styles(300);
    }

    ~Fixture() { m_context->ClearState(); }

    void styles(UINT count)
    {
        unbind();
        m_style_srv.Reset();
        m_styles.Reset();
        std::vector<XMFLOAT4> data(count, XMFLOAT4{ 0, 1, 0, 0.6f });
        for (UINT i = 2; i < count; ++i)
            data[i] = { float(i + 1) / float(count), 1.0f - float(i + 1) / float(count), 0.25f, 0.6f };
        data[0] = { 1, 0, 0, 0.4f };
        data[1] = { 0, 0, 1, 0.8f };
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = count * sizeof(XMFLOAT4);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(XMFLOAT4);
        checked(m_device->CreateBuffer(&desc, nullptr, &m_styles), "style buffer");
        D3D11_BOX const box{ 0, 0, 0, desc.ByteWidth, 1, 1 };
        m_context->UpdateSubresource(m_styles.Get(), 0, &box, data.data(), 0, 0);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = count;
        checked(m_device->CreateShaderResourceView(m_styles.Get(), &srv, &m_style_srv), "style SRV");
    }

    void clear_color()
    {
        float const zero[4]{};
        m_context->ClearRenderTargetView(m_color_rtv.Get(), zero);
    }

    void begin_mask(bool depth)
    {
        unbind();
        ID3D11RenderTargetView* const rtv = m_id_rtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, depth ? m_dsv.Get() : nullptr);
        m_context->OMSetBlendState(m_no_blend.Get(), nullptr, UINT_MAX);
        m_context->OMSetDepthStencilState(depth ? m_depth_on.Get() : m_depth_off.Get(), 0);
        float const zero[4]{};
        m_context->ClearRenderTargetView(rtv, zero);
        m_context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 0, 0);
    }

    void rectangle(UINT id, float left, float top, float right, float bottom, float z_left, float z_right, bool skin = false)
    {
        auto const vertex = [&](float x, float y, float z) {
            Vertex v{ (2 * x / m_size - 1) * z, (1 - 2 * y / m_size) * z, z };
            if (skin)
                v.weights[0] = 0.8f; // Non-unit homogeneous weight must not change projected depth.
            return v;
        };
        Vertex const vertices[] = { vertex(left, top, z_left), vertex(right, top, z_right), vertex(left, bottom, z_left),
            vertex(left, bottom, z_left), vertex(right, top, z_right), vertex(right, bottom, z_right) };
        m_context->UpdateSubresource(m_vb.Get(), 0, nullptr, vertices, 0, 0);
        XMFLOAT4X4 projection{ 1,0,0,0, 0,1,0,0, 0,0,-9,7, 0,0,1,0 };
        check(make_private_depth_projection(projection, 1, false), "valid private projection");
        DrawConstants const constants{ projection, id };
        m_context->UpdateSubresource(m_draw_cb.Get(), 0, nullptr, &constants, 0, 0);
        ID3D11Buffer* const cbs[] = { m_draw_cb.Get(), m_palette_cb.Get() };
        m_context->VSSetConstantBuffers(0, 2, cbs);
        m_context->IASetInputLayout(skin ? m_skin_layout.Get() : m_layout.Get());
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT const stride = sizeof(Vertex), offset = 0;
        ID3D11Buffer* const vb = m_vb.Get();
        m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        m_context->VSSetShader(skin ? m_skin.Get() : m_static.Get(), nullptr, 0);
        m_context->PSSetShader(m_mask_ps.Get(), nullptr, 0);
        m_context->Draw(6, 0);
    }

    void composite(bool outline)
    {
        ID3D11RenderTargetView* const rtv = m_color_rtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->OMSetDepthStencilState(m_depth_off.Get(), 0);
        m_context->OMSetBlendState(m_blend.Get(), nullptr, UINT_MAX);
        m_context->IASetInputLayout(nullptr);
        m_context->VSSetShader(m_full.Get(), nullptr, 0);
        m_context->PSSetShader(outline ? m_outline_ps.Get() : m_fill_ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* const srvs[] = { m_id_srv.Get(), m_style_srv.Get() };
        m_context->PSSetShaderResources(0, 2, srvs);
        float const params[] = { 1, 0.5f, 0, 0 };
        m_context->UpdateSubresource(m_composite_cb.Get(), 0, nullptr, params, 0, 0);
        ID3D11Buffer* const cb = m_composite_cb.Get();
        m_context->PSSetConstantBuffers(0, 1, &cb);
        m_context->Draw(3, 0);
        unbind();
    }

    XMFLOAT4 pixel(UINT x, UINT y)
    {
        D3D11_TEXTURE2D_DESC desc{};
        m_color->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        checked(m_device->CreateTexture2D(&desc, nullptr, &staging), "readback");
        m_context->CopyResource(staging.Get(), m_color.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback map");
        XMFLOAT4 value;
        std::memcpy(&value, static_cast<char const*>(mapped.pData) + y * mapped.RowPitch + x * sizeof(value), sizeof(value));
        m_context->Unmap(staging.Get(), 0);
        return value;
    }

    void expect(UINT x, UINT y, XMFLOAT4 const& expected, char const* message)
    {
        XMFLOAT4 const actual = pixel(x, y);
        bool const equal = std::abs(actual.x - expected.x) < 0.0001f && std::abs(actual.y - expected.y) < 0.0001f &&
            std::abs(actual.z - expected.z) < 0.0001f && std::abs(actual.w - expected.w) < 0.0001f;
        if (!equal)
            std::cerr << message << " got " << actual.x << ',' << actual.y << ',' << actual.z << ',' << actual.w << '\n';
        check(equal, message);
    }

    void check_debug()
    {
        if (!m_info)
            return;
        for (UINT64 i = 0; i < m_info->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes = 0;
            m_info->GetMessage(i, nullptr, &bytes);
            std::vector<char> storage(bytes);
            D3D11_MESSAGE* const message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            checked(m_info->GetMessage(i, message, &bytes), "debug message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
            {
                std::cerr << message->pDescription << '\n';
                throw std::runtime_error("D3D11 debug warning/error");
            }
        }
        std::cout << "PASS: D3D11 debug layer has no warnings/errors\n";
    }

private:
    void unbind()
    {
        ID3D11ShaderResourceView* const empty[2]{};
        m_context->PSSetShaderResources(0, 2, empty);
    }
    ComPtr<ID3D11Buffer> buffer(UINT bytes, UINT flags)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = bytes;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = flags;
        ComPtr<ID3D11Buffer> result;
        checked(m_device->CreateBuffer(&desc, nullptr, &result), "buffer");
        return result;
    }
    ComPtr<ID3D11Texture2D> texture(DXGI_FORMAT format, UINT flags)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = m_size;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.BindFlags = flags;
        ComPtr<ID3D11Texture2D> result;
        checked(m_device->CreateTexture2D(&desc, nullptr, &result), "texture");
        return result;
    }
    UINT m_size;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11InfoQueue> m_info;
    ComPtr<ID3D11VertexShader> m_static, m_skin, m_full;
    ComPtr<ID3D11PixelShader> m_mask_ps, m_fill_ps, m_outline_ps;
    ComPtr<ID3D11InputLayout> m_layout, m_skin_layout;
    ComPtr<ID3D11Buffer> m_draw_cb, m_composite_cb, m_palette_cb, m_vb, m_styles;
    ComPtr<ID3D11Texture2D> m_id, m_color, m_depth;
    ComPtr<ID3D11RenderTargetView> m_id_rtv, m_color_rtv;
    ComPtr<ID3D11ShaderResourceView> m_id_srv, m_style_srv;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    ComPtr<ID3D11DepthStencilState> m_depth_on, m_depth_off;
    ComPtr<ID3D11RasterizerState> m_raster;
    ComPtr<ID3D11BlendState> m_blend, m_no_blend;
};

void run(std::filesystem::path const& root)
{
    XMFLOAT4X4 projection{ 2,0,0,0, 0,2,0,0, 0,0,9,-7, 0,0,2,0 };
    XMFLOAT4X4 const original = projection;
    check(make_private_depth_projection(projection, 0.5f, false) && projection._34 == 1, "scaled near");
    check(std::memcmp(&projection.m[0], &original.m[0], 32) == 0 &&
        std::memcmp(&projection.m[3], &original.m[3], 16) == 0, "xyw preserved");
    check(!make_private_depth_projection(projection, 1, true), "orthographic rejected");
    check(!make_private_depth_projection(projection, 0, false), "invalid near rejected");
    projection._11 = std::numeric_limits<float>::quiet_NaN();
    check(!make_private_depth_projection(projection, 1, false), "nonfinite projection rejected");
    projection = original;
    projection._41 = projection._42 = projection._43 = 0;
    check(!make_private_depth_projection(projection, 1, false), "zero w gradient rejected");
    Fixture f(root);
    for (bool reverse : { false, true })
    {
        f.clear_color();
        f.begin_mask(true);
        for (UINT step = 0; step < 2; ++step)
        {
            UINT const id = reverse ? 2 - step : step + 1;
            float const z = id == 1 ? 2.0f : 4.0f;
            f.rectangle(id, 4, 4, 28, 28, z, z, id == 1);
        }
        f.composite(false);
        f.expect(16, 16, { 0.2f, 0, 0, 0.2f }, "nearest style, reversed order and skin weight");
        f.expect(0, 0, {}, "empty background");
    }
    for (bool reverse : { false, true })
    {
        f.clear_color();
        f.begin_mask(true);
        for (UINT step = 0; step < 2; ++step)
        {
            UINT const id = reverse ? 2 - step : step + 1;
            f.rectangle(id, 4, 4, 28, 28, id == 1 ? 2.0f : 6.0f, id == 1 ? 6.0f : 2.0f);
        }
        f.composite(false);
        f.expect(6, 16, { 0.2f, 0, 0, 0.2f }, "crossing left");
        f.expect(25, 16, { 0, 0, 0.4f, 0.4f }, "crossing right");
    }
    f.clear_color();
    f.begin_mask(true);
    f.rectangle(1, 4, 4, 28, 28, 0.5f, 0.5f);
    f.composite(false);
    f.expect(16, 16, {}, "near clipping");
    f.clear_color();
    f.begin_mask(false);
    f.rectangle(1, 10, 10, 22, 22, 4, 4);
    f.composite(true);
    f.begin_mask(false);
    f.rectangle(2, 4, 4, 28, 28, 2, 2);
    f.composite(true);
    f.expect(9, 16, { 0.4f, 0, 0, 0.4f }, "rear outline inside foreground silhouette");
    f.expect(3, 16, { 0, 0, 0.8f, 0.8f }, "foreground outline color");
    f.expect(16, 16, {}, "outline interiors transparent");
    f.clear_color();
    f.begin_mask(false);
    f.rectangle(1, 4, 8, 18, 24, 2, 2);
    f.rectangle(1, 14, 8, 28, 24, 2, 2);
    f.composite(true);
    f.expect(17, 16, {}, "same-object meshes form union without interior seam");
    f.clear_color();
    for (UINT id : { 1u, 2u })
    {
        f.begin_mask(false);
        f.rectangle(id, 10, 10, 22, 22, 2, 2);
        f.composite(true);
    }
    f.expect(9, 16, { 0.08f, 0, 0.8f, 0.88f }, "stable source-over outline crossing");
    for (UINT id : { 256u, 257u, 299u, 300u })
    {
        f.clear_color();
        f.begin_mask(true);
        f.rectangle(id, 4, 4, 28, 28, 2, 2);
        f.composite(false);
        f.expect(16, 16, { 0.3f * float(id) / 300.0f, 0.3f * (1.0f - float(id) / 300.0f), 0.075f, 0.3f },
            "ID above 255 uses its own distinct style");
    }
    f.styles(2);
    f.styles(512);
    f.check_debug();
    Fixture resized(root, 16);
    resized.clear_color();
    resized.begin_mask(true);
    resized.rectangle(2, 2, 2, 14, 14, 2, 2);
    resized.composite(false);
    resized.expect(8, 8, { 0, 0, 0.4f, 0.4f }, "resources recreated at new size/device");
    resized.check_debug();
    std::cout << "PASS: six production shader entries; private depth; reversed order; crossing depths; skin weights; "
        "near clipping; independent outlines; mesh union; premul crossings; large IDs; resource recreation\n";
}
}

int main(int argc, char** argv)
{
    try
    {
        mask_test::run(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path());
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
