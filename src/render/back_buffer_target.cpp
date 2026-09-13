//
// Created by AmazingBuff on 2026/9/13.
//

#include "back_buffer_target.h"

#include <d3d11.h>
#include <dxgi.h>

#include <RE/Skyrim.h>

PLUGIN_NAMESPACE_BEGIN

bool BackBufferTarget::ensure(IDXGISwapChain* a_swap_chain, ID3D11Device* a_device)
{
    ID3D11Texture2D* buffer = nullptr;
    HRESULT const hr = a_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer));
    if (FAILED(hr) || !buffer)
        return false;

    if (buffer == m_back_buffer)
    {
        buffer->Release();
        return true;
    }

    if (m_render_target)
    {
        m_render_target->Release();
        m_render_target = nullptr;
    }
    if (m_back_buffer)
    {
        m_back_buffer->Release();
        m_back_buffer = nullptr;
    }

    m_back_buffer = buffer;
    D3D11_TEXTURE2D_DESC desc{};
    buffer->GetDesc(&desc);
    m_width = desc.Width;
    m_height = desc.Height;
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        logger::warn(
            "Back buffer format is {} (expected B8G8R8A8_UNORM=87): HDR swap chain. "
            "R10G10B10A2's alpha channel is only 2-bit (4 levels), overlay opacity is quantized.",
            static_cast<int>(desc.Format));

    HRESULT const rtv_hr = a_device->CreateRenderTargetView(buffer, nullptr, &m_render_target);
    if (FAILED(rtv_hr) || !m_render_target)
    {
        logger::error("Failed to create backbuffer RTV: {:X}", static_cast<unsigned int>(rtv_hr));
        return false;
    }
    return true;
}

PLUGIN_NAMESPACE_END
