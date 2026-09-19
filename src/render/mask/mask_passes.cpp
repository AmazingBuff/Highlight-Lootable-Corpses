//
// Created by AmazingBuff on 2026/9/13.
//

#include "mask_passes.h"

#include "mask_geometry.h"

#include "render/shader_manager.h"
#include "render/shader_sources.h"

#include "render/dx11/common_states.h"

MASK_NAMESPACE_BEGIN

namespace
{
    struct PerDrawCBData
    {
        DirectX::XMFLOAT4X4 mvp;
        uint32_t object_id;
        float pad[3];
    };

    struct GlowCBData
    {
        DirectX::XMFLOAT4 narrow[Glow::Kernel_Slot_Count];
        DirectX::XMFLOAT4 wide[Glow::Kernel_Slot_Count];
        int radius;
        int width;
        int height;
        uint32_t object_id;
        REX::W32::D3D11_RECT rect;
    };
}

// ---------------------------------------------------------------------------
// MaskRenderTarget
// ---------------------------------------------------------------------------

RenderTarget::RenderTarget(REX::W32::DXGI_FORMAT color_format, REX::W32::DXGI_FORMAT depth_format) :
    m_color_format(color_format),
    m_depth_format(depth_format),
    m_ref_device(nullptr),
    m_texture(nullptr),
    m_depth_texture(nullptr),
    m_dsv(nullptr),
    m_rtv(nullptr),
    m_srv(nullptr),
    m_width(0),
    m_height(0) {}

RenderTarget::~RenderTarget()
{
    release();
}

bool RenderTarget::matches(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height) const
{
    const bool unchanged = m_ref_device == device && m_width == width && m_height == height && m_srv && m_dsv;
    return unchanged;
}

void RenderTarget::release()
{
    if (m_dsv)
    {
        m_dsv->Release();
        m_dsv = nullptr;
    }
    if (m_depth_texture)
    {
        m_depth_texture->Release();
        m_depth_texture = nullptr;
    }
    if (m_srv)
    {
        m_srv->Release();
        m_srv = nullptr;
    }
    if (m_rtv)
    {
        m_rtv->Release();
        m_rtv = nullptr;
    }
    if (m_texture)
    {
        m_texture->Release();
        m_texture = nullptr;
    }
    m_width = 0;
    m_height = 0;
    m_ref_device = nullptr;
}

bool RenderTarget::init(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height)
{
    REX::W32::D3D11_TEXTURE2D_DESC td{};
    td.width = width;
    td.height = height;
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = m_color_format;
    td.sampleDesc.count = 1;
    td.usage = REX::W32::D3D11_USAGE_DEFAULT;
    td.bindFlags = REX::W32::D3D11_BIND_RENDER_TARGET | REX::W32::D3D11_BIND_SHADER_RESOURCE;

    REX::W32::HRESULT const tex_hr = device->CreateTexture2D(&td, nullptr, &m_texture);
    if (!REX::W32::SUCCESS(tex_hr) || !m_texture)
    {
        logger::error("Mask overlay: failed to create mask texture ({:X})", static_cast<unsigned int>(tex_hr));
        release();
        return false;
    }
    REX::W32::HRESULT const rtv_hr = device->CreateRenderTargetView(m_texture, nullptr, &m_rtv);
    REX::W32::HRESULT const srv_hr = device->CreateShaderResourceView(m_texture, nullptr, &m_srv);
    if (!REX::W32::SUCCESS(rtv_hr) || !m_rtv || !REX::W32::SUCCESS(srv_hr) || !m_srv)
    {
        logger::error("Mask overlay: failed to create mask views (rtv={:X}, srv={:X})", static_cast<unsigned int>(rtv_hr), static_cast<unsigned int>(srv_hr));
        release();
        return false;
    }

    if (m_depth_format != REX::W32::DXGI_FORMAT_UNKNOWN)
    {
        td.format = m_depth_format;
        td.bindFlags = REX::W32::D3D11_BIND_DEPTH_STENCIL;
        REX::W32::HRESULT const depth_hr = device->CreateTexture2D(&td, nullptr, &m_depth_texture);
        if (!REX::W32::SUCCESS(depth_hr) || !REX::W32::SUCCESS(device->CreateDepthStencilView(m_depth_texture, nullptr, &m_dsv))) {
          logger::error("Mask overlay: failed to create private depth target");
          release();
          return false;
        }
    }

    m_ref_device = device;
    m_width = width;
    m_height = height;
    return true;
}

// ---------------------------------------------------------------------------
// MaskGeometryPass
// ---------------------------------------------------------------------------

MaskGeometryPass::MaskGeometryPass() :
    m_ref_vs_static(nullptr),
    m_ref_vs_skinned(nullptr),
    m_ref_ps_mask(nullptr),
    m_per_draw_cb(nullptr),
    m_palette_cb(nullptr),
    m_depth_nearest(nullptr) {}

MaskGeometryPass::~MaskGeometryPass()
{
    release();
}

// ---------------------------------------------------------------------------
// MaskGeometryPass InputLayout cache: keyed by (skinned, precision, attribute offsets, stride) -
// the attribute offsets come from each mesh's vertexDesc and creating a device object per mesh is
// not acceptable, so the cache deduplicates (corpse meshes come in very few layout varieties).
// ---------------------------------------------------------------------------
REX::W32::ID3D11InputLayout* MaskGeometryPass::get_layout(
    REX::W32::ID3D11Device* device,
    REX::W32::ID3DBlob* blob,
    bool skinned,
    RE::BSGraphics::VertexDesc const& desc,
    uint32_t stride,
    REX::W32::DXGI_FORMAT position_format,
    uint32_t position_offset,
    MaskSkinLayout const* skin_layout)
{
    // Position format/offset: the static path passes a calibration result (UNKNOWN means derive
    // from desc); the skinned path passes an attribute-offset spacing result (never UNKNOWN).
    REX::W32::DXGI_FORMAT const resolved_format =
        (position_format == REX::W32::DXGI_FORMAT_UNKNOWN) ?
        (desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) ?
        REX::W32::DXGI_FORMAT_R32G32B32_FLOAT :
        REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT) :
        position_format;
    uint32_t const resolved_offset =
        (position_format == REX::W32::DXGI_FORMAT_UNKNOWN) ?
        desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION) :
        position_offset;
    // The skinning weight/index layout comes from a calibration result; the static path has none (nullptr).
    MaskSkinLayout const skin_layout_ref = skin_layout ? *skin_layout : MaskSkinLayout{};
    LayoutKey key{
        .skinned = skinned,
        .full_precision = desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC),
        .position_format = static_cast<uint32_t>(resolved_format),
        .position_offset = resolved_offset,
        .skinning_offset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING),
        .stride = stride,
        .weight_format = static_cast<uint32_t>(skin_layout_ref.weight_format),
        .weight_offset = skin_layout_ref.weight_offset,
        .index_format = static_cast<uint32_t>(skin_layout_ref.index_format),
        .index_offset = skin_layout_ref.index_offset,
    };

    const auto layout_it = m_layout_cache.find(key);
    if (layout_it != m_layout_cache.end())
        return layout_it->second;

    REX::W32::D3D11_INPUT_ELEMENT_DESC elements[3]{};
    uint32_t count = 0;
    elements[count++] = {
        .semanticName = "POSITION",
        .semanticIndex = 0,
        .format = resolved_format,
        .inputSlot = 0,
        .alignedByteOffset = resolved_offset,
        .inputSlotClass = REX::W32::D3D11_INPUT_PER_VERTEX_DATA,
        .instanceDataStepRate = 0
    };
    if (skinned)
    {
        // The layout inside the SKINNING block comes from the self-calibration (the weight/index
        // formats and byte offsets); the semantic name order (BLENDWEIGHT / BLENDINDICES) matches
        // the skinned VS.
        elements[count++] = {
            .semanticName = "BLENDWEIGHT",
            .semanticIndex = 0,
            .format = skin_layout_ref.weight_format,
            .inputSlot = 0,
            .alignedByteOffset = skin_layout_ref.weight_offset,
            .inputSlotClass = REX::W32::D3D11_INPUT_PER_VERTEX_DATA,
            .instanceDataStepRate = 0
        };
        elements[count++] = {
            .semanticName = "BLENDINDICES",
            .semanticIndex = 0,
            .format = skin_layout_ref.index_format,
            .inputSlot = 0,
            .alignedByteOffset = skin_layout_ref.index_offset,
            .inputSlotClass = REX::W32::D3D11_INPUT_PER_VERTEX_DATA,
            .instanceDataStepRate = 0
        };
    }

    REX::W32::ID3D11InputLayout* layout = nullptr;
    REX::W32::HRESULT const hr = device->CreateInputLayout(elements, count, blob->GetBufferPointer(), blob->GetBufferSize(), &layout);
    if (!REX::W32::SUCCESS(hr) || !layout)
    {
        logger::error("Mask overlay: CreateInputLayout failed ({:X}), affected meshes skipped", static_cast<unsigned int>(hr));
        return nullptr;
    }
    m_layout_cache.emplace(key, layout);
    return layout;
}

void MaskGeometryPass::release_layouts()
{
    for (REX::W32::ID3D11InputLayout* const val : m_layout_cache | std::views::values)
    {
        if (val)
            val->Release();
    }
    m_layout_cache.clear();
}

bool MaskGeometryPass::init(REX::W32::ID3D11Device* device)
{
    const bool ready = create_pipeline(device);
    if (!ready)
    {
        release();
        logger::error("Mask overlay pipeline creation failed, mask rendering disabled");
    }
    return ready;
}

bool MaskGeometryPass::create_pipeline(REX::W32::ID3D11Device* device)
{
    // The shaders are precompiled by the ShaderManager; only the device objects of this pass are created here.
    ShaderManager& shaders = ShaderManager::instance();
    m_ref_vs_static = shaders.mask_static_vs();
    m_ref_vs_skinned = shaders.mask_skinned_vs();
    m_ref_ps_mask = shaders.mask_ps();

    REX::W32::D3D11_BUFFER_DESC cb{};
    cb.usage = REX::W32::D3D11_USAGE_DYNAMIC;
    cb.bindFlags = REX::W32::D3D11_BIND_CONSTANT_BUFFER;
    cb.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
    cb.byteWidth = sizeof(PerDrawCBData);
    device->CreateBuffer(&cb, nullptr, &m_per_draw_cb);
    cb.byteWidth = static_cast<uint32_t>(Palette_CB_Bytes);
    device->CreateBuffer(&cb, nullptr, &m_palette_cb);

    // Reverse-Z nearest test (GREATER): no CommonStates equivalent - the pipeline is reverse-Z.
    REX::W32::D3D11_DEPTH_STENCIL_DESC depth{};
    depth.depthEnable = true;
    depth.depthWriteMask = REX::W32::D3D11_DEPTH_WRITE_MASK_ALL;
    depth.depthFunc = REX::W32::D3D11_COMPARISON_GREATER;
    device->CreateDepthStencilState(&depth, &m_depth_nearest);

    bool const ready = m_ref_vs_static && m_ref_vs_skinned && m_ref_ps_mask &&
                       m_per_draw_cb && m_palette_cb && m_depth_nearest;
    if (!ready)
        return false;

    logger::info("Mask overlay pipeline ready (palette {} bones/draw)", Max_Palette_Bones);
    return true;
}

void MaskGeometryPass::release()
{
    // The shader objects and blobs are owned by the ShaderManager - not released here.
    if (m_depth_nearest)
    {
        m_depth_nearest->Release();
        m_depth_nearest = nullptr;
    }
    if (m_palette_cb)
    {
        m_palette_cb->Release();
        m_palette_cb = nullptr;
    }
    if (m_per_draw_cb)
    {
        m_per_draw_cb->Release();
        m_per_draw_cb = nullptr;
    }
    m_ref_ps_mask = nullptr;
    m_ref_vs_skinned = nullptr;
    m_ref_vs_static = nullptr;
    release_layouts();
}

void MaskGeometryPass::draw(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context, DirectX::XMFLOAT4X4 const& view_proj, std::span<MaskDraw const> draws)
{
    for (MaskDraw const& draw : draws)
    {
        if (!draw.vertex_buffer || !draw.index_buffer || draw.index_count == 0 || draw.vertex_stride == 0)
            continue;

        // Skinned draws pass the calibrated layout; static draws pass nullptr (behaviour exactly as before)
        ShaderManager& shaders = ShaderManager::instance();
        REX::W32::ID3D11InputLayout* layout = get_layout(device, draw.skinned ? shaders.mask_skinned_vs_blob() : shaders.mask_static_vs_blob(),
            draw.skinned, draw.vertex_desc, draw.vertex_stride,
            draw.position_format, draw.position_offset, draw.skinned ? &draw.skin_layout : nullptr);
        if (!layout)
            continue;

        REX::W32::ID3D11VertexShader* vs = draw.skinned ? m_ref_vs_skinned : m_ref_vs_static;
        if (!vs || !m_ref_ps_mask)
            continue;

        // b0: static = ViewProj × world transform; palette skinning = ViewProj (the world transform lives in the palette)
        DirectX::XMFLOAT4X4 per_draw = view_proj;
        if (!draw.skinned)
        {
            RE::NiTransform const& node_transform = draw.node->world;
            DirectX::XMFLOAT4X4 node_world{};
            DirectX::XMStoreFloat4x4(&node_world, DirectX::XMMatrixIdentity());
            for (int row = 0; row < 3; ++row)
            {
                for (int col = 0; col < 3; ++col)
                    node_world.m[row][col] = node_transform.rotate.entry[row][col] * node_transform.scale;
                node_world.m[row][3] = node_transform.translate[row];
            }
            DirectX::XMStoreFloat4x4(&per_draw, DirectX::XMMatrixMultiply(DirectX::XMLoadFloat4x4(&view_proj), DirectX::XMLoadFloat4x4(&node_world)));
        }

        PerDrawCBData cb_data{};
        cb_data.mvp = per_draw;

        cb_data.object_id = draw.target_index + 1;
        REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!REX::W32::SUCCESS(context->Map(m_per_draw_cb, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;
        std::memcpy(mapped.data, &cb_data, sizeof(PerDrawCBData));
        context->Unmap(m_per_draw_cb, 0);
        context->VSSetConstantBuffers(0, 1, &m_per_draw_cb);

        if (draw.skinned)
        {
            RE::NiSkinInstance* skin = draw.skin.get();

            // Palette: palette[i] = the 4x4 expansion of boneWorld[i] · the 4x4 expansion of
            // skinToBone(i) (the multiplication order keeps boneWorld first).
            // Empirically verified consumption convention (world variant): consuming a NiTransform
            // as is (expanded, not transposed) is the engine semantics. The engine's skinning
            // composition is skin→bone→world (StB first, then BW), whose column-vector matrix is
            // BW_col·StB_col - consistent with the column form of the engine's row-vector notation
            // v·StB·BW, namely (StB·BW)ᵀ = BWᵀ·StBᵀ (M_col(X) = X stored as is), so the
            // multiplication order keeps boneWorld first.
            // The palette is built in **global bone index space** (a vertex index is a subscript
            // into the skin bone array), with P = min(skinData bone count, numMatrices) as its
            // valid length; part.bones is not used (it is partition-local). Unused slots are filled
            // with a replica of palette[P-1] (to prevent reading undefined content out of bounds)
            // and the whole block of Max_Palette_Bones matrices is uploaded.
            uint32_t const palette_count = palette_slot_count(skin);
            if (palette_count == 0 || palette_count > Max_Palette_Bones)
            {
                logger::warn("{}", fmt::format("Mask overlay: skip skinned draw [palette slot count out of range] node=\"{}\" {}",
                    draw.node ? draw.node->name.c_str() : "?",
                    fmt::format("partition={} P={} budget={}", draw.partition, palette_count, Max_Palette_Bones)));
                continue;
            }

            DirectX::XMFLOAT4X4 palette[Max_Palette_Bones];
            bool palette_ok = true;
            for (uint32_t i = 0; i < palette_count; ++i)
            {
                // i < numMatrices and i < GetBoneCount() are guaranteed by the definition of P (read stays in bounds)
                if (!skin->boneWorldTransforms[i])
                {
                    palette_ok = false;
                    break;
                }
                RE::NiTransform const& bone_world_transform = *skin->boneWorldTransforms[i];
                DirectX::XMFLOAT4X4 bone_world{};
                DirectX::XMStoreFloat4x4(&bone_world, DirectX::XMMatrixIdentity());
                for (int row = 0; row < 3; ++row)
                {
                    for (int col = 0; col < 3; ++col)
                        bone_world.m[row][col] = bone_world_transform.rotate.entry[row][col] * bone_world_transform.scale;
                    bone_world.m[row][3] = bone_world_transform.translate[row];
                }

                RE::NiTransform const& skin_to_bone_transform = skin->skinData->GetBoneDataSkinToBone(i);
                DirectX::XMFLOAT4X4 skin_to_bone{};
                DirectX::XMStoreFloat4x4(&skin_to_bone, DirectX::XMMatrixIdentity());
                for (int row = 0; row < 3; ++row)
                {
                    for (int col = 0; col < 3; ++col)
                        skin_to_bone.m[row][col] = skin_to_bone_transform.rotate.entry[row][col] * skin_to_bone_transform.scale;
                    skin_to_bone.m[row][3] = skin_to_bone_transform.translate[row];
                }

                DirectX::XMStoreFloat4x4(&palette[i], DirectX::XMMatrixMultiply(DirectX::XMLoadFloat4x4(&bone_world), DirectX::XMLoadFloat4x4(&skin_to_bone)));
            }
            if (!palette_ok)
            {
                logger::warn("{}", fmt::format("Mask overlay: skip skinned draw [null bone world transform in palette range] node=\"{}\" {}",
                    draw.node ? draw.node->name.c_str() : "?",
                    fmt::format("partition={} P={}", draw.partition, palette_count)));
                continue;
            }
            for (size_t k = palette_count; k < Max_Palette_Bones; ++k)
                palette[k] = palette[palette_count - 1];

            if (!REX::W32::SUCCESS(context->Map(m_palette_cb, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return;
            std::memcpy(mapped.data, palette, Max_Palette_Bones * sizeof(DirectX::XMFLOAT4X4));
            context->Unmap(m_palette_cb, 0);
            context->VSSetConstantBuffers(1, 1, &m_palette_cb);
        }

        context->IASetInputLayout(layout);
        context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        uint32_t const stride = draw.vertex_stride;
        uint32_t const offset = 0;
        REX::W32::ID3D11Buffer* vb = draw.vertex_buffer;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        // BSTriShape::vertexCount and a partition's vertices are both uint16_t: indices are always 16-bit
        context->IASetIndexBuffer(draw.index_buffer, REX::W32::DXGI_FORMAT_R16_UINT, 0);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(m_ref_ps_mask, nullptr, 0);
        context->DrawIndexed(draw.index_count, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Full-screen consumption passes
// ---------------------------------------------------------------------------

FullscreenPass::FullscreenPass() :
    m_ref_vertex_shader(nullptr),
    m_style_buffer(nullptr),
    m_style_srv(nullptr),
    m_style_capacity(0) {}

FullscreenPass::~FullscreenPass()
{
    FullscreenPass::release();
}

bool FullscreenPass::init([[maybe_unused]] REX::W32::ID3D11Device* device)
{
    // The fullscreen vertex shader is precompiled and owned by the ShaderManager; nothing is
    // created on the device here anymore (the parameter stays for the shared pass interface).
    m_ref_vertex_shader = ShaderManager::instance().fullscreen_vs();

    // The premultiplied-alpha blend state comes from the shared CommonStates (alpha_blend) at draw time.
    return m_ref_vertex_shader != nullptr;
}

void FullscreenPass::release()
{
    m_style_capacity = 0;
    if (m_style_srv)
    {
        m_style_srv->Release();
        m_style_srv = nullptr;
    }
    if (m_style_buffer)
    {
        m_style_buffer->Release();
        m_style_buffer = nullptr;
    }
    // m_vertex_shader is owned by the ShaderManager - only the borrowing pointer is cleared.
    m_ref_vertex_shader = nullptr;
}

bool FullscreenPass::update_styles(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context, std::span<MaskTarget const> targets)
{
    if (targets.size() > m_style_capacity)
    {
        if (m_style_srv)
            m_style_srv->Release();
        if (m_style_buffer)
            m_style_buffer->Release();
        m_style_srv = nullptr;
        m_style_buffer = nullptr;
        m_style_capacity = 0;

        REX::W32::D3D11_BUFFER_DESC desc{};
        desc.byteWidth = static_cast<uint32_t>(targets.size() * sizeof(DirectX::XMFLOAT4));
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
        desc.miscFlags = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.structureByteStride = sizeof(DirectX::XMFLOAT4);
        if (!REX::W32::SUCCESS(device->CreateBuffer(&desc, nullptr, &m_style_buffer)))
            return false;

        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.viewDimension = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
        srv.buffer.numElements = static_cast<uint32_t>(targets.size());
        if (!REX::W32::SUCCESS(device->CreateShaderResourceView(m_style_buffer, &srv, &m_style_srv)))
            return false;
        m_style_capacity = targets.size();
    }

    std::vector<DirectX::XMFLOAT4> styles;
    styles.reserve(targets.size());
    for (const auto& [ref, color] : targets)
        styles.push_back(color);

    REX::W32::D3D11_BOX const box{ 0, 0, 0, static_cast<uint32_t>(styles.size() * sizeof(DirectX::XMFLOAT4)), 1, 1 };
    context->UpdateSubresource(m_style_buffer, 0, &box, styles.data(), 0, 0);

    return REX::W32::SUCCESS(device->GetDeviceRemovedReason());
}

// ---------------------------------------------------------------------------
// Silhouette consumption passes
// ---------------------------------------------------------------------------

SilhouettePass::SilhouettePass() : m_ref_silhouette_shader(nullptr) {  }

SilhouettePass::~SilhouettePass()
{
    release();
}

bool SilhouettePass::init(REX::W32::ID3D11Device* device)
{
    if (!FullscreenPass::init(device))
        return false;

    m_ref_silhouette_shader = ShaderManager::instance().silhouette_ps();

    const bool ready = m_ref_silhouette_shader;
    if (!ready)
    {
        release();
        logger::warn("Mask overlay silhouette pass unavailable, inner fill disabled (mask rendering stays active)");
    }
    return ready;
}

void SilhouettePass::release()
{
    // m_pixel_shader is owned by the ShaderManager - only the borrowing pointer is cleared.
    m_ref_silhouette_shader = nullptr;
    FullscreenPass::release();
}

bool SilhouettePass::draw(REX::W32::ID3D11DeviceContext* context, REX::W32::ID3D11ShaderResourceView* mask_srv) const
{
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    REX::W32::ID3D11Buffer* const no_vb = nullptr;
    uint32_t const zero = 0;
    context->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
    context->IASetIndexBuffer(nullptr, REX::W32::DXGI_FORMAT_UNKNOWN, 0);
    context->VSSetShader(m_ref_vertex_shader, nullptr, 0);

    REX::W32::ID3D11ShaderResourceView* const srvs[] = { mask_srv, m_style_srv };
    context->PSSetShader(m_ref_silhouette_shader, nullptr, 0);
    context->PSSetShaderResources(0, 2, srvs);
    context->Draw(3, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Outline consumption passes
// ---------------------------------------------------------------------------

OutlinePass::OutlinePass() :
    m_scratch_rt(REX::W32::DXGI_FORMAT_R16G16_FLOAT, REX::W32::DXGI_FORMAT_UNKNOWN),
    m_cb(nullptr),
    m_ref_horizontal_shader(nullptr),
    m_ref_vertical_shader(nullptr) {}

OutlinePass::~OutlinePass()
{
    release();
}

bool OutlinePass::init(REX::W32::ID3D11Device* device)
{
    if (!FullscreenPass::init(device))
        return false;

    // Both glow shaders are precompiled and owned by the ShaderManager.
    ShaderManager& shaders = ShaderManager::instance();
    m_ref_horizontal_shader = shaders.glow_horizontal_ps();
    m_ref_vertical_shader = shaders.glow_vertical_ps();

    REX::W32::D3D11_BUFFER_DESC glow_cb{};
    glow_cb.usage = REX::W32::D3D11_USAGE_DYNAMIC;
    glow_cb.bindFlags = REX::W32::D3D11_BIND_CONSTANT_BUFFER;
    glow_cb.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
    glow_cb.byteWidth = sizeof(GlowCBData);
    device->CreateBuffer(&glow_cb, nullptr, &m_cb);

    const bool ready = m_ref_horizontal_shader && m_ref_vertical_shader && m_cb;
    if (!ready)
    {
        release();
        logger::warn("Mask glow pass unavailable, corpse outline glow disabled (mask rendering stays active)");
    }
    return ready;
}

void OutlinePass::release()
{
    m_scratch_rt.release();
    if (m_cb)
    {
        m_cb->Release();
        m_cb = nullptr;
    }
    // Both glow shaders are owned by the ShaderManager - only the borrowing pointers are cleared.
    m_ref_horizontal_shader = nullptr;
    FullscreenPass::release();
}

bool OutlinePass::draw(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11RenderTargetView* target,
    REX::W32::ID3D11ShaderResourceView* mask_srv,
    REX::W32::D3D11_VIEWPORT const& viewport,
    uint32_t object_id,
    int thickness,
    ROI::Rect horizontal_rect,
    ROI::Rect vertical_rect,
    CommonStates const& states) const
{
    Glow::KernelProfile const profile = Glow::make_kernel_profile(thickness);
    GlowCBData cb_data{};
    std::memcpy(cb_data.narrow, profile.narrow.data(), sizeof(profile.narrow));
    std::memcpy(cb_data.wide, profile.wide.data(), sizeof(profile.wide));
    cb_data.radius = profile.radius;
    cb_data.width = static_cast<int>(viewport.width);
    cb_data.height = static_cast<int>(viewport.height);
    cb_data.object_id = object_id;
    cb_data.rect.x1 = horizontal_rect.left;
    cb_data.rect.y1 = horizontal_rect.top;
    cb_data.rect.x2 = horizontal_rect.right;
    cb_data.rect.y2 = horizontal_rect.bottom;

    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!REX::W32::SUCCESS(context->Map(m_cb, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.data, &cb_data, sizeof(cb_data));
    context->Unmap(m_cb, 0);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    REX::W32::ID3D11Buffer* const no_vb = nullptr;
    uint32_t const zero = 0;
    context->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
    context->IASetIndexBuffer(nullptr, REX::W32::DXGI_FORMAT_UNKNOWN, 0);
    context->VSSetShader(m_ref_vertex_shader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &m_cb);

    REX::W32::ID3D11RenderTargetView* const scratch_rtv = m_scratch_rt.rtv();
    context->OMSetRenderTargets(1, &scratch_rtv, nullptr);
    context->ClearRenderTargetView(scratch_rtv, Mask_Clear_Color);
    context->OMSetBlendState(states.opaque(), nullptr, 0xFFFFFFFF);

    REX::W32::D3D11_RECT const horizontal_scissor{horizontal_rect.left, horizontal_rect.top, horizontal_rect.right, horizontal_rect.bottom };
    context->RSSetScissorRects(1, &horizontal_scissor);

    REX::W32::ID3D11ShaderResourceView* const horizontal_srvs[] = { mask_srv };
    context->PSSetShader(m_ref_horizontal_shader, nullptr, 0);
    context->PSSetShaderResources(0, 1, horizontal_srvs);
    context->Draw(3, 0);

    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(states.non_premultiplied(), nullptr, 0xFFFFFFFF);

    REX::W32::D3D11_RECT const vertical_scissor{vertical_rect.left, vertical_rect.top, vertical_rect.right, vertical_rect.bottom };
    context->RSSetScissorRects(1, &vertical_scissor);

    REX::W32::ID3D11ShaderResourceView* const vertical_srvs[] = { mask_srv, m_scratch_rt.srv(), m_style_srv };
    context->PSSetShader(m_ref_vertical_shader, nullptr, 0);
    context->PSSetShaderResources(0, 3, vertical_srvs);
    context->Draw(3, 0);

    return true;
}

MASK_NAMESPACE_END
