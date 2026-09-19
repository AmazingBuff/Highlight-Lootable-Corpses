//
// Created by AmazingBuff on 2026/9/19.
//

#include "shader_manager.h"

#include "render/shader_sources.h"
#include "render/dx11/d3d11_util.h"

PLUGIN_NAMESPACE_BEGIN

namespace
{
    // Compile + create one entry; on failure the outputs stay null and the caller aborts.
    // blob is optional: only the input layouts need the VS bytecode, so the VS without a layout
    // (fullscreen composite) passes nullptr and the blob is released right after creation.
    bool create_vertex_shader(
        REX::W32::ID3D11Device* device, char const* source, char const* entry, char const* name,
        REX::W32::ID3D11VertexShader** shader, REX::W32::ID3DBlob** blob)
    {
        REX::W32::ID3DBlob* compiled = compile_shader(source, entry, "vs_5_0", name, "shader manager");
        if (!compiled)
            return false;

        device->CreateVertexShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), nullptr, shader);
        if (blob)
            *blob = compiled;
        else
            compiled->Release();

        return *shader != nullptr;
    }

    bool create_pixel_shader(
        REX::W32::ID3D11Device* device, char const* source, char const* entry, char const* name,
        REX::W32::ID3D11PixelShader** shader)
    {
        REX::W32::ID3DBlob* blob = compile_shader(source, entry, "ps_5_0", name, "shader manager");
        if (!blob)
            return false;
        device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader);
        blob->Release();
        return *shader != nullptr;
    }
}

ShaderManager& ShaderManager::instance()
{
    static ShaderManager s_instance;
    return s_instance;
}

ShaderManager::ShaderManager() :
    m_icon_vs(nullptr),
    m_icon_ps(nullptr),
    m_icon_vs_blob(nullptr),
    m_mask_static_vs(nullptr),
    m_mask_skinned_vs(nullptr),
    m_mask_ps(nullptr),
    m_mask_static_vs_blob(nullptr),
    m_mask_skinned_vs_blob(nullptr),
    m_fullscreen_vs(nullptr),
    m_silhouette_ps(nullptr),
    m_glow_horizontal_ps(nullptr),
    m_glow_vertical_ps(nullptr),
    m_ready(false) {}

ShaderManager::~ShaderManager()
{
    release();
}

bool ShaderManager::compile()
{
    if (!m_ready)
    {
        RE::BSGraphics::Renderer* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer)
        {
            logger::info("Shader precompile skipped, renderer not available at data-loaded");
            return false;
        }

        REX::W32::ID3D11Device* device = renderer->GetRuntimeData().forwarder;
        if (!device)
        {
            logger::info("Shader precompile skipped, D3D11 device not available at data-loaded");
            return false;
        }

        m_ready =
            create_vertex_shader(device, render_shaders::IconOverlay, "vs_main", "ui overlay", &m_icon_vs, &m_icon_vs_blob) &&
            create_pixel_shader(device, render_shaders::IconOverlay, "ps_main", "ui overlay", &m_icon_ps) &&

            create_vertex_shader(device, render_shaders::MaskGeometry, "vs_static_main", "outline mask static", &m_mask_static_vs, &m_mask_static_vs_blob) &&
            create_vertex_shader(device, render_shaders::MaskGeometry, "vs_skinned_main", "outline mask skinned", &m_mask_skinned_vs, &m_mask_skinned_vs_blob) &&
            create_pixel_shader(device, render_shaders::MaskGeometry, "ps_main", "outline mask", &m_mask_ps) &&

            create_vertex_shader(device, render_shaders::MaskComposite, "vs_main", "outline mask fullscreen", &m_fullscreen_vs, nullptr) &&
            create_pixel_shader(device, render_shaders::MaskComposite, "ps_silhouette_main", "outline mask silhouette", &m_silhouette_ps) &&

            create_pixel_shader(device, render_shaders::MaskGlow, "ps_glow_horizontal", "outline glow horizontal", &m_glow_horizontal_ps) &&
            create_pixel_shader(device, render_shaders::MaskGlow, "ps_glow_vertical", "outline glow vertical", &m_glow_vertical_ps);

        if (!m_ready)
        {
            release();
            logger::error("Shader compilation failed, overlay rendering disabled");
            return false;
        }

        logger::info("All overlay shaders compiled");
    }

    return true;
}

void ShaderManager::release()
{
    auto const release_ptr = [](auto& com_ptr) {
        if (com_ptr)
        {
            com_ptr->Release();
            com_ptr = nullptr;
        }
    };

    release_ptr(m_icon_vs);
    release_ptr(m_icon_ps);
    release_ptr(m_mask_static_vs);
    release_ptr(m_mask_skinned_vs);
    release_ptr(m_mask_ps);
    release_ptr(m_fullscreen_vs);
    release_ptr(m_silhouette_ps);
    release_ptr(m_glow_horizontal_ps);
    release_ptr(m_glow_vertical_ps);
    release_ptr(m_icon_vs_blob);
    release_ptr(m_mask_static_vs_blob);
    release_ptr(m_mask_skinned_vs_blob);
}

PLUGIN_NAMESPACE_END
