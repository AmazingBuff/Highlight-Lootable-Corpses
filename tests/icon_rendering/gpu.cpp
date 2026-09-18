//
// Created by AmazingBuff on 2026/09/17.
//

#include "render/icon/icon_geometry.h"

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace Amazing::HighlightLootableCorpses
{
using Microsoft::WRL::ComPtr;
void check(bool condition, char const* message);

namespace
{
void checked(HRESULT result, char const* operation)
{
    if (FAILED(result))
    {
        std::cerr << operation << ": " << std::hex << result << std::dec << '\n';
        throw std::runtime_error(operation);
    }
}

ComPtr<ID3DBlob> compile(std::filesystem::path const& path, char const* entry, char const* profile)
{
    ComPtr<ID3DBlob> blob, errors;
    HRESULT const result = D3DCompileFromFile(path.c_str(), nullptr, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (errors)
        std::cerr << static_cast<char const*>(errors->GetBufferPointer());
    checked(result, entry);
    return blob;
}

class Fixture
{
public:
    static constexpr UINT Width = 800, Height = 420;
    explicit Fixture(std::filesystem::path const& root)
    {
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
            nullptr, 0, D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
        if (FAILED(result))
        {
            checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                nullptr, 0, D3D11_SDK_VERSION, &m_device, nullptr, &m_context), "WARP create");
            std::cout << "LIMIT: D3D11 debug layer unavailable\n";
        }
        m_device.As(&m_info);
        ComPtr<ID3DBlob> const vs = compile(root / "src/render/shaders/icon_overlay.hlsl", "vs_main", "vs_5_0");
        ComPtr<ID3DBlob> const ps = compile(root / "src/render/shaders/icon_overlay.hlsl", "ps_main", "ps_5_0");
        checked(m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs), "VS create");
        checked(m_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps), "PS create");
        D3D11_INPUT_ELEMENT_DESC const elements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };
        checked(m_device->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &m_layout), "layout create");
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = static_cast<UINT>(Icon_Marker_Vertex_Count * 16 * sizeof(IconVertex));
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        checked(m_device->CreateBuffer(&buffer, nullptr, &m_buffer), "VB create");
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = Width; texture.Height = Height;
        texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
        texture.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET;
        checked(m_device->CreateTexture2D(&texture, nullptr, &m_color), "color create");
        checked(m_device->CreateRenderTargetView(m_color.Get(), nullptr, &m_target), "RTV create");
        texture.Usage = D3D11_USAGE_STAGING;
        texture.BindFlags = 0;
        texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        checked(m_device->CreateTexture2D(&texture, nullptr, &m_staging), "staging create");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
        checked(m_device->CreateRasterizerState(&raster, &m_raster), "raster create");
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        checked(m_device->CreateDepthStencilState(&depth, &m_depth), "depth create");
        D3D11_BLEND_DESC blend{};
        D3D11_RENDER_TARGET_BLEND_DESC& rt = blend.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        checked(m_device->CreateBlendState(&blend, &m_blend), "blend create");
        ID3D11RenderTargetView* const target = m_target.Get();
        m_context->OMSetRenderTargets(1, &target, nullptr);
        m_context->OMSetBlendState(m_blend.Get(), nullptr, UINT_MAX);
        m_context->OMSetDepthStencilState(m_depth.Get(), 0);
        m_context->RSSetState(m_raster.Get());
        D3D11_VIEWPORT const viewport{ 0, 0, float(Width), float(Height), 0, 1 };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(m_layout.Get());
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* const vb = m_buffer.Get();
        UINT const stride = sizeof(IconVertex), offset = 0;
        m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        m_context->VSSetShader(m_vs.Get(), nullptr, 0);
        m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    }
    ~Fixture() { m_context->ClearState(); }

    void clear()
    {
        float const color[4]{};
        m_context->ClearRenderTargetView(m_target.Get(), color);
    }
    void draw(IconMarker const& marker)
    {
        IconGeometry const geometry = icon_geometry(marker, { 0.1f, 1.0f, 0.4f }, float(Width), float(Height));
        if (geometry.count == 0)
            return;
        D3D11_BOX const box{ 0, 0, 0, static_cast<UINT>(geometry.count * sizeof(IconVertex)), 1, 1 };
        m_context->UpdateSubresource(m_buffer.Get(), 0, &box, geometry.vertices.data(), 0, 0);
        m_context->Draw(static_cast<UINT>(geometry.count), 0);
    }
    std::vector<DirectX::XMFLOAT4> pixels()
    {
        m_context->CopyResource(m_staging.Get(), m_color.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback map");
        std::vector<DirectX::XMFLOAT4> result(Width * Height);
        for (UINT y = 0; y < Height; ++y)
            std::memcpy(result.data() + y * Width, static_cast<char const*>(mapped.pData) + y * mapped.RowPitch, Width * sizeof(DirectX::XMFLOAT4));
        m_context->Unmap(m_staging.Get(), 0);
        return result;
    }
    void check_debug()
    {
        if (!m_info)
            return;
        for (UINT64 index = 0; index < m_info->GetNumStoredMessages(); ++index)
        {
            SIZE_T bytes = 0;
            checked(m_info->GetMessage(index, nullptr, &bytes), "debug size");
            std::vector<char> storage(bytes);
            D3D11_MESSAGE* const message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            checked(m_info->GetMessage(index, message, &bytes), "debug message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
                throw std::runtime_error(message->pDescription);
        }
        std::cout << "PASS: D3D11 debug layer no warnings/errors\n";
    }
private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11InfoQueue> m_info;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11InputLayout> m_layout;
    ComPtr<ID3D11Buffer> m_buffer;
    ComPtr<ID3D11Texture2D> m_color, m_staging;
    ComPtr<ID3D11RenderTargetView> m_target;
    ComPtr<ID3D11RasterizerState> m_raster;
    ComPtr<ID3D11DepthStencilState> m_depth;
    ComPtr<ID3D11BlendState> m_blend;
};

void preview(std::filesystem::path const& root, std::vector<DirectX::XMFLOAT4> const& pixels)
{
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + Fixture::Width * Fixture::Height * 4;
    info.biSize = sizeof(info);
    info.biWidth = Fixture::Width; info.biHeight = -static_cast<LONG>(Fixture::Height);
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::ofstream stream(root / "build/icon_rendering/preview.bmp", std::ios::binary);
    stream.write(reinterpret_cast<char const*>(&file), sizeof(file));
    stream.write(reinterpret_cast<char const*>(&info), sizeof(info));
    for (DirectX::XMFLOAT4 const& pixel : pixels)
    {
        auto const channel = [&](float value) { return static_cast<unsigned char>(std::clamp(value + 0.18f * (1 - pixel.w), 0.0f, 1.0f) * 255); };
        unsigned char const bgra[]{ channel(pixel.z), channel(pixel.y), channel(pixel.x), 255 };
        stream.write(reinterpret_cast<char const*>(bgra), sizeof(bgra));
    }
    check(bool(stream), "preview BMP write");
}
}

void test_gpu(std::filesystem::path const& root)
{
    Fixture fixture(root);
    for (float base : { 5.0f, 10.0f, 20.0f })
    {
        for (bool clustered : { false, true })
        {
            fixture.clear();
            fixture.draw({ 1, clustered ? 2u : 1u, { 100.25f, 100.25f }, icon_radius(base, 2000, 2000, clustered), 0.25f });
            std::size_t fill_count = 0, border_count = 0;
            for (DirectX::XMFLOAT4 const& pixel : fixture.pixels())
            {
                if (pixel.w == 0)
                    continue;
                check(std::abs(pixel.w - 0.25f) < 0.0001f, "border/fill must cover pixel exactly once");
                if (std::abs(pixel.y - 0.25f) < 0.0001f)
                {
                    ++fill_count;
                    check(std::abs(pixel.x - 0.025f) < 0.0001f && std::abs(pixel.z - 0.1f) < 0.0001f, "fill untinted and premultiplied once");
                }
                else
                {
                    ++border_count;
                    check(std::abs(pixel.y - 0.01f) < 0.0001f, "dark border alpha follows marker");
                }
            }
            check(fill_count > 0 && border_count > 0, "minimum-size border and fill visible");
        }
    }
    fixture.clear();
    fixture.draw({ 1, 2, { 100, 100 }, 20, 0 });
    for (DirectX::XMFLOAT4 const& pixel : fixture.pixels())
        check(pixel.w == 0, "zero alpha leaves target empty");
    fixture.clear();
    for (UINT row = 0; row < 3; ++row)
    {
        float const base = row == 0 ? 5.0f : row == 1 ? 10.0f : 20.0f;
        for (UINT column = 0; column < 4; ++column)
        {
            bool const clustered = column >= 2;
            bool const is_far = column % 2 == 1;
            fixture.draw({ row * 4 + column + 1, clustered ? 2u : 1u,
                { 220.0f + column * 155.0f, 140.0f + row * 120.0f },
                icon_radius(base, is_far ? 2000.0f : 0.0f, 2000, clustered), is_far ? 0.4f : 0.9f });
        }
    }
    preview(root, fixture.pixels());
    fixture.check_debug();
    std::cout << "PASS: actual HLSL/WARP single coverage, premultiplied alpha, tiny arrows and preview\n";
}
}
