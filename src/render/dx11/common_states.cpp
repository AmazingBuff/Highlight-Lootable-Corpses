//
// Created by AmazingBuff on 2026/9/18.
//

#include "common_states.h"

#include <cfloat>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    // State creation helpers mirroring DirectXTK's CommonStates::Impl one-to-one
    // (pinned source tree build/vcpkg_installed/vcpkg/blds/directxtk/src/
    // may2026-a68e548ea6.clean/Src/CommonStates.cpp), re-typed to REX::W32.

    bool create_blend_state(REX::W32::ID3D11Device* device, REX::W32::D3D11_BLEND src_blend, REX::W32::D3D11_BLEND dest_blend,
        REX::W32::ID3D11BlendState** result)
    {
        REX::W32::D3D11_BLEND_DESC desc{};

        desc.renderTarget[0].blendEnable = (src_blend != REX::W32::D3D11_BLEND_ONE) || (dest_blend != REX::W32::D3D11_BLEND_ZERO);
        desc.renderTarget[0].srcBlend = desc.renderTarget[0].srcBlendAlpha = src_blend;
        desc.renderTarget[0].destBlend = desc.renderTarget[0].destBlendAlpha = dest_blend;
        desc.renderTarget[0].blendOp = desc.renderTarget[0].blendOpAlpha = REX::W32::D3D11_BLEND_OP_ADD;

        desc.renderTarget[0].renderTargetWriteMask = REX::W32::D3D11_COLOR_WRITE_ENABLE_ALL;

        REX::W32::HRESULT const hr = device->CreateBlendState(&desc, result);
        if (!REX::W32::SUCCESS(hr) || !*result)
        {
            logger::error("CommonStates: CreateBlendState failed ({:X})", static_cast<unsigned int>(hr));
            return false;
        }
        return true;
    }

    bool create_depth_stencil_state(REX::W32::ID3D11Device* device, bool enable, bool write_enable,
        REX::W32::ID3D11DepthStencilState** result)
    {
        REX::W32::D3D11_DEPTH_STENCIL_DESC desc{};

        desc.depthEnable = enable;
        desc.depthWriteMask = write_enable ? REX::W32::D3D11_DEPTH_WRITE_MASK_ALL : REX::W32::D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.depthFunc = REX::W32::D3D11_COMPARISON_LESS_EQUAL;

        desc.stencilEnable = false;
        desc.stencilReadMask = REX::W32::D3D11_DEFAULT_STENCIL_READ_MASK;
        desc.stencilWriteMask = REX::W32::D3D11_DEFAULT_STENCIL_WRITE_MASK;

        desc.frontFace.stencilFunc = REX::W32::D3D11_COMPARISON_ALWAYS;
        desc.frontFace.stencilPassOp = REX::W32::D3D11_STENCIL_OP_KEEP;
        desc.frontFace.stencilFailOp = REX::W32::D3D11_STENCIL_OP_KEEP;
        desc.frontFace.stencilDepthFailOp = REX::W32::D3D11_STENCIL_OP_KEEP;

        desc.backFace = desc.frontFace;

        REX::W32::HRESULT const hr = device->CreateDepthStencilState(&desc, result);
        if (!REX::W32::SUCCESS(hr) || !*result)
        {
            logger::error("CommonStates: CreateDepthStencilState failed ({:X})", static_cast<unsigned int>(hr));
            return false;
        }
        return true;
    }

    bool create_rasterizer_state(REX::W32::ID3D11Device* device, REX::W32::D3D11_CULL_MODE cull_mode, REX::W32::D3D11_FILL_MODE fill_mode, bool scissor_enable,
        REX::W32::ID3D11RasterizerState** result)
    {
        REX::W32::D3D11_RASTERIZER_DESC desc{};

        desc.cullMode = cull_mode;
        desc.fillMode = fill_mode;
        desc.scissorEnable = scissor_enable;
        desc.depthClipEnable = true;
        desc.multisampleEnable = true;

        REX::W32::HRESULT const hr = device->CreateRasterizerState(&desc, result);
        if (!REX::W32::SUCCESS(hr) || !*result)
        {
            logger::error("CommonStates: CreateRasterizerState failed ({:X})", static_cast<unsigned int>(hr));
            return false;
        }
        return true;
    }

    bool create_sampler_state(REX::W32::ID3D11Device* device, REX::W32::D3D11_FILTER filter, REX::W32::D3D11_TEXTURE_ADDRESS_MODE address_mode,
        REX::W32::ID3D11SamplerState** result)
    {
        REX::W32::D3D11_SAMPLER_DESC desc{};

        desc.filter = filter;

        desc.addressU = address_mode;
        desc.addressV = address_mode;
        desc.addressW = address_mode;

        desc.maxAnisotropy = device->GetFeatureLevel() > REX::W32::D3D_FEATURE_LEVEL_9_1 ? REX::W32::D3D11_MAX_MAXANISOTROPY : 2u;

        desc.maxLOD = FLT_MAX;
        desc.comparisonFunc = REX::W32::D3D11_COMPARISON_NEVER;

        REX::W32::HRESULT const hr = device->CreateSamplerState(&desc, result);
        if (!REX::W32::SUCCESS(hr) || !*result)
        {
            logger::error("CommonStates: CreateSamplerState failed ({:X})", static_cast<unsigned int>(hr));
            return false;
        }
        return true;
    }
}

CommonStates::CommonStates(REX::W32::ID3D11Device* device) :
    m_opaque(nullptr),
    m_alpha_blend(nullptr),
    m_additive(nullptr),
    m_non_premultiplied(nullptr),
    m_depth_default(nullptr),
    m_depth_read(nullptr),
    m_depth_none(nullptr),
    m_cull_none(nullptr),
    m_cull_none_scissor(nullptr),
    m_cull_clockwise(nullptr),
    m_cull_counter_clockwise(nullptr),
    m_wireframe(nullptr),
    m_point_wrap(nullptr),
    m_point_clamp(nullptr),
    m_linear_wrap(nullptr),
    m_linear_clamp(nullptr),
    m_anisotropic_wrap(nullptr),
    m_anisotropic_clamp(nullptr),
    m_valid(true)
{
    if (!device)
    {
        logger::error("CommonStates: null D3D11 device");
        m_valid = false;
        return;
    }

    m_valid = create_blend_state(device, REX::W32::D3D11_BLEND_ONE, REX::W32::D3D11_BLEND_ZERO, &m_opaque) &&
              create_blend_state(device, REX::W32::D3D11_BLEND_ONE, REX::W32::D3D11_BLEND_INV_SRC_ALPHA, &m_alpha_blend) &&
              create_blend_state(device, REX::W32::D3D11_BLEND_SRC_ALPHA, REX::W32::D3D11_BLEND_ONE, &m_additive) &&
              create_blend_state(device, REX::W32::D3D11_BLEND_SRC_ALPHA, REX::W32::D3D11_BLEND_INV_SRC_ALPHA, &m_non_premultiplied) &&
              create_depth_stencil_state(device, true, true, &m_depth_default) &&
              create_depth_stencil_state(device, true, false, &m_depth_read) &&
              create_depth_stencil_state(device, false, false, &m_depth_none) &&
              create_rasterizer_state(device, REX::W32::D3D11_CULL_NONE, REX::W32::D3D11_FILL_SOLID, false, &m_cull_none) &&
              create_rasterizer_state(device, REX::W32::D3D11_CULL_NONE, REX::W32::D3D11_FILL_SOLID, true, &m_cull_none_scissor) &&
              create_rasterizer_state(device, REX::W32::D3D11_CULL_FRONT, REX::W32::D3D11_FILL_SOLID, false, &m_cull_clockwise) &&
              create_rasterizer_state(device, REX::W32::D3D11_CULL_BACK, REX::W32::D3D11_FILL_SOLID, false, &m_cull_counter_clockwise) &&
              create_rasterizer_state(device, REX::W32::D3D11_CULL_NONE, REX::W32::D3D11_FILL_WIREFRAME,false, &m_wireframe) &&
              create_sampler_state(device, REX::W32::D3D11_FILTER_MIN_MAG_MIP_POINT, REX::W32::D3D11_TEXTURE_ADDRESS_WRAP, &m_point_wrap) &&
              create_sampler_state(device, REX::W32::D3D11_FILTER_MIN_MAG_MIP_POINT, REX::W32::D3D11_TEXTURE_ADDRESS_CLAMP, &m_point_clamp) &&
              create_sampler_state(device, REX::W32::D3D11_FILTER_MIN_MAG_MIP_LINEAR, REX::W32::D3D11_TEXTURE_ADDRESS_WRAP, &m_linear_wrap) &&
              create_sampler_state(device, REX::W32::D3D11_FILTER_MIN_MAG_MIP_LINEAR, REX::W32::D3D11_TEXTURE_ADDRESS_CLAMP, &m_linear_clamp) &&
              create_sampler_state(device, REX::W32::D3D11_FILTER_ANISOTROPIC, REX::W32::D3D11_TEXTURE_ADDRESS_WRAP, &m_anisotropic_wrap) &&
              create_sampler_state(device, REX::W32::D3D11_FILTER_ANISOTROPIC, REX::W32::D3D11_TEXTURE_ADDRESS_CLAMP, &m_anisotropic_clamp);
}

CommonStates::~CommonStates()
{
    if (m_anisotropic_clamp)
    {
        m_anisotropic_clamp->Release();
        m_anisotropic_clamp = nullptr;
    }
    if (m_anisotropic_wrap)
    {
        m_anisotropic_wrap->Release();
        m_anisotropic_wrap = nullptr;
    }
    if (m_linear_clamp)
    {
        m_linear_clamp->Release();
        m_linear_clamp = nullptr;
    }
    if (m_linear_wrap)
    {
        m_linear_wrap->Release();
        m_linear_wrap = nullptr;
    }
    if (m_point_clamp)
    {
        m_point_clamp->Release();
        m_point_clamp = nullptr;
    }
    if (m_point_wrap)
    {
        m_point_wrap->Release();
        m_point_wrap = nullptr;
    }
    if (m_wireframe)
    {
        m_wireframe->Release();
        m_wireframe = nullptr;
    }
    if (m_cull_counter_clockwise)
    {
        m_cull_counter_clockwise->Release();
        m_cull_counter_clockwise = nullptr;
    }
    if (m_cull_clockwise)
    {
        m_cull_clockwise->Release();
        m_cull_clockwise = nullptr;
    }
    if (m_cull_none)
    {
        m_cull_none->Release();
        m_cull_none = nullptr;
    }
    if (m_depth_none)
    {
        m_depth_none->Release();
        m_depth_none = nullptr;
    }
    if (m_depth_read)
    {
        m_depth_read->Release();
        m_depth_read = nullptr;
    }
    if (m_depth_default)
    {
        m_depth_default->Release();
        m_depth_default = nullptr;
    }
    if (m_non_premultiplied)
    {
        m_non_premultiplied->Release();
        m_non_premultiplied = nullptr;
    }
    if (m_additive)
    {
        m_additive->Release();
        m_additive = nullptr;
    }
    if (m_alpha_blend)
    {
        m_alpha_blend->Release();
        m_alpha_blend = nullptr;
    }
    if (m_opaque)
    {
        m_opaque->Release();
        m_opaque = nullptr;
    }
}

bool CommonStates::valid() const
{
    return m_valid;
}

REX::W32::ID3D11BlendState* CommonStates::opaque() const
{
    return m_opaque;
}

REX::W32::ID3D11BlendState* CommonStates::alpha_blend() const
{
    return m_alpha_blend;
}

REX::W32::ID3D11BlendState* CommonStates::additive() const
{
    return m_additive;
}

REX::W32::ID3D11BlendState* CommonStates::non_premultiplied() const
{
    return m_non_premultiplied;
}

REX::W32::ID3D11DepthStencilState* CommonStates::depth_default() const
{
    return m_depth_default;
}

REX::W32::ID3D11DepthStencilState* CommonStates::depth_read() const
{
    return m_depth_read;
}

REX::W32::ID3D11DepthStencilState* CommonStates::depth_none() const
{
    return m_depth_none;
}

REX::W32::ID3D11RasterizerState* CommonStates::cull_none() const
{
    return m_cull_none;
}

REX::W32::ID3D11RasterizerState* CommonStates::cull_none_scissor() const
{
    return m_cull_none_scissor;
}

REX::W32::ID3D11RasterizerState* CommonStates::cull_clockwise() const
{
    return m_cull_clockwise;
}

REX::W32::ID3D11RasterizerState* CommonStates::cull_counter_clockwise() const
{
    return m_cull_counter_clockwise;
}

REX::W32::ID3D11RasterizerState* CommonStates::wireframe() const
{
    return m_wireframe;
}

REX::W32::ID3D11SamplerState* CommonStates::point_wrap() const
{
    return m_point_wrap;
}

REX::W32::ID3D11SamplerState* CommonStates::point_clamp() const
{
    return m_point_clamp;
}

REX::W32::ID3D11SamplerState* CommonStates::linear_wrap() const
{
    return m_linear_wrap;
}

REX::W32::ID3D11SamplerState* CommonStates::linear_clamp() const
{
    return m_linear_clamp;
}

REX::W32::ID3D11SamplerState* CommonStates::anisotropic_wrap() const
{
    return m_anisotropic_wrap;
}

REX::W32::ID3D11SamplerState* CommonStates::anisotropic_clamp() const
{
    return m_anisotropic_clamp;
}

PLUGIN_NAMESPACE_END
