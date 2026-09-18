//
// Created by AmazingBuff on 2026/9/18.
//

#pragma once

#include <REX/W32/D3D11.h>

PLUGIN_NAMESPACE_BEGIN

// Local, REX::W32-typed replacement for DirectXTK's CommonStates. Every state
// description mirrors the pinned DirectXTK source tree (vcpkg directxtk may2026,
// Src/CommonStates.cpp, MIT license) byte-faithfully. Deliberate deviation from
// DirectXTK: the constructor never throws; a failed device call logs, leaves the
// affected state null and clears valid().
class CommonStates
{
public:
    explicit CommonStates(REX::W32::ID3D11Device* device);
    ~CommonStates();

    CommonStates(CommonStates const&) = delete;
    CommonStates(CommonStates&&) = delete;
    CommonStates& operator=(CommonStates const&) = delete;
    CommonStates& operator=(CommonStates&&) = delete;

    [[nodiscard]] bool valid() const;

    [[nodiscard]] REX::W32::ID3D11BlendState* opaque() const;
    [[nodiscard]] REX::W32::ID3D11BlendState* alpha_blend() const;
    [[nodiscard]] REX::W32::ID3D11BlendState* additive() const;
    [[nodiscard]] REX::W32::ID3D11BlendState* non_premultiplied() const;

    [[nodiscard]] REX::W32::ID3D11DepthStencilState* depth_default() const;
    [[nodiscard]] REX::W32::ID3D11DepthStencilState* depth_read() const;
    [[nodiscard]] REX::W32::ID3D11DepthStencilState* depth_none() const;

    [[nodiscard]] REX::W32::ID3D11RasterizerState* cull_none() const;
    [[nodiscard]] REX::W32::ID3D11RasterizerState* cull_none_scissor() const;
    [[nodiscard]] REX::W32::ID3D11RasterizerState* cull_clockwise() const;
    [[nodiscard]] REX::W32::ID3D11RasterizerState* cull_counter_clockwise() const;
    [[nodiscard]] REX::W32::ID3D11RasterizerState* wireframe() const;

    [[nodiscard]] REX::W32::ID3D11SamplerState* point_wrap() const;
    [[nodiscard]] REX::W32::ID3D11SamplerState* point_clamp() const;
    [[nodiscard]] REX::W32::ID3D11SamplerState* linear_wrap() const;
    [[nodiscard]] REX::W32::ID3D11SamplerState* linear_clamp() const;
    [[nodiscard]] REX::W32::ID3D11SamplerState* anisotropic_wrap() const;
    [[nodiscard]] REX::W32::ID3D11SamplerState* anisotropic_clamp() const;

private:
    REX::W32::ID3D11BlendState* m_opaque;
    REX::W32::ID3D11BlendState* m_alpha_blend;
    REX::W32::ID3D11BlendState* m_additive;
    REX::W32::ID3D11BlendState* m_non_premultiplied;

    REX::W32::ID3D11DepthStencilState* m_depth_default;
    REX::W32::ID3D11DepthStencilState* m_depth_read;
    REX::W32::ID3D11DepthStencilState* m_depth_none;

    REX::W32::ID3D11RasterizerState* m_cull_none;
    REX::W32::ID3D11RasterizerState* m_cull_none_scissor;
    REX::W32::ID3D11RasterizerState* m_cull_clockwise;
    REX::W32::ID3D11RasterizerState* m_cull_counter_clockwise;
    REX::W32::ID3D11RasterizerState* m_wireframe;

    REX::W32::ID3D11SamplerState* m_point_wrap;
    REX::W32::ID3D11SamplerState* m_point_clamp;
    REX::W32::ID3D11SamplerState* m_linear_wrap;
    REX::W32::ID3D11SamplerState* m_linear_clamp;
    REX::W32::ID3D11SamplerState* m_anisotropic_wrap;
    REX::W32::ID3D11SamplerState* m_anisotropic_clamp;

    bool m_valid;
};

PLUGIN_NAMESPACE_END
