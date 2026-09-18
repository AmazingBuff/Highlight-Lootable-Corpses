//
// Created by AmazingBuff on 2026/9/13.
//

#include "mask_passes.h"

#include "mask_geometry.h"

#include "render/render_util.h"
#include "render/shader_sources.h"

#include "render/dx11/common_states.h"
#include "render/dx11/d3d11_util.h"

MASK_NAMESPACE_BEGIN

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

    struct GlowCBData
    {
        DirectX::XMFLOAT4 narrow[Glow::Kernel_Slot_Count];
        DirectX::XMFLOAT4 wide[Glow::Kernel_Slot_Count];
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

    // Silhouette inner fill factor (keeps the existing brightness)
    constexpr float Silhouette_Fill_Alpha = 0.5f;

    bool draw_fullscreen_triangle(
        REX::W32::ID3D11DeviceContext* context, REX::W32::ID3D11RenderTargetView* target, REX::W32::ID3D11ShaderResourceView* mask_srv,
        uint32_t width, uint32_t height,
        REX::W32::ID3D11VertexShader* vs, REX::W32::ID3D11PixelShader* ps, REX::W32::ID3D11BlendState* blend,
        REX::W32::ID3D11DepthStencilState* depth_none, REX::W32::ID3D11RasterizerState* cull_none,
        REX::W32::ID3D11Buffer* cb0, void const* cb0_data, size_t cb0_bytes,
        REX::W32::ID3D11ShaderResourceView* style_srv)
    {
        if (!context || !target || !mask_srv || !vs || !ps || !blend || !cb0 || !style_srv)
            return false;

        REX::W32::ID3D11ShaderResourceView* previous_srvs[2]{};
        context->PSGetShaderResources(0, 2, previous_srvs);
        REX::W32::ID3D11Buffer* previous_cb = nullptr;
        context->PSGetConstantBuffers(0, 1, &previous_cb);
        auto const restore_bindings = [&]() {
            context->PSSetConstantBuffers(0, 1, &previous_cb);
            context->PSSetShaderResources(0, 2, previous_srvs);
            if (previous_cb)
                previous_cb->Release();
            for (REX::W32::ID3D11ShaderResourceView* srv : previous_srvs)
            {
                if (srv)
                    srv->Release();
            }
        };

        REX::W32::ID3D11ShaderResourceView* const empty_srvs[2]{};
        context->PSSetShaderResources(0, 2, empty_srvs);
        REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!REX::W32::SUCCESS(context->Map(cb0, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            restore_bindings();
            return false;
        }
        std::memcpy(mapped.data, cb0_data, cb0_bytes);
        context->Unmap(cb0, 0);

        REX::W32::D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        context->OMSetRenderTargets(1, &target, nullptr);
        context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(depth_none, 0);
        context->RSSetState(cull_none);
        context->RSSetViewports(1, &vp);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        REX::W32::ID3D11Buffer* const no_vb = nullptr;
        uint32_t const zero = 0;
        context->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
        context->IASetIndexBuffer(nullptr, REX::W32::DXGI_FORMAT_UNKNOWN, 0);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        REX::W32::ID3D11ShaderResourceView* const srvs[] = { mask_srv, style_srv };
        context->PSSetShaderResources(0, 2, srvs);
        context->PSSetConstantBuffers(0, 1, &cb0);
        context->Draw(3, 0);
        restore_bindings();
        return true;
    }
}

// ---------------------------------------------------------------------------
// MaskRenderTarget
// ---------------------------------------------------------------------------

RenderTarget::RenderTarget() :
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

bool RenderTarget::matches(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height) const noexcept
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

bool RenderTarget::init(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height)
{
    REX::W32::D3D11_TEXTURE2D_DESC td{};
    td.width = width;
    td.height = height;
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = REX::W32::DXGI_FORMAT_R32_UINT;
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

    td.format = REX::W32::DXGI_FORMAT_D32_FLOAT;
    td.bindFlags = REX::W32::D3D11_BIND_DEPTH_STENCIL;
    REX::W32::HRESULT const depth_hr = device->CreateTexture2D(&td, nullptr, &m_depth_texture);
    if (!REX::W32::SUCCESS(depth_hr) || !REX::W32::SUCCESS(device->CreateDepthStencilView(m_depth_texture, nullptr, &m_dsv)))
    {
        logger::error("Mask overlay: failed to create private depth target");
        release();
        return false;
    }

    m_ref_device = device;
    m_width = width;
    m_height = height;
    return true;
}

GlowScratch::GlowScratch() :
    m_ref_device(nullptr),
    m_texture(nullptr),
    m_rtv(nullptr),
    m_srv(nullptr),
    m_width(0),
    m_height(0) {}

GlowScratch::~GlowScratch()
{
    release();
}

bool GlowScratch::matches(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height) const noexcept
{
    return m_ref_device == device && m_width == width && m_height == height && m_rtv && m_srv;
}

void GlowScratch::release()
{
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
    m_ref_device = nullptr;
    m_width = 0;
    m_height = 0;
}

bool GlowScratch::init(REX::W32::ID3D11Device* device, uint32_t width, uint32_t height)
{
    if (!device || width == 0 || height == 0)
        return false;
    if (matches(device, width, height))
        return true;

    release();
    REX::W32::D3D11_TEXTURE2D_DESC desc{};
    desc.width = width;
    desc.height = height;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = REX::W32::DXGI_FORMAT_R16G16_FLOAT;
    desc.sampleDesc.count = 1;
    desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
    desc.bindFlags = REX::W32::D3D11_BIND_RENDER_TARGET | REX::W32::D3D11_BIND_SHADER_RESOURCE;

    REX::W32::HRESULT const texture_hr = device->CreateTexture2D(&desc, nullptr, &m_texture);
    if (!REX::W32::SUCCESS(texture_hr) || !m_texture)
    {
        logger::error("Mask glow: failed to create R16G16_FLOAT scratch ({:X})", static_cast<unsigned int>(texture_hr));
        release();
        return false;
    }
    REX::W32::HRESULT const rtv_hr = device->CreateRenderTargetView(m_texture, nullptr, &m_rtv);
    REX::W32::HRESULT const srv_hr = device->CreateShaderResourceView(m_texture, nullptr, &m_srv);
    if (!REX::W32::SUCCESS(rtv_hr) || !m_rtv || !REX::W32::SUCCESS(srv_hr) || !m_srv)
    {
        logger::error("Mask glow: failed to create scratch views (rtv={:X}, srv={:X})", static_cast<unsigned int>(rtv_hr), static_cast<unsigned int>(srv_hr));
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
    m_depth_nearest(nullptr),
    m_upload_transposed(false) {}

MaskGeometryPass::~MaskGeometryPass()
{
    release();
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
    m_vs_static_blob = compile_shader(render_shaders::MaskGeometry, "vs_static_main", "vs_5_0", "outline mask static", "outline mask");
    m_vs_skinned_blob = compile_shader(render_shaders::MaskGeometry, "vs_skinned_main", "vs_5_0", "outline mask skinned", "outline mask");
    REX::W32::ID3DBlob* ps_mask_blob = compile_shader(render_shaders::MaskGeometry, "ps_main", "ps_5_0", "outline mask", "outline mask");
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

    REX::W32::D3D11_BUFFER_DESC cb{};
    cb.usage = REX::W32::D3D11_USAGE_DYNAMIC;
    cb.bindFlags = REX::W32::D3D11_BIND_CONSTANT_BUFFER;
    cb.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
    cb.byteWidth = 80;  // PerDrawCBData: float4x4 + uint object_id + pad[3]
    device->CreateBuffer(&cb, nullptr, &m_per_draw_cb);
    cb.byteWidth = static_cast<uint32_t>(Palette_CB_Bytes);
    device->CreateBuffer(&cb, nullptr, &m_palette_cb);

    // Reverse-Z nearest test (GREATER): no CommonStates equivalent - the pipeline is reverse-Z.
    REX::W32::D3D11_DEPTH_STENCIL_DESC depth{};
    depth.depthEnable = true;
    depth.depthWriteMask = REX::W32::D3D11_DEPTH_WRITE_MASK_ALL;
    depth.depthFunc = REX::W32::D3D11_COMPARISON_GREATER;
    device->CreateDepthStencilState(&depth, &m_depth_nearest);

    bool const ready = m_vs_static && m_vs_skinned && m_ps_mask &&
                       m_per_draw_cb && m_palette_cb && m_depth_nearest;
    if (!ready)
        return false;

    logger::info("Mask overlay pipeline ready (palette {} bones/draw)", Max_Palette_Bones);
    return true;
}

void MaskGeometryPass::release()
{
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
}

void MaskGeometryPass::calibrate_upload_orientation(
    RE::NiCamera* camera, DirectX::XMFLOAT4X4 const& view_proj, std::vector<MaskDraw> const& draws,
    uint32_t width, uint32_t height)
{
    static bool s_checked = false;
    if (s_checked || draws.empty() || !draws.front().node)
        return;

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

    RE::NiPoint3 const anchor = draws.front().node->world.translate;
    bool const engine_ok = project(camera, render_cast(anchor), static_cast<float>(width), static_cast<float>(height), eng_px, eng_py, eng_depth);

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
            "Mask overlay: ground-truth delta direct=({:.4f},{:.4f})px transposed-upload=({:.4f},{:.4f})px "
            "-> upload {} anchor engine=({:.2f},{:.2f}) composed=({:.2f},{:.2f})",
            px_d - eng_px, py_d - eng_py, px_t - eng_px, py_t - eng_py,
            m_upload_transposed ? "transposed" : "direct",
            eng_px, eng_py, composed_px, composed_py);
    }
    // Anchor behind the camera / engine projection failed: retry on the next frame (do not latch)
}

void MaskGeometryPass::draw(REX::W32::ID3D11Device* device, REX::W32::ID3D11DeviceContext* context, DirectX::XMFLOAT4X4 const& view_proj, std::span<MaskDraw const> draws)
{
    for (MaskDraw const& draw : draws)
    {
        if (!draw.vertex_buffer || !draw.index_buffer || draw.index_count == 0 || draw.vertex_stride == 0)
            continue;

        // Skinned draws pass the calibrated layout; static draws pass nullptr (behaviour exactly as before)
        REX::W32::ID3D11InputLayout* layout = get_layout(device, draw.skinned ? m_vs_skinned_blob : m_vs_static_blob, draw.skinned, draw.vertex_desc, draw.vertex_stride,
            draw.position_format, draw.position_offset, draw.skinned ? &draw.skin_layout : nullptr);
        if (!layout)
            continue;

        REX::W32::ID3D11VertexShader* vs = draw.skinned ? m_vs_skinned : m_vs_static;
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
        context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        uint32_t const stride = draw.vertex_stride;
        uint32_t const offset = 0;
        REX::W32::ID3D11Buffer* vb = draw.vertex_buffer;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        // BSTriShape::vertexCount and a partition's vertices are both uint16_t: indices are always 16-bit
        context->IASetIndexBuffer(draw.index_buffer, REX::W32::DXGI_FORMAT_R16_UINT, 0);
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
    m_cb(nullptr),
    m_style_buffer(nullptr),
    m_style_srv(nullptr),
    m_style_capacity(0) {}

FullscreenPass::~FullscreenPass()
{
    FullscreenPass::release();
}

bool FullscreenPass::init(REX::W32::ID3D11Device* device)
{
    REX::W32::ID3DBlob* vs_blob = compile_shader(render_shaders::MaskComposite, "vs_main", "vs_5_0", "outline mask fullscreen", "outline mask");
    if (vs_blob)
    {
        device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vertex_shader);
        vs_blob->Release();
    }

    // The premultiplied-alpha blend state comes from the shared CommonStates (alpha_blend) at draw time.
    return m_vertex_shader != nullptr;
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
    if (m_vertex_shader)
    {
        m_vertex_shader->Release();
        m_vertex_shader = nullptr;
    }
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

SilhouettePass::~SilhouettePass()
{
    release();
}

bool SilhouettePass::init(REX::W32::ID3D11Device* device)
{
    if (!FullscreenPass::init(device))
        return false;

    REX::W32::ID3DBlob* ps_blob = compile_shader(render_shaders::MaskComposite, "ps_silhouette_main", "ps_5_0", "outline mask silhouette", "outline mask");
    if (ps_blob)
    {
        device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_pixel_shader);
        ps_blob->Release();
    }

    // b0: radius, fill factor, padding.
    REX::W32::D3D11_BUFFER_DESC ccb{};
    ccb.usage = REX::W32::D3D11_USAGE_DYNAMIC;
    ccb.bindFlags = REX::W32::D3D11_BIND_CONSTANT_BUFFER;
    ccb.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
    ccb.byteWidth = 16;
    device->CreateBuffer(&ccb, nullptr, &m_cb);

    const bool ready = m_pixel_shader && m_cb;
    if (!ready)
    {
        release();
        logger::warn("Mask overlay silhouette pass unavailable, inner fill disabled (mask rendering stays active)");
    }
    return ready;
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
}

bool SilhouettePass::draw(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11RenderTargetView* target,
    REX::W32::ID3D11ShaderResourceView* mask_srv,
    uint32_t width,
    uint32_t height,
    CommonStates const& states) const
{
    float const cb_data[4] = { 0.0f, Silhouette_Fill_Alpha, 0.0f, 0.0f };
    return draw_fullscreen_triangle(context, target, mask_srv, width, height,
        m_vertex_shader, m_pixel_shader, states.alpha_blend(), states.depth_none(), states.cull_none(),
        m_cb, cb_data, sizeof(cb_data), m_style_srv);
}

OutlinePass::OutlinePass() : m_horizontal_shader(nullptr) {}

OutlinePass::~OutlinePass()
{
    release();
}

bool OutlinePass::init(REX::W32::ID3D11Device* device)
{
    if (!FullscreenPass::init(device))
        return false;

    REX::W32::ID3DBlob* horizontal_blob = compile_shader(render_shaders::MaskGlow, "ps_glow_horizontal", "ps_5_0", "outline glow horizontal", "outline glow");
    if (horizontal_blob)
    {
        device->CreatePixelShader(horizontal_blob->GetBufferPointer(), horizontal_blob->GetBufferSize(), nullptr, &m_horizontal_shader);
        horizontal_blob->Release();
    }
    REX::W32::ID3DBlob* vertical_blob = compile_shader(render_shaders::MaskGlow, "ps_glow_vertical", "ps_5_0", "outline glow vertical", "outline glow");
    if (vertical_blob)
    {
        device->CreatePixelShader(vertical_blob->GetBufferPointer(), vertical_blob->GetBufferSize(), nullptr, &m_pixel_shader);
        vertical_blob->Release();
    }

    REX::W32::D3D11_BUFFER_DESC glow_cb{};
    glow_cb.usage = REX::W32::D3D11_USAGE_DYNAMIC;
    glow_cb.bindFlags = REX::W32::D3D11_BIND_CONSTANT_BUFFER;
    glow_cb.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
    glow_cb.byteWidth = sizeof(GlowCBData);
    device->CreateBuffer(&glow_cb, nullptr, &m_cb);

    const bool ready = m_horizontal_shader && m_pixel_shader && m_cb;
    if (!ready)
    {
        release();
        logger::warn("Mask glow pass unavailable, corpse outline glow disabled (mask rendering stays active)");
    }
    return ready;
}

void OutlinePass::release()
{
    m_scratch.release();
    if (m_cb)
    {
        m_cb->Release();
        m_cb = nullptr;
    }
    if (m_horizontal_shader)
    {
        m_horizontal_shader->Release();
        m_horizontal_shader = nullptr;
    }
    if (m_pixel_shader)
    {
        m_pixel_shader->Release();
        m_pixel_shader = nullptr;
    }
}

bool OutlinePass::draw(
    REX::W32::ID3D11Device* device,
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11RenderTargetView* target,
    REX::W32::ID3D11ShaderResourceView* mask_srv,
    uint32_t width,
    uint32_t height,
    uint32_t object_id,
    int thickness,
    ROI::Rect horizontal_rect,
    ROI::Rect vertical_rect,
    CommonStates const& states)
{
    Glow::KernelProfile const profile = Glow::make_kernel_profile(thickness);
    GlowCBData cb_data{};
    std::memcpy(cb_data.narrow, profile.narrow.data(), sizeof(profile.narrow));
    std::memcpy(cb_data.wide, profile.wide.data(), sizeof(profile.wide));
    cb_data.radius = static_cast<uint32_t>(profile.radius);
    cb_data.width = width;
    cb_data.height = height;
    cb_data.object_id = object_id;
    cb_data.horizontal_left = horizontal_rect.left;
    cb_data.horizontal_top = horizontal_rect.top;
    cb_data.horizontal_right = horizontal_rect.right;
    cb_data.horizontal_bottom = horizontal_rect.bottom;

    REX::W32::ID3D11ShaderResourceView* previous_srvs[3]{};
    context->PSGetShaderResources(0, 3, previous_srvs);
    REX::W32::ID3D11Buffer* previous_cb = nullptr;
    context->PSGetConstantBuffers(0, 1, &previous_cb);
    auto const restore_bindings = [&]() {
        context->PSSetConstantBuffers(0, 1, &previous_cb);
        context->PSSetShaderResources(0, 3, previous_srvs);
        if (previous_cb)
            previous_cb->Release();
        for (REX::W32::ID3D11ShaderResourceView* srv : previous_srvs)
        {
            if (srv)
                srv->Release();
        }
    };

    REX::W32::ID3D11ShaderResourceView* const empty_srvs[3]{};
    context->PSSetShaderResources(0, 3, empty_srvs);
    if (!m_scratch.init(device, width, height))
    {
        restore_bindings();
        return false;
    }

    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!REX::W32::SUCCESS(context->Map(m_cb, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        restore_bindings();
        return false;
    }
    std::memcpy(mapped.data, &cb_data, sizeof(cb_data));
    context->Unmap(m_cb, 0);

    REX::W32::D3D11_VIEWPORT const viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    REX::W32::ID3D11Buffer* const no_vb = nullptr;
    uint32_t const zero = 0;
    context->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
    context->IASetIndexBuffer(nullptr, REX::W32::DXGI_FORMAT_UNKNOWN, 0);
    context->VSSetShader(m_vertex_shader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &m_cb);

    REX::W32::ID3D11RenderTargetView* const scratch_rtv = m_scratch.rtv();
    context->OMSetRenderTargets(1, &scratch_rtv, nullptr);
    context->OMSetBlendState(states.opaque(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(states.depth_none(), 0);
    context->RSSetState(states.cull_none_scissor());
    REX::W32::D3D11_RECT const horizontal_scissor{
        horizontal_rect.left, horizontal_rect.top, horizontal_rect.right, horizontal_rect.bottom };
    context->RSSetScissorRects(1, &horizontal_scissor);
    context->PSSetShader(m_horizontal_shader, nullptr, 0);
    REX::W32::ID3D11ShaderResourceView* const horizontal_srvs[] = { mask_srv };
    context->PSSetShaderResources(0, 1, horizontal_srvs);
    context->Draw(3, 0);

    context->PSSetShaderResources(0, 3, empty_srvs);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(states.alpha_blend(), nullptr, 0xFFFFFFFF);
    context->RSSetState(states.cull_none_scissor());
    REX::W32::D3D11_RECT const vertical_scissor{
        vertical_rect.left, vertical_rect.top, vertical_rect.right, vertical_rect.bottom };
    context->RSSetScissorRects(1, &vertical_scissor);
    context->PSSetShader(m_pixel_shader, nullptr, 0);
    REX::W32::ID3D11ShaderResourceView* const vertical_srvs[] = { mask_srv, m_scratch.srv(), m_style_srv };
    context->PSSetShaderResources(0, 3, vertical_srvs);
    context->Draw(3, 0);

    restore_bindings();
    return true;
}

MASK_NAMESPACE_END
