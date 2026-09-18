//
// Created by AmazingBuff on 2026/9/13.
//

#include "mask_passes.h"

#include "mask_geometry.h"

#include "render/shader_sources.h"

#include "render/dx11/d3d11_util.h"

#include "Plugin.h"

#include <algorithm>
#include <cstring>

MASK_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------
// MaskGeometryPass InputLayout cache: keyed by (skinned, precision, attribute offsets, stride) -
// the attribute offsets come from each mesh's vertexDesc and creating a device object per mesh is
// not acceptable, so the cache deduplicates (corpse meshes come in very few layout varieties).
// ---------------------------------------------------------------------------
ID3D11InputLayout* MaskGeometryPass::get_layout(
    ID3D11Device* device, ID3DBlob* blob, bool skinned, RE::BSGraphics::VertexDesc const& desc, uint32_t stride,
    DXGI_FORMAT position_format, uint32_t position_offset, MaskSkinLayout const* skin_layout)
{
    // Position format/offset: the static path passes a calibration result (UNKNOWN means derive
    // from desc); the skinned path passes an attribute-offset spacing result (never UNKNOWN).
    DXGI_FORMAT const resolved_format = (position_format == DXGI_FORMAT_UNKNOWN)
                                            ? (desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT)
                                            : position_format;
    uint32_t const resolved_offset = (position_format == DXGI_FORMAT_UNKNOWN)
                                              ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION)
                                              : position_offset;
    // The skinning weight/index layout comes from a calibration result; the static path has none (nullptr).
    MaskSkinLayout const skin_layout_ref = skin_layout ? *skin_layout : MaskSkinLayout{};
    LayoutKey const key{
        .skinned = skinned,
        .full_prec = desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC),
        .position_format = static_cast<uint32_t>(resolved_format),
        .position_offset = resolved_offset,
        .skinning_offset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING),
        .stride = stride,
        .weight_format = static_cast<uint32_t>(skin_layout_ref.weight_format),
        .weight_offset = skin_layout_ref.weight_offset,
        .index_format = static_cast<uint32_t>(skin_layout_ref.index_format),
        .index_offset = skin_layout_ref.index_offset,
    };

    for (auto const& [cached, layout] : m_layout_cache)
    {
        if (cached == key)
            return layout;
    }

    D3D11_INPUT_ELEMENT_DESC elements[3]{};
    UINT count = 0;
    elements[count++] = {
        .SemanticName = "POSITION",
        .SemanticIndex = 0,
        .Format = resolved_format,
        .InputSlot = 0,
        .AlignedByteOffset = resolved_offset,
        .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
        .InstanceDataStepRate = 0
    };
    if (skinned)
    {
        // The layout inside the SKINNING block comes from the self-calibration (the weight/index
        // formats and byte offsets); the semantic name order (BLENDWEIGHT / BLENDINDICES) matches
        // the skinned VS.
        elements[count++] = {
            .SemanticName = "BLENDWEIGHT",
            .SemanticIndex = 0,
            .Format = skin_layout_ref.weight_format,
            .InputSlot = 0,
            .AlignedByteOffset = skin_layout_ref.weight_offset,
            .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
            .InstanceDataStepRate = 0
        };
        elements[count++] = {
            .SemanticName = "BLENDINDICES",
            .SemanticIndex = 0,
            .Format = skin_layout_ref.index_format,
            .InputSlot = 0,
            .AlignedByteOffset = skin_layout_ref.index_offset,
            .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
            .InstanceDataStepRate = 0
        };
    }

    ID3D11InputLayout* layout = nullptr;
    HRESULT const hr = device->CreateInputLayout(elements, count, blob->GetBufferPointer(), blob->GetBufferSize(), &layout);
    if (FAILED(hr) || !layout)
    {
        static bool s_layout_failure_reported = false;
        if (!s_layout_failure_reported)
        {
            s_layout_failure_reported = true;
            logger::error("outline mask: CreateInputLayout failed ({:X}), affected meshes skipped", static_cast<unsigned int>(hr));
        }
        return nullptr;
    }
    m_layout_cache.emplace_back(key, layout);
    return layout;
}

void MaskGeometryPass::release_layouts()
{
    for (ID3D11InputLayout* const val : m_layout_cache | std::views::values)
    {
        if (val)
            val->Release();
    }
    m_layout_cache.clear();
}

namespace
{
    struct PerDrawCBData
    {
        DirectX::XMFLOAT4X4 mvp;
        uint32_t object_id;
        float pad[3];
    };
    static_assert(sizeof(PerDrawCBData) == 80);
    static_assert(offsetof(PerDrawCBData, object_id) == 64);
    static_assert(sizeof(DirectX::XMFLOAT4) == 16);

    // Silhouette inner fill factor (keeps the existing brightness)
    constexpr float Silhouette_Fill_Alpha = 0.5f;

    bool draw_fullscreen_triangle(
        ID3D11DeviceContext* context, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* mask_srv,
        uint32_t width, uint32_t height,
        ID3D11VertexShader* vs, ID3D11PixelShader* ps, ID3D11BlendState* blend,
        ID3D11DepthStencilState* depth_none, ID3D11RasterizerState* cull_none,
        ID3D11Buffer* cb0, void const* cb0_data, size_t cb0_bytes,
        ID3D11ShaderResourceView* style_srv)
    {
        if (!target || !mask_srv || !vs || !ps || !blend || !cb0 || !style_srv)
            return false;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(cb0, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        std::memcpy(mapped.pData, cb0_data, cb0_bytes);
        context->Unmap(cb0, 0);

        D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        context->OMSetRenderTargets(1, &target, nullptr);
        context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(depth_none, 0);
        context->RSSetState(cull_none);
        context->RSSetViewports(1, &vp);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* const no_vb = nullptr;
        UINT const zero = 0;
        context->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
        context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView* const srvs[] = { mask_srv, style_srv };
        context->PSSetShaderResources(0, 2, srvs);
        ID3D11Buffer* previous_cb = nullptr;
        context->PSGetConstantBuffers(0, 1, &previous_cb);
        context->PSSetConstantBuffers(0, 1, &cb0);
        context->Draw(3, 0);
        context->PSSetConstantBuffers(0, 1, &previous_cb);
        if (previous_cb)
            previous_cb->Release();
        ID3D11ShaderResourceView* const empty_srvs[2]{};
        context->PSSetShaderResources(0, 2, empty_srvs);
        return true;
    }
}

// ---------------------------------------------------------------------------
// MaskRenderTarget
// ---------------------------------------------------------------------------

RenderTarget::RenderTarget() :
    m_ref_device(nullptr),
    m_texture(nullptr),
    m_rtv(nullptr),
    m_srv(nullptr),
    m_width(0),
    m_height(0) {}

RenderTarget::~RenderTarget()
{
    release();
}

bool RenderTarget::matches(ID3D11Device* device, uint32_t width, uint32_t height) const noexcept
{
    return m_ref_device == device && m_width == width && m_height == height && m_srv && m_dsv;
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

bool RenderTarget::init(ID3D11Device* device, uint32_t width, uint32_t height)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT const tex_hr = device->CreateTexture2D(&td, nullptr, &m_texture);
    if (FAILED(tex_hr) || !m_texture)
    {
        logger::error("Outline mask: failed to create mask texture ({:X})", static_cast<unsigned int>(tex_hr));
        release();
        return false;
    }
    HRESULT const rtv_hr = device->CreateRenderTargetView(m_texture, nullptr, &m_rtv);
    HRESULT const srv_hr = device->CreateShaderResourceView(m_texture, nullptr, &m_srv);
    if (FAILED(rtv_hr) || !m_rtv || FAILED(srv_hr) || !m_srv)
    {
        logger::error("Outline mask: failed to create mask views (rtv={:X}, srv={:X})", static_cast<unsigned int>(rtv_hr), static_cast<unsigned int>(srv_hr));
        release();
        return false;
    }

    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    HRESULT const depth_hr = device->CreateTexture2D(&td, nullptr, &m_depth_texture);
    if (FAILED(depth_hr) || FAILED(device->CreateDepthStencilView(m_depth_texture, nullptr, &m_dsv)))
    {
        logger::error("Outline mask: failed to create private depth target");
        release();
        return false;
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
    m_vs_static(nullptr),
    m_vs_skinned(nullptr),
    m_ps_mask(nullptr),
    m_vs_static_blob(nullptr),
    m_vs_skinned_blob(nullptr),
    m_per_draw_cb(nullptr),
    m_palette_cb(nullptr),
    m_blend_mask_write(nullptr),
    m_depth_nearest(nullptr),
    m_depth_disabled(nullptr),
    m_rasterizer(nullptr),
    m_ready(false),
    m_failed(false),
    m_upload_transposed(false) {}

MaskGeometryPass::~MaskGeometryPass()
{
    release();
}

bool MaskGeometryPass::init(ID3D11Device* device)
{
    if (m_ready || m_failed)
        return m_ready;

    m_ready = create_pipeline(device);
    if (!m_ready)
    {
        release();
        m_failed = true;
        logger::error("outline mask pipeline creation failed, mask rendering disabled");
    }
    return m_ready;
}

bool MaskGeometryPass::create_pipeline(ID3D11Device* device)
{
    m_vs_static_blob = compile_shader(render_shaders::MaskGeometry, "vs_static_main", "vs_5_0", "outline mask static", "outline mask");
    m_vs_skinned_blob = compile_shader(render_shaders::MaskGeometry, "vs_skinned_main", "vs_5_0", "outline mask skinned", "outline mask");
    ID3DBlob* ps_mask_blob = compile_shader(render_shaders::MaskGeometry, "ps_main", "ps_5_0", "outline mask", "outline mask");
    if (!m_vs_static_blob || !m_vs_skinned_blob || !ps_mask_blob)
    {
        if (ps_mask_blob)
            ps_mask_blob->Release();
        return false;
    }

    device->CreateVertexShader(m_vs_static_blob->GetBufferPointer(), m_vs_static_blob->GetBufferSize(), nullptr, &m_vs_static);
    device->CreateVertexShader(m_vs_skinned_blob->GetBufferPointer(), m_vs_skinned_blob->GetBufferSize(), nullptr, &m_vs_skinned);
    device->CreatePixelShader(ps_mask_blob->GetBufferPointer(), ps_mask_blob->GetBufferSize(), nullptr, &m_ps_mask);
    ps_mask_blob->Release();

    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = 80;  // PerDrawCBData: float4x4 + uint object_id + pad[3]
    device->CreateBuffer(&cb, nullptr, &m_per_draw_cb);
    cb.ByteWidth = static_cast<UINT>(Palette_CB_Bytes);
    device->CreateBuffer(&cb, nullptr, &m_palette_cb);

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blend, &m_blend_mask_write);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;  // Independent outlines and full-screen composites.
    device->CreateDepthStencilState(&depth, &m_depth_disabled);

    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_GREATER;
    device->CreateDepthStencilState(&depth, &m_depth_nearest);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&raster, &m_rasterizer);

    bool const ready = m_vs_static && m_vs_skinned && m_ps_mask &&
                       m_per_draw_cb && m_palette_cb && m_blend_mask_write &&
                       m_depth_disabled && m_depth_nearest && m_rasterizer;
    if (!ready)
        return false;

    logger::info("outline mask pipeline ready (palette {} bones/draw)", Max_Palette_Bones);
    return true;
}

void MaskGeometryPass::release()
{
    m_failed = false;
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
    if (m_rasterizer)
    {
        m_rasterizer->Release();
        m_rasterizer = nullptr;
    }
    if (m_depth_disabled)
    {
        m_depth_disabled->Release();
        m_depth_disabled = nullptr;
    }
    if (m_blend_mask_write)
    {
        m_blend_mask_write->Release();
        m_blend_mask_write = nullptr;
    }
    if (m_vs_static_blob)
    {
        m_vs_static_blob->Release();
        m_vs_static_blob = nullptr;
    }
    if (m_vs_skinned_blob)
    {
        m_vs_skinned_blob->Release();
        m_vs_skinned_blob = nullptr;
    }
    if (m_ps_mask)
    {
        m_ps_mask->Release();
        m_ps_mask = nullptr;
    }
    if (m_vs_skinned)
    {
        m_vs_skinned->Release();
        m_vs_skinned = nullptr;
    }
    if (m_vs_static)
    {
        m_vs_static->Release();
        m_vs_static = nullptr;
    }
    release_layouts();
    m_ready = false;
}

void MaskGeometryPass::calibrate_upload_orientation(
    RE::NiCamera* camera, DirectX::XMFLOAT4X4 const& view_proj, std::vector<MaskDraw> const& draws,
    uint32_t width, uint32_t height)
{
    static bool s_checked = false;
    if (s_checked || draws.empty() || !draws.front().node)
        return;

    RE::NiPoint3 const anchor = draws.front().node->world.translate;
    RE::NiTransform const& model_transform = draws.front().node->world;
    DirectX::XMFLOAT4X4 model{};
    DirectX::XMStoreFloat4x4(&model, DirectX::XMMatrixIdentity());
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
            model.m[row][col] = model_transform.rotate.entry[row][col] * model_transform.scale;
        model.m[row][3] = model_transform.translate[row];
    }

    DirectX::XMFLOAT4X4 mvp{};
    DirectX::XMStoreFloat4x4(&mvp, DirectX::XMMatrixMultiply(DirectX::XMLoadFloat4x4(&view_proj), DirectX::XMLoadFloat4x4(&model)));

    // Engine pixels: normalised output with a lower-left origin; when the port is in pixels it is first normalised (handled inside project_engine)
    float eng_px = 0.0f;
    float eng_py = 0.0f;
    float eng_depth = 0.0f;
    bool const engine_ok = project(camera, anchor, static_cast<float>(width), static_cast<float>(height), eng_px, eng_py, eng_depth);

    // Direct upload: the clip coordinate of the local origin (0,0,0,1) = column 4 of mvp; transposed upload: = row 3 of mvp
    float const cw = mvp.m[3][3];
    if (engine_ok && cw > 1e-5f)
    {
        s_checked = true;
        float const w = static_cast<float>(width);
        float const h = static_cast<float>(height);
        float const px_d = ((mvp.m[0][3] / cw) * 0.5f + 0.5f) * w;
        float const py_d = (1.0f - ((mvp.m[1][3] / cw) * 0.5f + 0.5f)) * h;
        float const px_t = ((mvp.m[3][0] / cw) * 0.5f + 0.5f) * w;
        float const py_t = (1.0f - ((mvp.m[3][1] / cw) * 0.5f + 0.5f)) * h;
        float const err_d = std::fabs(px_d - eng_px) + std::fabs(py_d - eng_py);
        float const err_t = std::fabs(px_t - eng_px) + std::fabs(py_t - eng_py);
        m_upload_transposed = err_t < err_d;
        float const composed_px = m_upload_transposed ? px_t : px_d;
        float const composed_py = m_upload_transposed ? py_t : py_d;
        logger::info(
            "outline mask: ground-truth delta direct=({:.4f},{:.4f})px transposed-upload=({:.4f},{:.4f})px "
            "-> upload {} anchor engine=({:.2f},{:.2f}) composed=({:.2f},{:.2f})",
            px_d - eng_px, py_d - eng_py, px_t - eng_px, py_t - eng_py,
            m_upload_transposed ? "transposed" : "direct",
            eng_px, eng_py, composed_px, composed_py);
    }
    // Anchor behind the camera / engine projection failed: retry on the next frame (do not latch)
}

void MaskGeometryPass::draw(ID3D11Device* device, ID3D11DeviceContext* context, DirectX::XMFLOAT4X4 const& view_proj, std::span<MaskDraw const> draws)
{
    for (MaskDraw const& draw : draws)
    {
        if (!draw.vertex_buffer || !draw.index_buffer || draw.index_count == 0 || draw.vertex_stride == 0)
            continue;

        // Skinned draws pass the calibrated layout; static draws pass nullptr (behaviour exactly as before)
        ID3D11InputLayout* layout = get_layout(device, draw.skinned ? m_vs_skinned_blob : m_vs_static_blob, draw.skinned, draw.vertex_desc, draw.vertex_stride,
            draw.position_format, draw.position_offset, draw.skinned ? &draw.skin_layout : nullptr);
        if (!layout)
            continue;

        ID3D11VertexShader* vs = draw.skinned ? m_vs_skinned : m_vs_static;
        if (!vs || !m_ps_mask)
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
        if (m_upload_transposed)
            DirectX::XMStoreFloat4x4(&cb_data.mvp, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&per_draw)));
        else
            cb_data.mvp = per_draw;

        cb_data.object_id = draw.target_index + 1;
        update_constant_buffer(context, m_per_draw_cb, &cb_data, sizeof(cb_data));
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
            uint32_t const skin_bones = skin->skinData->GetBoneCount();
            uint32_t const matrix_count = skin->numMatrices;
            uint32_t const palette_count = palette_slot_count(skin);
            if (palette_count == 0 || palette_count > Max_Palette_Bones)
            {
                log_skinned_skip_once(true, draw.node ? draw.node->name.c_str() : nullptr, "palette slot count out of range",
                    fmt::format("partition={} P={} skin_bones={} numMatrices={} budget={}",
                        draw.partition, palette_count, skin_bones, matrix_count, Max_Palette_Bones));
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
                log_skinned_skip_once(true, draw.node ? draw.node->name.c_str() : nullptr, "null bone world transform in palette range",
                    fmt::format("partition={} P={} skin_bones={} numMatrices={}", draw.partition, palette_count, skin_bones, matrix_count));
                continue;
            }
            for (size_t k = palette_count; k < Max_Palette_Bones; ++k)
                palette[k] = palette[palette_count - 1];
            size_t const palette_bytes = Max_Palette_Bones * sizeof(DirectX::XMFLOAT4X4);
            if (m_upload_transposed)
            {
                DirectX::XMFLOAT4X4 palette_upload[Max_Palette_Bones];
                for (size_t k = 0; k < Max_Palette_Bones; ++k)
                    DirectX::XMStoreFloat4x4(&palette_upload[k], DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&palette[k])));
                update_constant_buffer(context, m_palette_cb, palette_upload, palette_bytes);
            }
            else
            {
                update_constant_buffer(context, m_palette_cb, palette, palette_bytes);
            }
            context->VSSetConstantBuffers(1, 1, &m_palette_cb);
        }

        context->IASetInputLayout(layout);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT const stride = draw.vertex_stride;
        UINT const offset = 0;
        ID3D11Buffer* vb = draw.vertex_buffer;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        // BSTriShape::vertexCount and a partition's vertices are both uint16_t: indices are always 16-bit
        context->IASetIndexBuffer(draw.index_buffer, DXGI_FORMAT_R16_UINT, 0);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(m_ps_mask, nullptr, 0);
        context->DrawIndexed(draw.index_count, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Full-screen consumption passes
// ---------------------------------------------------------------------------

FullscreenPass::FullscreenPass() :
    m_vertex_shader(nullptr),
    m_pixel_shader(nullptr),
    m_blend_premul_alpha(nullptr),
    m_cb(nullptr),
    m_ready(false),
    m_failed(false),
    m_style_buffer(nullptr),
    m_style_srv(nullptr),
    m_style_capacity(0),
    m_styles_valid(false) {}

FullscreenPass::~FullscreenPass()
{
    FullscreenPass::release();
}

bool FullscreenPass::init(ID3D11Device* device)
{
    if (m_ready)
        return true;

    ID3DBlob* vs_blob = compile_shader(render_shaders::MaskComposite, "vs_main", "vs_5_0", "outline mask fullscreen", "outline mask");
    if (vs_blob)
    {
        device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vertex_shader);
        vs_blob->Release();
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blend, &m_blend_premul_alpha);

    m_ready = m_vertex_shader && m_blend_premul_alpha;
    return m_ready;
}

void FullscreenPass::release()
{
    m_styles_valid = false;
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
    if (m_blend_premul_alpha)
    {
        m_blend_premul_alpha->Release();
        m_blend_premul_alpha = nullptr;
    }
    if (m_vertex_shader)
    {
        m_vertex_shader->Release();
        m_vertex_shader = nullptr;
    }
}

bool FullscreenPass::update_styles(ID3D11Device* device, ID3D11DeviceContext* context, std::span<MaskTarget const> targets)
{
    m_styles_valid = false;
    if (targets.empty() || targets.size() > UINT_MAX / sizeof(DirectX::XMFLOAT4))
        return false;
    if (targets.size() > m_style_capacity)
    {
        if (m_style_srv)
            m_style_srv->Release();
        if (m_style_buffer)
            m_style_buffer->Release();
        m_style_srv = nullptr;
        m_style_buffer = nullptr;
        m_style_capacity = 0;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(targets.size() * sizeof(DirectX::XMFLOAT4));
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(DirectX::XMFLOAT4);
        if (FAILED(device->CreateBuffer(&desc, nullptr, &m_style_buffer)))
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = static_cast<UINT>(targets.size());
        if (FAILED(device->CreateShaderResourceView(m_style_buffer, &srv, &m_style_srv)))
            return false;
        m_style_capacity = targets.size();
    }
    std::vector<DirectX::XMFLOAT4> styles;
    styles.reserve(targets.size());
    for (MaskTarget const& target : targets)
        styles.push_back(target.color);
    D3D11_BOX const box{ 0, 0, 0, static_cast<UINT>(styles.size() * sizeof(DirectX::XMFLOAT4)), 1, 1 };
    context->UpdateSubresource(m_style_buffer, 0, &box, styles.data(), 0, 0);
    m_styles_valid = SUCCEEDED(device->GetDeviceRemovedReason());
    return m_styles_valid;
}

SilhouettePass::~SilhouettePass()
{
    release();
}

bool SilhouettePass::init(ID3D11Device* device)
{
    if (m_ready || m_failed)
        return m_ready;

    if (!FullscreenPass::init(device))
    {
        m_failed = true;
        return false;
    }

    ID3DBlob* ps_blob = compile_shader(render_shaders::MaskComposite, "ps_silhouette_main", "ps_5_0", "outline mask silhouette", "outline mask");
    if (ps_blob)
    {
        device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_pixel_shader);
        ps_blob->Release();
    }

    // b0: radius, fill factor, padding.
    D3D11_BUFFER_DESC ccb{};
    ccb.Usage = D3D11_USAGE_DYNAMIC;
    ccb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ccb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ccb.ByteWidth = 16;
    device->CreateBuffer(&ccb, nullptr, &m_cb);

    m_ready = m_pixel_shader && m_cb;
    if (!m_ready)
    {
        m_failed = true;
        logger::warn("outline mask silhouette pass unavailable, inner fill disabled (mask rendering stays active)");
    }
    return m_ready;
}

void SilhouettePass::release()
{
    if (m_cb)
    {
        m_cb->Release();
        m_cb = nullptr;
    }
    if (m_pixel_shader)
    {
        m_pixel_shader->Release();
        m_pixel_shader = nullptr;
    }
    m_ready = false;
    m_failed = false;  // allow a rebuild after a device change (same behaviour as the original release_pipeline → rebuild)
}

bool SilhouettePass::draw(
    ID3D11DeviceContext* context,
    ID3D11RenderTargetView* target,
    ID3D11ShaderResourceView* mask_srv,
    uint32_t width,
    uint32_t height,
    ID3D11DepthStencilState* depth_none,
    ID3D11RasterizerState* cull_none) const
{
    if (!m_ready || !m_styles_valid)
        return false;

    float const cb_data[4] = { 0.0f, Silhouette_Fill_Alpha, 0.0f, 0.0f };
    return draw_fullscreen_triangle(context, target, mask_srv, width, height,
        m_vertex_shader, m_pixel_shader, m_blend_premul_alpha, depth_none, cull_none,
        m_cb, cb_data, sizeof(cb_data), m_style_srv);
}

OutlinePass::~OutlinePass()
{
    release();
}

bool OutlinePass::init(ID3D11Device* device)
{
    if (m_ready || m_failed)
        return m_ready;

    if (!FullscreenPass::init(device))
    {
        m_failed = true;
        return false;
    }

    ID3DBlob* ps_blob = compile_shader(render_shaders::MaskComposite, "ps_outline_main", "ps_5_0", "outline mask outline", "outline mask");
    if (ps_blob)
    {
        device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_pixel_shader);
        ps_blob->Release();
    }

    // b0: radius, fill factor, padding.
    D3D11_BUFFER_DESC ocb{};
    ocb.Usage = D3D11_USAGE_DYNAMIC;
    ocb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ocb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ocb.ByteWidth = 16;
    device->CreateBuffer(&ocb, nullptr, &m_cb);

    m_ready = m_pixel_shader && m_cb;
    if (!m_ready)
    {
        m_failed = true;
        logger::warn("outline mask outline pass unavailable, corpse outline band disabled (mask rendering stays active)");
    }
    return m_ready;
}

void OutlinePass::release()
{
    if (m_cb)
    {
        m_cb->Release();
        m_cb = nullptr;
    }
    if (m_pixel_shader)
    {
        m_pixel_shader->Release();
        m_pixel_shader = nullptr;
    }
    m_ready = false;
    m_failed = false;
}

bool OutlinePass::draw(
    ID3D11DeviceContext* context,
    ID3D11RenderTargetView* target,
    ID3D11ShaderResourceView* mask_srv,
    uint32_t width,
    uint32_t height,
    int thickness,
    ID3D11DepthStencilState* depth_none,
    ID3D11RasterizerState* cull_none) const
{
    if (!m_ready || !m_styles_valid)
        return false;

    float const cb_data[4] = { static_cast<float>(thickness), 1.0f, 0.0f, 0.0f };
    return draw_fullscreen_triangle(context, target, mask_srv, width, height,
        m_vertex_shader, m_pixel_shader, m_blend_premul_alpha, depth_none, cull_none,
        m_cb, cb_data, sizeof(cb_data), m_style_srv);
}

MASK_NAMESPACE_END
