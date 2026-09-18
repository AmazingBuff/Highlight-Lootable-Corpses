//
// Created by AmazingBuff on 2026/9/18.
//

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#define PLUGIN_NAMESPACE_BEGIN namespace outline_glow_test {
#define PLUGIN_NAMESPACE_END }
#include "render/mask/outline_glow.h"
#include "render/mask/outline_roi.h"
#undef PLUGIN_NAMESPACE_BEGIN
#undef PLUGIN_NAMESPACE_END

namespace
{
using Microsoft::WRL::ComPtr;
using outline_glow_test::Mask::Glow::KernelProfile;
using outline_glow_test::Mask::Glow::Kernel_Slot_Count;
using outline_glow_test::Mask::Glow::Max_Radius;
using outline_glow_test::Mask::Glow::make_kernel_profile;
using outline_glow_test::Mask::ROI::Rect;
using outline_glow_test::Mask::ROI::RegionKind;
using outline_glow_test::Mask::ROI::Sphere;
using outline_glow_test::Mask::ROI::Viewport;
using outline_glow_test::Mask::ROI::expand;
using outline_glow_test::Mask::ROI::make_region;
using DirectX::XMFLOAT4;

void check(bool condition, char const* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void checked(HRESULT hr, char const* message)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(message) + " HRESULT=" + std::to_string(static_cast<long>(hr)));
}

ComPtr<ID3DBlob> compile(std::filesystem::path const& path, char const* entry, char const* profile)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT const hr = D3DCompileFromFile(path.c_str(), nullptr, nullptr, entry, profile,
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (errors)
        std::cerr << static_cast<char const*>(errors->GetBufferPointer()) << '\n';
    checked(hr, entry);
    return blob;
}

struct GlowCBData
{
    XMFLOAT4 narrow[Kernel_Slot_Count];
    XMFLOAT4 wide[Kernel_Slot_Count];
    uint32_t radius;
    uint32_t width;
    uint32_t height;
    uint32_t object_id;
    int32_t horizontal_left;
    int32_t horizontal_top;
    int32_t horizontal_right;
    int32_t horizontal_bottom;
};
static_assert(sizeof(GlowCBData) == 192);

struct Pixel
{
    float r;
    float g;
    float b;
    float a;
};

uint32_t crc32(std::vector<std::uint8_t> const& bytes)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t value : bytes)
    {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u))));
    }
    return ~crc;
}

uint32_t adler32(std::vector<std::uint8_t> const& bytes)
{
    uint32_t a = 1;
    uint32_t b = 0;
    for (std::uint8_t value : bytes)
    {
        a = (a + value) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void append_be(std::vector<std::uint8_t>& output, uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 24));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_chunk(std::vector<std::uint8_t>& png, char const type[4], std::vector<std::uint8_t> const& data)
{
    append_be(png, static_cast<uint32_t>(data.size()));
    std::vector<std::uint8_t> crc_data(type, type + 4);
    crc_data.insert(crc_data.end(), data.begin(), data.end());
    png.insert(png.end(), crc_data.begin(), crc_data.end());
    append_be(png, crc32(crc_data));
}

void write_png(std::filesystem::path const& path, uint32_t width, uint32_t height, std::vector<Pixel> const& pixels)
{
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 4));
    for (uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x)
        {
            Pixel const& pixel = pixels[static_cast<size_t>(y) * width + x];
            raw.push_back(static_cast<std::uint8_t>(std::clamp(pixel.r, 0.0f, 1.0f) * 255.0f + 0.5f));
            raw.push_back(static_cast<std::uint8_t>(std::clamp(pixel.g, 0.0f, 1.0f) * 255.0f + 0.5f));
            raw.push_back(static_cast<std::uint8_t>(std::clamp(pixel.b, 0.0f, 1.0f) * 255.0f + 0.5f));
            raw.push_back(static_cast<std::uint8_t>(std::clamp(pixel.a, 0.0f, 1.0f) * 255.0f + 0.5f));
        }
    }

    std::vector<std::uint8_t> idat{ 0x78, 0x01 };
    for (size_t offset = 0; offset < raw.size();)
    {
        size_t const remaining = raw.size() - offset;
        uint16_t const length = static_cast<uint16_t>(std::min<size_t>(remaining, 65535));
        bool const final_block = offset + length == raw.size();
        idat.push_back(final_block ? 1 : 0);
        idat.push_back(static_cast<std::uint8_t>(length));
        idat.push_back(static_cast<std::uint8_t>(length >> 8));
        uint16_t const inverse = static_cast<uint16_t>(~length);
        idat.push_back(static_cast<std::uint8_t>(inverse));
        idat.push_back(static_cast<std::uint8_t>(inverse >> 8));
        idat.insert(idat.end(), raw.begin() + offset, raw.begin() + offset + length);
        offset += length;
    }
    uint32_t const checksum = adler32(raw);
    append_be(idat, checksum);

    std::vector<std::uint8_t> png{ 137, 80, 78, 71, 13, 10, 26, 10 };
    std::vector<std::uint8_t> header;
    append_be(header, width);
    append_be(header, height);
    header.insert(header.end(), { 8, 6, 0, 0, 0 });
    append_chunk(png, "IHDR", header);
    append_chunk(png, "IDAT", idat);
    append_chunk(png, "IEND", {});

    std::ofstream stream(path, std::ios::binary);
    check(stream.good(), "open PNG output");
    stream.write(reinterpret_cast<char const*>(png.data()), static_cast<std::streamsize>(png.size()));
}

class Fixture
{
public:
    Fixture(std::filesystem::path const& root, uint32_t size) : m_root(root), m_size(size)
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

        std::filesystem::path const shader_dir = root / "src/render/shaders";
        ComPtr<ID3DBlob> const glow_vs_blob = compile(shader_dir / "mask_glow.hlsl", "vs_main", "vs_5_0");
        ComPtr<ID3DBlob> const horizontal_blob = compile(shader_dir / "mask_glow.hlsl", "ps_glow_horizontal", "ps_5_0");
        ComPtr<ID3DBlob> const vertical_blob = compile(shader_dir / "mask_glow.hlsl", "ps_glow_vertical", "ps_5_0");
        ComPtr<ID3DBlob> const legacy_blob = compile(shader_dir / "mask_composite.hlsl", "ps_outline_main", "ps_5_0");
        checked(m_device->CreateVertexShader(glow_vs_blob->GetBufferPointer(), glow_vs_blob->GetBufferSize(), nullptr, &m_vs), "glow VS");
        checked(m_device->CreatePixelShader(horizontal_blob->GetBufferPointer(), horizontal_blob->GetBufferSize(), nullptr, &m_horizontal), "horizontal PS");
        checked(m_device->CreatePixelShader(vertical_blob->GetBufferPointer(), vertical_blob->GetBufferSize(), nullptr, &m_vertical), "vertical PS");
        checked(m_device->CreatePixelShader(legacy_blob->GetBufferPointer(), legacy_blob->GetBufferSize(), nullptr, &m_legacy), "legacy PS");

        create_resources();
        create_states();
        set_styles({ XMFLOAT4{ 1.0f, 0.15f, 0.05f, 0.75f }, XMFLOAT4{ 0.05f, 0.35f, 1.0f, 0.65f } });
    }

    void set_styles(std::vector<XMFLOAT4> const& styles)
    {
        m_styles.Reset();
        m_style_srv.Reset();
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(styles.size() * sizeof(XMFLOAT4));
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(XMFLOAT4);
        checked(m_device->CreateBuffer(&desc, nullptr, &m_styles), "style buffer");
        m_context->UpdateSubresource(m_styles.Get(), 0, nullptr, styles.data(), 0, 0);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = static_cast<UINT>(styles.size());
        checked(m_device->CreateShaderResourceView(m_styles.Get(), &srv, &m_style_srv), "style SRV");
    }

    void set_mask(std::vector<uint32_t> const& ids)
    {
        check(ids.size() == static_cast<size_t>(m_size) * m_size, "mask dimensions");
        unbind();
        D3D11_BOX const box{ 0, 0, 0, m_size, m_size, 1 };
        m_context->UpdateSubresource(m_id.Get(), 0, &box, ids.data(), m_size * sizeof(uint32_t), 0);
    }

    void clear_color(XMFLOAT4 const& value)
    {
        m_context->ClearRenderTargetView(m_color_rtv.Get(), &value.x);
    }

    Rect full_rect() const noexcept
    {
        return { 0, 0, static_cast<int32_t>(m_size), static_cast<int32_t>(m_size) };
    }

    void draw_glow(uint32_t object_id, int thickness)
    {
        Rect const full = full_rect();
        draw_glow(object_id, thickness, full, full);
    }

    void draw_glow(uint32_t object_id, int thickness, Rect horizontal_rect, Rect vertical_rect)
    {
        KernelProfile const profile = make_kernel_profile(thickness);
        GlowCBData constants{};
        std::memcpy(constants.narrow, profile.narrow.data(), sizeof(profile.narrow));
        std::memcpy(constants.wide, profile.wide.data(), sizeof(profile.wide));
        constants.radius = static_cast<uint32_t>(profile.radius);
        constants.width = m_size;
        constants.height = m_size;
        constants.object_id = object_id;
        constants.horizontal_left = horizontal_rect.left;
        constants.horizontal_top = horizontal_rect.top;
        constants.horizontal_right = horizontal_rect.right;
        constants.horizontal_bottom = horizontal_rect.bottom;

        unbind();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(m_context->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "glow CB map");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        m_context->Unmap(m_cb.Get(), 0);
        setup_fullscreen();
        ID3D11Buffer* cb = m_cb.Get();
        m_context->PSSetConstantBuffers(0, 1, &cb);

        ID3D11RenderTargetView* scratch = m_scratch_rtv.Get();
        m_context->OMSetRenderTargets(1, &scratch, nullptr);
        m_context->OMSetBlendState(m_opaque.Get(), nullptr, UINT_MAX);
        m_context->RSSetState(m_scissor.Get());
        D3D11_RECT const h_scissor{ horizontal_rect.left, horizontal_rect.top, horizontal_rect.right, horizontal_rect.bottom };
        m_context->RSSetScissorRects(1, &h_scissor);
        m_context->PSSetShader(m_horizontal.Get(), nullptr, 0);
        ID3D11ShaderResourceView* horizontal[] = { m_id_srv.Get() };
        m_context->PSSetShaderResources(0, 1, horizontal);
        m_context->Draw(3, 0);

        unbind();
        ID3D11RenderTargetView* color = m_color_rtv.Get();
        m_context->OMSetRenderTargets(1, &color, nullptr);
        m_context->OMSetBlendState(m_premul.Get(), nullptr, UINT_MAX);
        m_context->RSSetState(m_scissor.Get());
        D3D11_RECT const v_scissor{ vertical_rect.left, vertical_rect.top, vertical_rect.right, vertical_rect.bottom };
        m_context->RSSetScissorRects(1, &v_scissor);
        m_context->PSSetShader(m_vertical.Get(), nullptr, 0);
        ID3D11ShaderResourceView* vertical[] = { m_id_srv.Get(), m_scratch_srv.Get(), m_style_srv.Get() };
        m_context->PSSetShaderResources(0, 3, vertical);
        m_context->Draw(3, 0);
        unbind();
    }

    void poison_scratch(XMFLOAT4 const& value)
    {
        unbind();
        m_context->ClearRenderTargetView(m_scratch_rtv.Get(), &value.x);
    }

    void draw_legacy(int radius)
    {
        float const constants[4] = { static_cast<float>(radius), 1.0f, 0.0f, 0.0f };
        unbind();
        m_context->UpdateSubresource(m_legacy_cb.Get(), 0, nullptr, constants, 0, 0);
        setup_fullscreen();
        ID3D11RenderTargetView* color = m_color_rtv.Get();
        m_context->OMSetRenderTargets(1, &color, nullptr);
        m_context->OMSetBlendState(m_premul.Get(), nullptr, UINT_MAX);
        m_context->PSSetShader(m_legacy.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { m_id_srv.Get(), m_style_srv.Get() };
        m_context->PSSetShaderResources(0, 2, srvs);
        ID3D11Buffer* cb = m_legacy_cb.Get();
        m_context->PSSetConstantBuffers(0, 1, &cb);
        m_context->Draw(3, 0);
        unbind();
    }

    Pixel pixel(uint32_t x, uint32_t y)
    {
        D3D11_TEXTURE2D_DESC desc{};
        m_color->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        checked(m_device->CreateTexture2D(&desc, nullptr, &staging), "readback texture");
        m_context->CopyResource(staging.Get(), m_color.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback map");
        Pixel result{};
        std::memcpy(&result, static_cast<char const*>(mapped.pData) + y * mapped.RowPitch + x * sizeof(Pixel), sizeof(Pixel));
        m_context->Unmap(staging.Get(), 0);
        return result;
    }

    std::vector<Pixel> pixels()
    {
        std::vector<Pixel> result(static_cast<size_t>(m_size) * m_size);
        D3D11_TEXTURE2D_DESC desc{};
        m_color->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        checked(m_device->CreateTexture2D(&desc, nullptr, &staging), "image readback");
        m_context->CopyResource(staging.Get(), m_color.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "image readback map");
        for (uint32_t y = 0; y < m_size; ++y)
            std::memcpy(result.data() + static_cast<size_t>(y) * m_size,
                static_cast<char const*>(mapped.pData) + y * mapped.RowPitch, static_cast<size_t>(m_size) * sizeof(Pixel));
        m_context->Unmap(staging.Get(), 0);
        return result;
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
            D3D11_MESSAGE* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            checked(m_info->GetMessage(i, message, &bytes), "debug queue message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
                throw std::runtime_error(std::string("D3D11 debug warning: ") + message->pDescription);
        }
        std::cout << "PASS: D3D11 debug layer has no warnings/errors\n";
    }

    uint32_t size() const noexcept { return m_size; }

private:
    void create_resources()
    {
        auto make_texture = [&](DXGI_FORMAT format, UINT bind_flags) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = m_size;
            desc.Height = m_size;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = bind_flags;
            ComPtr<ID3D11Texture2D> texture;
            checked(m_device->CreateTexture2D(&desc, nullptr, &texture), "texture");
            return texture;
        };
        m_id = make_texture(DXGI_FORMAT_R32_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        m_scratch = make_texture(DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        m_color = make_texture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
        checked(m_device->CreateRenderTargetView(m_id.Get(), nullptr, &m_id_rtv), "mask RTV");
        checked(m_device->CreateShaderResourceView(m_id.Get(), nullptr, &m_id_srv), "mask SRV");
        checked(m_device->CreateRenderTargetView(m_scratch.Get(), nullptr, &m_scratch_rtv), "scratch RTV");
        checked(m_device->CreateShaderResourceView(m_scratch.Get(), nullptr, &m_scratch_srv), "scratch SRV");
        checked(m_device->CreateRenderTargetView(m_color.Get(), nullptr, &m_color_rtv), "color RTV");

        D3D11_BUFFER_DESC cb_desc{};
        cb_desc.ByteWidth = sizeof(GlowCBData);
        cb_desc.Usage = D3D11_USAGE_DYNAMIC;
        cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        checked(m_device->CreateBuffer(&cb_desc, nullptr, &m_cb), "glow CB");
        cb_desc.ByteWidth = 16;
        cb_desc.Usage = D3D11_USAGE_DEFAULT;
        cb_desc.CPUAccessFlags = 0;
        checked(m_device->CreateBuffer(&cb_desc, nullptr, &m_legacy_cb), "legacy CB");
    }

    void create_states()
    {
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        checked(m_device->CreateBlendState(&blend, &m_opaque), "opaque blend");
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        checked(m_device->CreateBlendState(&blend, &m_premul), "premultiplied blend");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        checked(m_device->CreateDepthStencilState(&depth, &m_depth_none), "depth state");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        checked(m_device->CreateRasterizerState(&raster, &m_raster), "raster state");
        raster.ScissorEnable = TRUE;
        checked(m_device->CreateRasterizerState(&raster, &m_scissor), "scissor raster state");
    }

    void setup_fullscreen()
    {
        D3D11_VIEWPORT const viewport{ 0.0f, 0.0f, static_cast<float>(m_size), static_cast<float>(m_size), 0.0f, 1.0f };
        m_context->RSSetViewports(1, &viewport);
        m_context->RSSetState(m_raster.Get());
        m_context->OMSetDepthStencilState(m_depth_none.Get(), 0);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* no_vb = nullptr;
        UINT zero = 0;
        m_context->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
        m_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    }

    void unbind()
    {
        ID3D11ShaderResourceView* empty[3]{};
        m_context->PSSetShaderResources(0, 3, empty);
    }

    std::filesystem::path m_root;
    uint32_t m_size;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11InfoQueue> m_info;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_horizontal;
    ComPtr<ID3D11PixelShader> m_vertical;
    ComPtr<ID3D11PixelShader> m_legacy;
    ComPtr<ID3D11Buffer> m_cb;
    ComPtr<ID3D11Buffer> m_legacy_cb;
    ComPtr<ID3D11Buffer> m_styles;
    ComPtr<ID3D11ShaderResourceView> m_style_srv;
    ComPtr<ID3D11Texture2D> m_id;
    ComPtr<ID3D11Texture2D> m_scratch;
    ComPtr<ID3D11Texture2D> m_color;
    ComPtr<ID3D11RenderTargetView> m_id_rtv;
    ComPtr<ID3D11RenderTargetView> m_scratch_rtv;
    ComPtr<ID3D11RenderTargetView> m_color_rtv;
    ComPtr<ID3D11ShaderResourceView> m_id_srv;
    ComPtr<ID3D11ShaderResourceView> m_scratch_srv;
    ComPtr<ID3D11BlendState> m_opaque;
    ComPtr<ID3D11BlendState> m_premul;
    ComPtr<ID3D11DepthStencilState> m_depth_none;
    ComPtr<ID3D11RasterizerState> m_raster;
    ComPtr<ID3D11RasterizerState> m_scissor;
};

void check_kernel_profiles()
{
    for (int thickness = 1; thickness <= 5; ++thickness)
    {
        KernelProfile const profile = make_kernel_profile(thickness);
        check(profile.radius == 3 * thickness + 3, "radius mapping");
        check(profile.radius >= 6 && profile.radius <= Max_Radius, "radius bounds");
        float narrow_sum = profile.narrow[0];
        float wide_sum = profile.wide[0];
        for (int distance = 1; distance <= profile.radius; ++distance)
        {
            narrow_sum += 2.0f * profile.narrow[distance];
            wide_sum += 2.0f * profile.wide[distance];
            check(profile.narrow[distance] <= profile.narrow[distance - 1], "narrow monotonic weights");
            check(profile.wide[distance] <= profile.wide[distance - 1], "wide monotonic weights");
        }
        check(std::abs(narrow_sum - 1.0f) < 1e-5f, "narrow normalization");
        check(std::abs(wide_sum - 1.0f) < 1e-5f, "wide normalization");
        check(profile.narrow[profile.radius] >= 0.0f && profile.wide[profile.radius] >= 0.0f, "finite kernel support");
    }
    std::cout << "PASS: normalized bounded kernel profiles for thickness 1..5\n";
}

void check_roi_helper()
{
    DirectX::XMFLOAT4X4 const identity{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f };
    Viewport const viewport{ 100, 80 };
    std::array<Sphere, 1> sphere{ Sphere{ 0.0, 0.0, 0.0, 0.2 } };
    auto const projected = make_region(sphere, identity, viewport);
    check(projected.kind == RegionKind::e_rect, "perspective ROI rectangle");
    check(projected.rect.left == 38 && projected.rect.top == 30 && projected.rect.right == 62 && projected.rect.bottom == 50,
        "half-open ROI rounding and margin");

    std::array<Sphere, 2> union_spheres{ Sphere{ -0.5, 0.0, 0.0, 0.2 }, Sphere{ 0.5, 0.0, 0.0, 0.2 } };
    auto const united = make_region(union_spheres, identity, viewport);
    check(united.kind == RegionKind::e_rect && united.rect.left < projected.rect.left && united.rect.right > projected.rect.right,
        "union ROI bounds");
    auto const horizontal = expand(projected, 9, true, false, viewport);
    auto const vertical = expand(projected, 9, true, true, viewport);
    check(horizontal.kind == RegionKind::e_rect && horizontal.rect.left == 29 && horizontal.rect.top == projected.rect.top &&
        horizontal.rect.right == 71 && horizontal.rect.bottom == projected.rect.bottom, "horizontal dependency region");
    check(vertical.kind == RegionKind::e_rect && vertical.rect.left == 29 && vertical.rect.top == 21 &&
        vertical.rect.right == 71 && vertical.rect.bottom == 59, "vertical dependency region");

    DirectX::XMFLOAT4X4 const perspective{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 0.0f };
    std::array<Sphere, 1> perspective_sphere{ Sphere{ 0.0, 0.0, 10.0, 2.0 } };
    auto const perspective_region = make_region(perspective_sphere, perspective, viewport);
    check(perspective_region.kind == RegionKind::e_rect && perspective_region.rect.left == 35 &&
        perspective_region.rect.top == 28 && perspective_region.rect.right == 65 && perspective_region.rect.bottom == 52,
        "perspective projection and near-safe bounds");
    std::array<Sphere, 1> perspective_near{ Sphere{ 0.0, 0.0, 1.1, 0.2 } };
    check(make_region(perspective_near, perspective, viewport).kind == RegionKind::e_full, "perspective near crossing fallback");
    std::array<Sphere, 1> perspective_behind{ Sphere{ 0.0, 0.0, -10.0, 1.0 } };
    check(make_region(perspective_behind, perspective, viewport).kind == RegionKind::e_full, "perspective behind-camera fallback");
    DirectX::XMFLOAT4X4 const translated_perspective{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, -100.0f };
    std::array<Sphere, 1> translated_sphere{ Sphere{ 0.0, 0.0, 110.0, 2.0 } };
    auto const translated_region = make_region(translated_sphere, translated_perspective, viewport);
    check(translated_region.kind == RegionKind::e_rect && translated_region.rect.left == perspective_region.rect.left &&
        translated_region.rect.top == perspective_region.rect.top && translated_region.rect.right == perspective_region.rect.right &&
        translated_region.rect.bottom == perspective_region.rect.bottom, "translated perspective bounds");
    std::array<Sphere, 1> edge_sphere{ Sphere{ 9.5, 0.0, 10.0, 0.1 } };
    check(make_region(edge_sphere, perspective, viewport).rect.right == viewport.width, "viewport edge clipping");
    DirectX::XMFLOAT4X4 invalid_matrix = identity;
    invalid_matrix.m[0][0] = std::numeric_limits<float>::quiet_NaN();
    check(make_region(sphere, invalid_matrix, viewport).kind == RegionKind::e_full, "invalid matrix full fallback");

    std::array<Sphere, 1> offscreen{ Sphere{ 3.0, 0.0, 0.0, 0.1 } };
    check(make_region(offscreen, identity, viewport).kind == RegionKind::e_empty, "offscreen ROI empty");
    std::array<Sphere, 1> near_crossing{ Sphere{ 0.0, 0.0, 1.0, 0.2 } };
    check(make_region(near_crossing, identity, viewport).kind == RegionKind::e_full, "near crossing full fallback");
    std::array<Sphere, 1> zero_bound{ Sphere{ 0.0, 0.0, 0.0, 0.0 } };
    check(make_region(zero_bound, identity, viewport).kind == RegionKind::e_full, "zero bound full fallback");
    std::array<Sphere, 1> invalid_bound{ Sphere{ std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.2 } };
    check(make_region(invalid_bound, identity, viewport).kind == RegionKind::e_full, "invalid bound full fallback");
    std::cout << "PASS: ROI projection, union, viewport clipping, near/invalid fallback, half-open rounding, and H/V domains\n";
}

void compile_regressions(std::filesystem::path const& root)
{
    std::filesystem::path const shaders = root / "src/render/shaders";
    compile(shaders / "mask_composite.hlsl", "vs_main", "vs_5_0");
    compile(shaders / "mask_composite.hlsl", "ps_silhouette_main", "ps_5_0");
    compile(shaders / "mask_geometry.hlsl", "vs_static_main", "vs_5_0");
    compile(shaders / "mask_geometry.hlsl", "vs_skinned_main", "vs_5_0");
    compile(shaders / "mask_geometry.hlsl", "ps_main", "ps_5_0");
    std::cout << "PASS: silhouette and geometry shader entrypoints compile\n";
}

void compare_full_and_local(Fixture& fixture, std::vector<uint32_t> const& mask, uint32_t object_id,
    int thickness, Rect horizontal_rect, Rect vertical_rect, char const* label)
{
    fixture.set_mask(mask);
    fixture.clear_color({ 0, 0, 0, 0 });
    fixture.draw_glow(object_id, thickness);
    std::vector<Pixel> const full = fixture.pixels();

    fixture.set_mask(mask);
    fixture.clear_color({ 0, 0, 0, 0 });
    fixture.poison_scratch({ 0.73f, 0.19f, 0.41f, 0.91f });
    fixture.draw_glow(object_id, thickness, horizontal_rect, vertical_rect);
    std::vector<Pixel> const local = fixture.pixels();
    float max_delta = 0.0f;
    for (size_t i = 0; i < full.size(); ++i)
    {
        max_delta = std::max(max_delta, std::abs(full[i].r - local[i].r));
        max_delta = std::max(max_delta, std::abs(full[i].g - local[i].g));
        max_delta = std::max(max_delta, std::abs(full[i].b - local[i].b));
        max_delta = std::max(max_delta, std::abs(full[i].a - local[i].a));
    }
    check(max_delta <= 0.002f, label);
    std::cout << "PASS: " << label << " max_delta=" << max_delta << "\n";
}

void run(std::filesystem::path const& root, std::filesystem::path const& output)
{
    check_kernel_profiles();
    check_roi_helper();
    compile_regressions(root);
    Fixture fixture(root, 40);
    std::vector<uint32_t> rectangle(40 * 40, 0);
    for (uint32_t y = 12; y < 28; ++y)
        for (uint32_t x = 12; x < 28; ++x)
            rectangle[static_cast<size_t>(y) * 40 + x] = 1;

    Rect const base_region{ 10, 10, 30, 30 };
    Viewport const fixture_viewport{ 40, 40 };
    int const comparison_radius = make_kernel_profile(2).radius;
    Rect const horizontal_region = expand({ RegionKind::e_rect, base_region }, comparison_radius, true, false, fixture_viewport).rect;
    Rect const vertical_region = expand({ RegionKind::e_rect, base_region }, comparison_radius, true, true, fixture_viewport).rect;
    compare_full_and_local(fixture, rectangle, 1, 2, horizontal_region, vertical_region, "local glow matches full-screen reference");
    for (int thickness : { 1, 5 })
    {
        int const radius = make_kernel_profile(thickness).radius;
        Rect const h_rect = expand({ RegionKind::e_rect, base_region }, radius, true, false, fixture_viewport).rect;
        Rect const v_rect = expand({ RegionKind::e_rect, base_region }, radius, true, true, fixture_viewport).rect;
        compare_full_and_local(fixture, rectangle, 1, thickness, h_rect, v_rect, "thickness ROI matches full-screen reference");
    }

    std::vector<uint32_t> moving_mask(40 * 40, 0);
    for (uint32_t y = 6; y < 24; ++y)
        for (uint32_t x = 2; x < 12; ++x)
            moving_mask[static_cast<size_t>(y) * 40 + x] = 1;
    Rect const moving_base{ 0, 4, 14, 26 };
    Rect const moving_horizontal = expand({ RegionKind::e_rect, moving_base }, comparison_radius, true, false, fixture_viewport).rect;
    Rect const moving_vertical = expand({ RegionKind::e_rect, moving_base }, comparison_radius, true, true, fixture_viewport).rect;
    compare_full_and_local(fixture, moving_mask, 1, 2, moving_horizontal, moving_vertical, "moving disjoint ROI matches full-screen reference");
    fixture.set_mask(std::vector<uint32_t>(40 * 40, 0));
    fixture.clear_color({ 0, 0, 0, 0 });
    fixture.poison_scratch({ 0.61f, 0.31f, 0.17f, 0.88f });
    fixture.draw_glow(1, 2, moving_horizontal, moving_vertical);
    check(fixture.pixel(20, 20).a == 0.0f, "empty frame leaves no reused ID coverage");
    compare_full_and_local(fixture, rectangle, 1, 2, horizontal_region, vertical_region, "reused object ID after empty frame");
    size_t const full_work = static_cast<size_t>(fixture.size()) * fixture.size() * 2;
    size_t const local_work = static_cast<size_t>(horizontal_region.right - horizontal_region.left) *
                                  (horizontal_region.bottom - horizontal_region.top) +
                              static_cast<size_t>(vertical_region.right - vertical_region.left) *
                                  (vertical_region.bottom - vertical_region.top);
    check(local_work < full_work, "local ROI reduces processed area");
    std::cout << "PASS: local H/V work areas full=" << full_work << " local=" << local_work
              << " reduction=" << (100.0 * static_cast<double>(full_work - local_work) / static_cast<double>(full_work)) << "%\n";

    std::vector<uint32_t> first_id(40 * 40, 0);
    std::vector<uint32_t> second_id(40 * 40, 0);
    std::vector<uint32_t> retained_ids(40 * 40, 0);
    for (uint32_t y = 8; y < 30; ++y)
        for (uint32_t x = 6; x < 23; ++x)
            first_id[static_cast<size_t>(y) * 40 + x] = 1;
    for (uint32_t y = 8; y < 30; ++y)
        for (uint32_t x = 18; x < 35; ++x)
            second_id[static_cast<size_t>(y) * 40 + x] = 2;
    retained_ids = first_id;
    for (size_t i = 0; i < retained_ids.size(); ++i)
        if (second_id[i] != 0)
            retained_ids[i] = second_id[i];
    Rect const second_base{ 16, 6, 37, 32 };
    Rect const second_h = expand({ RegionKind::e_rect, second_base }, comparison_radius, true, false, fixture_viewport).rect;
    Rect const second_v = expand({ RegionKind::e_rect, second_base }, comparison_radius, true, true, fixture_viewport).rect;
    fixture.clear_color({ 0, 0, 0, 0 });
    fixture.set_mask(first_id);
    fixture.draw_glow(1, 2);
    fixture.set_mask(retained_ids);
    fixture.poison_scratch({ 0.37f, 0.62f, 0.11f, 0.83f });
    fixture.draw_glow(2, 2, second_h, second_v);
    std::vector<Pixel> const mixed_local = fixture.pixels();
    fixture.clear_color({ 0, 0, 0, 0 });
    fixture.set_mask(first_id);
    fixture.draw_glow(1, 2);
    fixture.set_mask(second_id);
    fixture.draw_glow(2, 2);
    std::vector<Pixel> const mixed_reference = fixture.pixels();
    float mixed_delta = 0.0f;
    for (size_t i = 0; i < mixed_local.size(); ++i)
    {
        mixed_delta = std::max(mixed_delta, std::abs(mixed_local[i].r - mixed_reference[i].r));
        mixed_delta = std::max(mixed_delta, std::abs(mixed_local[i].g - mixed_reference[i].g));
        mixed_delta = std::max(mixed_delta, std::abs(mixed_local[i].b - mixed_reference[i].b));
        mixed_delta = std::max(mixed_delta, std::abs(mixed_local[i].a - mixed_reference[i].a));
    }
    check(mixed_delta <= 0.002f, "foreign IDs do not suppress later glow");
    std::cout << "PASS: mixed retained-ID reference max_delta=" << mixed_delta << "\n";

    fixture.clear_color({ 0.02f, 0.02f, 0.02f, 0.0f });
    fixture.set_mask(rectangle);
    fixture.draw_glow(1, 2);
    std::vector<Pixel> glow_pixels = fixture.pixels();
    Pixel const background = glow_pixels.front();
    Pixel const interior = glow_pixels[20 * 40 + 20];
    check(std::abs(interior.r - background.r) < 0.01f && std::abs(interior.g - background.g) < 0.01f &&
        std::abs(interior.b - background.b) < 0.01f && std::abs(interior.a - background.a) < 0.01f, "transparent raw-mask interior");
    float previous_alpha = std::numeric_limits<float>::max();
    for (uint32_t distance = 1; distance <= 9; ++distance)
    {
        float const alpha = glow_pixels[20 * 40 + 27 + distance].a;
        check(alpha <= previous_alpha + 0.002f, "straight-edge monotonic fade");
        previous_alpha = alpha;
    }
    check(glow_pixels[20 * 40 + 28].a > glow_pixels[20 * 40 + 35].a, "tight core and diminishing halo");
    check(glow_pixels[20 * 40 + 37].a <= 0.01f, "finite support beyond radius");

    fixture.set_styles({ XMFLOAT4{ 1.0f, 0.15f, 0.05f, 0.0f }, XMFLOAT4{ 0.05f, 0.35f, 1.0f, 0.65f } });
    fixture.clear_color({ 0, 0, 0, 0 });
    fixture.set_mask(rectangle);
    fixture.draw_glow(1, 2);
    Pixel const zero_alpha = fixture.pixel(28, 20);
    check(zero_alpha.r < 0.001f && zero_alpha.g < 0.001f && zero_alpha.b < 0.001f && zero_alpha.a < 0.001f, "zero target alpha");
    fixture.set_styles({ XMFLOAT4{ 1.0f, 0.15f, 0.05f, 1.0f }, XMFLOAT4{ 0.05f, 0.35f, 1.0f, 0.65f } });
    fixture.clear_color({ 0, 0, 0, 0 });
    fixture.draw_glow(1, 2);
    Pixel const full_alpha = fixture.pixel(28, 20);
    fixture.set_styles({ XMFLOAT4{ 1.0f, 0.15f, 0.05f, 0.5f }, XMFLOAT4{ 0.05f, 0.35f, 1.0f, 0.65f } });
    fixture.clear_color({ 0, 0, 0, 0 });
    fixture.draw_glow(1, 2);
    Pixel const half_alpha = fixture.pixel(28, 20);
    check(std::abs(half_alpha.a * 2.0f - full_alpha.a) < 0.02f && std::abs(half_alpha.r * 2.0f - full_alpha.r) < 0.02f,
        "target alpha scales premultiplied output once");
    fixture.set_styles({ XMFLOAT4{ 1.0f, 0.15f, 0.05f, 0.75f }, XMFLOAT4{ 0.05f, 0.35f, 1.0f, 0.65f } });

    std::vector<uint32_t> edge_mask(40 * 40, 0);
    for (uint32_t y = 10; y < 30; ++y)
        for (uint32_t x = 0; x < 12; ++x)
            edge_mask[static_cast<size_t>(y) * 40 + x] = 1;
    fixture.clear_color({ 0, 0, 0, 0 });
    fixture.set_mask(edge_mask);
    fixture.draw_glow(1, 2);
    check(fixture.pixel(12, 20).a > fixture.pixel(39, 20).a, "screen-edge coverage");

    fixture.clear_color({ 0.02f, 0.02f, 0.02f, 0.0f });
    fixture.set_mask(std::vector<uint32_t>(40 * 40, 0));
    fixture.draw_glow(1, 5);
    Pixel const empty = fixture.pixel(20, 20);
    check(std::abs(empty.r - 0.02f) < 0.01f && std::abs(empty.g - 0.02f) < 0.01f && std::abs(empty.b - 0.02f) < 0.01f && empty.a < 0.01f,
        "empty frame unchanged");

    Fixture preview(root, 96);
    std::vector<uint32_t> preview_mask(96 * 96, 0);
    for (uint32_t y = 6; y < 74; ++y)
        for (uint32_t x = 31; x < 66; ++x)
        {
            float const dx = static_cast<float>(x) - 48.0f;
            float const dy = static_cast<float>(y) - 47.0f;
            bool const torso = (dx * dx) / (18.0f * 18.0f) + (dy * dy) / (28.0f * 28.0f) <= 1.0f;
            bool const head = (static_cast<float>(x) - 48.0f) * (static_cast<float>(x) - 48.0f) +
                (static_cast<float>(y) - 15.0f) * (static_cast<float>(y) - 15.0f) <= 9.0f * 9.0f;
            if (torso || head)
                preview_mask[static_cast<size_t>(y) * 96 + x] = 1;
        }
    for (uint32_t y = 70; y < 84; ++y)
        for (uint32_t x = 35; x < 45; ++x)
            preview_mask[static_cast<size_t>(y) * 96 + x] = 1;
    for (uint32_t y = 70; y < 84; ++y)
        for (uint32_t x = 52; x < 62; ++x)
            preview_mask[static_cast<size_t>(y) * 96 + x] = 1;
    preview.set_mask(preview_mask);
    preview.clear_color({ 0.02f, 0.02f, 0.02f, 1.0f });
    preview.draw_legacy(2);
    std::vector<Pixel> old_dark = preview.pixels();
    preview.clear_color({ 0.72f, 0.72f, 0.72f, 1.0f });
    preview.draw_legacy(2);
    std::vector<Pixel> old_light = preview.pixels();
    preview.clear_color({ 0.02f, 0.02f, 0.02f, 1.0f });
    preview.draw_glow(1, 2);
    std::vector<Pixel> new_dark = preview.pixels();
    preview.clear_color({ 0.72f, 0.72f, 0.72f, 1.0f });
    preview.draw_glow(1, 2);
    std::vector<Pixel> new_light = preview.pixels();

    uint32_t const panel = preview.size();
    std::vector<Pixel> comparison(static_cast<size_t>(panel) * panel * 4);
    for (uint32_t y = 0; y < panel; ++y)
        for (uint32_t x = 0; x < panel; ++x)
        {
            size_t const source = static_cast<size_t>(y) * panel + x;
            comparison[static_cast<size_t>(y) * panel * 4 + x] = old_dark[source];
            comparison[static_cast<size_t>(y) * panel * 4 + panel + x] = new_dark[source];
            comparison[static_cast<size_t>(y) * panel * 4 + panel * 2 + x] = old_light[source];
            comparison[static_cast<size_t>(y) * panel * 4 + panel * 3 + x] = new_light[source];
        }
    std::filesystem::create_directories(output);
    write_png(output / "outline_glow_comparison.png", panel * 4, panel, comparison);
    std::ofstream legend(output / "outline_glow_comparison.txt");
    check(legend.good(), "open PNG legend");
    legend << "Panels left-to-right: legacy hard band on dark background; new glow on dark background; "
              "legacy hard band on light background; new glow on light background.\n";

    fixture.clear_color({ 0.0f, 0.0f, 0.0f, 0.0f });
    std::vector<uint32_t> first(40 * 40, 0);
    std::vector<uint32_t> second(40 * 40, 0);
    for (uint32_t y = 8; y < 30; ++y)
        for (uint32_t x = 8; x < 25; ++x)
            first[static_cast<size_t>(y) * 40 + x] = 1;
    for (uint32_t y = 8; y < 30; ++y)
        for (uint32_t x = 19; x < 35; ++x)
            second[static_cast<size_t>(y) * 40 + x] = 2;
    fixture.set_mask(first);
    fixture.draw_glow(1, 2);
    Pixel const first_halo = fixture.pixel(26, 19);
    fixture.set_mask(second);
    fixture.draw_glow(2, 2);
    Pixel const retained = fixture.pixel(26, 19);
    Pixel const second_edge = fixture.pixel(36, 19);
    check(retained.r > 0.0f && retained.r >= first_halo.r * 0.95f, "overlap retains earlier halo");
    check(second_edge.b > second_edge.r * 1.4f && second_edge.b > second_edge.g * 1.2f, "overlap draws later target color");

    Fixture resized(root, 24);
    std::vector<uint32_t> small_mask(24 * 24, 0);
    for (uint32_t y = 5; y < 19; ++y)
        for (uint32_t x = 5; x < 19; ++x)
            small_mask[static_cast<size_t>(y) * 24 + x] = 1;
    resized.clear_color({ 0, 0, 0, 0 });
    resized.set_mask(small_mask);
    resized.draw_glow(1, 1);
    check(resized.pixel(12, 12).a == 0.0f, "resized scratch keeps interior transparent");
    fixture.check_debug();
    resized.check_debug();
    std::cout << "PASS: WARP glow core/halo, empty/interior, colors, overlap retention, screen edge, resize, and PNG preview\n";
}
}

int main(int argc, char** argv)
{
    try
    {
        std::filesystem::path const root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
        std::filesystem::path const output = argc > 2 ? std::filesystem::path(argv[2]) : root / "build/outline_glow";
        run(root, output);
        return 0;
    }
    catch (std::exception const& e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
