//
// Created by AmazingBuff on 2026/9/19.
//

#pragma once

#include <REX/W32/D3D11.h>

PLUGIN_NAMESPACE_BEGIN

// One-shot shader compilation for every pass of the plugin. init(device) runs once when the
// D3D11 device is first available (renderer init); afterwards every pass's init only picks up
// the ready-made shader objects. The VS blobs stay alive here because CreateInputLayout needs
// the VS bytecode, which is not retrievable from an ID3D11VertexShader.
class ShaderManager
{
public:
    static ShaderManager& instance();

    ShaderManager(ShaderManager const&) = delete;
    ShaderManager& operator=(ShaderManager const&) = delete;

    // Compiles every embedded HLSL and creates the shader objects; returns false (and logs)
    // if any entry fails. Idempotent: a repeated call returns the previous verdict.
    [[nodiscard]] bool compile();

    // Icon overlay
    [[nodiscard]] REX::W32::ID3D11VertexShader* icon_vs() const noexcept { return m_icon_vs; }
    [[nodiscard]] REX::W32::ID3D11PixelShader* icon_ps() const noexcept { return m_icon_ps; }
    [[nodiscard]] REX::W32::ID3DBlob* icon_vs_blob() const noexcept { return m_icon_vs_blob; }

    // Mask geometry
    [[nodiscard]] REX::W32::ID3D11VertexShader* mask_static_vs() const noexcept { return m_mask_static_vs; }
    [[nodiscard]] REX::W32::ID3D11VertexShader* mask_skinned_vs() const noexcept { return m_mask_skinned_vs; }
    [[nodiscard]] REX::W32::ID3D11PixelShader* mask_ps() const noexcept { return m_mask_ps; }
    [[nodiscard]] REX::W32::ID3DBlob* mask_static_vs_blob() const noexcept { return m_mask_static_vs_blob; }
    [[nodiscard]] REX::W32::ID3DBlob* mask_skinned_vs_blob() const noexcept { return m_mask_skinned_vs_blob; }

    // Mask fullscreen composite
    [[nodiscard]] REX::W32::ID3D11VertexShader* fullscreen_vs() const noexcept { return m_fullscreen_vs; }
    [[nodiscard]] REX::W32::ID3D11PixelShader* silhouette_ps() const noexcept { return m_silhouette_ps; }

    // Mask glow
    [[nodiscard]] REX::W32::ID3D11PixelShader* glow_horizontal_ps() const noexcept { return m_glow_horizontal_ps; }
    [[nodiscard]] REX::W32::ID3D11PixelShader* glow_vertical_ps() const noexcept { return m_glow_vertical_ps; }

private:
    ShaderManager();
    ~ShaderManager();

    void release();

    REX::W32::ID3D11VertexShader* m_icon_vs;
    REX::W32::ID3D11PixelShader* m_icon_ps;
    REX::W32::ID3DBlob* m_icon_vs_blob;

    REX::W32::ID3D11VertexShader* m_mask_static_vs;
    REX::W32::ID3D11VertexShader* m_mask_skinned_vs;
    REX::W32::ID3D11PixelShader* m_mask_ps;
    REX::W32::ID3DBlob* m_mask_static_vs_blob;
    REX::W32::ID3DBlob* m_mask_skinned_vs_blob;

    REX::W32::ID3D11VertexShader* m_fullscreen_vs;
    REX::W32::ID3D11PixelShader* m_silhouette_ps;

    REX::W32::ID3D11PixelShader* m_glow_horizontal_ps;
    REX::W32::ID3D11PixelShader* m_glow_vertical_ps;

    bool m_ready;
};

PLUGIN_NAMESPACE_END
