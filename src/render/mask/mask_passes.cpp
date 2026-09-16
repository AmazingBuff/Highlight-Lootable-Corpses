//
// Created by AmazingBuff on 2026/9/13.
//

#include "mask_passes.h"

#include "mask_geometry.h"
#include "render/shader_sources.h"
#include "render/dx11/d3d11_util.h"

#include <algorithm>
#include <cstring>

MASK_NAMESPACE_BEGIN

namespace
{
    // ---------------------------------------------------------------------------
    // MaskGeometryPass 的 InputLayout 缓存：按 (蒙皮, 精度, 属性偏移, 步进) 缓存——
    // 属性偏移来自各 mesh 的 vertexDesc，逐 mesh 创建设备对象不可取，故缓存去重
    //（尸体 mesh 布局种类极少）。
    // ---------------------------------------------------------------------------
    struct LayoutKey
    {
        bool skinned;
        bool full_prec;
        std::uint32_t position_format;  // 位置格式为标定结果，须入键防不同格式共用布局
        std::uint32_t position_offset;
        std::uint32_t skinning_offset;
        std::uint32_t stride;
        // 蒙皮权重/索引布局：标定结果，须入键防不同布局共用同一 InputLayout
        //（静态 draw 保持默认 0/UNKNOWN）。
        std::uint32_t weight_format;
        std::uint32_t weight_offset;
        std::uint32_t index_format;
        std::uint32_t index_offset;

        bool operator==(LayoutKey const&) const = default;
    };
    std::vector<std::pair<LayoutKey, ID3D11InputLayout*>> g_layout_cache;

    // per-draw 常量缓冲：矩阵 + 逐 draw 的尸体索引。HLSL 侧声明为
    // `row_major float4x4 + float g_corpse_index + float3 g_pad`（packing 后同为
    // 80 字节，corpse_index 在 64 字节处）。80 为 16 的倍数，满足 CB 尺寸对齐要求。
    struct PerDrawCBData
    {
        DirectX::XMFLOAT4X4 mvp;  // 静态=ViewProj·World，蒙皮=ViewProj；按 m_upload_transposed 取向上传
        float corpse_index;       // 目标序号/255
        float pad[3];
    };
    static_assert(sizeof(PerDrawCBData) == 80);
    static_assert(sizeof(PerDrawCBData) % 16 == 0);

    // 描边常量缓冲（16 字节对齐）：texel/radius/pad + color
    struct OutlineCBData
    {
        float texel_x;
        float texel_y;
        float radius;
        float pad;
        float color_r;
        float color_g;
        float color_b;
        float color_a;
    };
    static_assert(sizeof(OutlineCBData) == 32);

    // PS 圆盘采样半径上限（(2r+1)² = 169 taps）
    constexpr std::uint32_t Max_Outline_Radius = 6;
    // 色带 alpha 恒 1.0（按距离淡出经 alpha LUT 生效）
    constexpr float Outline_Alpha = 1.0f;
    // 剪影内部填充系数（维持既有亮度）
    constexpr float Silhouette_Fill_Alpha = 0.5f;

    // 全屏三角形消费 pass 的绑定与绘制：OM/深度/光栅化/视口 + IA（无布局无缓冲）+
    // VS/PS + mask SRV/采样器 + PS 常量缓冲槽 b0/b1 就地保存/恢复 + Draw(3)。
    // （b0/b1 被本 pass 改写——外层 restore 路径不覆盖 PS 常量缓冲，故就地恢复）
    bool draw_fullscreen_triangle(
        ID3D11DeviceContext* context, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* mask_srv,
        std::uint32_t width, std::uint32_t height,
        ID3D11VertexShader* vs, ID3D11PixelShader* ps, ID3D11SamplerState* sampler, ID3D11BlendState* blend,
        ID3D11DepthStencilState* depth_none, ID3D11RasterizerState* cull_none,
        ID3D11Buffer* cb0, void const* cb0_data, std::size_t cb0_bytes,
        ID3D11Buffer* alphlut_cb)
    {
        if (!target || !mask_srv || !vs || !ps || !sampler || !blend || !cb0 || !alphlut_cb)
            return false;

        D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        context->OMSetRenderTargets(1, &target, nullptr);
        context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(depth_none, 0);
        context->RSSetState(cull_none);
        context->RSSetViewports(1, &vp);
        context->IASetInputLayout(nullptr);  // SV_VertexID 全屏三角形，无需布局
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* const no_vb = nullptr;
        UINT const zero = 0;
        context->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
        context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->PSSetShaderResources(0, 1, &mask_srv);
        context->PSSetSamplers(0, 1, &sampler);

        ID3D11Buffer* prev_ps_cbs[2] = {};
        context->PSGetConstantBuffers(0, 2, prev_ps_cbs);
        ID3D11Buffer* const ps_cbs[2] = { cb0, alphlut_cb };
        update_constant_buffer(context, cb0, cb0_data, cb0_bytes);
        context->PSSetConstantBuffers(0, 2, ps_cbs);

        context->Draw(3, 0);
        context->PSSetConstantBuffers(0, 2, prev_ps_cbs);
        for (ID3D11Buffer* cb : prev_ps_cbs)
        {
            if (cb)
                cb->Release();  // Get 系列返回已 AddRef 的接口
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// MaskRenderTarget
// ---------------------------------------------------------------------------

RenderTarget::RenderTarget()
    : m_ref_device(nullptr)
    , m_texture(nullptr)
    , m_rtv(nullptr)
    , m_srv(nullptr)
    , m_width(0)
    , m_height(0) {}

RenderTarget::~RenderTarget()
{
    release();
}

bool RenderTarget::matches(ID3D11Device* device, std::uint32_t width, std::uint32_t height) const
{
    return m_ref_device == device && m_width == width && m_height == height && m_srv;
}

void RenderTarget::release()
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
    m_width = 0;
    m_height = 0;
    m_ref_device = nullptr;
}

bool RenderTarget::init(ID3D11Device* device, std::uint32_t width, std::uint32_t height)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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

    m_ref_device = device;
    m_width = width;
    m_height = height;
    return true;
}

// ---------------------------------------------------------------------------
// MaskGeometryPass
// ---------------------------------------------------------------------------

bool MaskGeometryPass::init(ID3D11Device* device)
{
    if (m_ready || m_failed)
        return m_ready;

    m_ready = create_pipeline(device);
    if (!m_ready)
    {
        m_failed = true;
        release();
        logger::error("outline mask pipeline creation failed, mask rendering disabled");
    }
    return m_ready;
}

bool MaskGeometryPass::create_pipeline(ID3D11Device* device)
{
    // ---- mask 关键对象：任一失败则整体禁用 mask 渲染 ----
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
    cb.ByteWidth = 80;  // PerDrawCBData：Mat4 + corpse_index + pad[3]
    device->CreateBuffer(&cb, nullptr, &m_per_draw_cb);
    cb.ByteWidth = static_cast<UINT>(Palette_CB_Bytes);
    device->CreateBuffer(&cb, nullptr, &m_palette_cb);

    // MAX 混合：mask 取各 draw 覆盖的并集（静态/蒙皮两通道剪影在互相重叠时均保持可见）
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blend, &m_blend_mask_write);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;  // 深度测试关闭 —— mask 穿墙
    device->CreateDepthStencilState(&depth, &m_depth_disabled);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;  // 剪影不受三角形绕序影响
    raster.DepthClipEnable = FALSE;
    device->CreateRasterizerState(&raster, &m_rasterizer);

    bool const ready = m_vs_static && m_vs_skinned && m_ps_mask &&
                       m_per_draw_cb && m_palette_cb && m_blend_mask_write &&
                       m_depth_disabled && m_rasterizer;
    if (!ready)
        return false;

    logger::info("outline mask pipeline ready (palette {} bones/draw)", Max_Palette_Bones);
    return true;
}

void MaskGeometryPass::release()
{
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
    m_ready = false;
}

void MaskGeometryPass::release_layouts()
{
    for (auto& entry : g_layout_cache)
    {
        if (entry.second)
            entry.second->Release();
    }
    g_layout_cache.clear();
}

ID3D11InputLayout* MaskGeometryPass::get_layout(
    ID3D11Device* device, bool skinned, RE::BSGraphics::VertexDesc const& desc, std::uint32_t stride,
    DXGI_FORMAT position_format, std::uint32_t position_offset, MaskSkinLayout const* skin_layout)
{
    // 位置格式/偏移：静态路径为标定结果（UNKNOWN 表示按 desc 推导）；蒙皮路径为
    // 属性偏移间距判定结果（绝不为 UNKNOWN）。
    DXGI_FORMAT const resolved_format = (position_format == DXGI_FORMAT_UNKNOWN)
                                            ? (desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT)
                                            : position_format;
    std::uint32_t const resolved_offset = (position_format == DXGI_FORMAT_UNKNOWN)
                                              ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION)
                                              : position_offset;
    // 蒙皮权重/索引布局由标定结果给出；静态路径无（nullptr）。
    MaskSkinLayout const skin_layout_ref = skin_layout ? *skin_layout : MaskSkinLayout{};
    LayoutKey const key{
        .skinned = skinned,
        .full_prec = desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC),
        .position_format = static_cast<std::uint32_t>(resolved_format),
        .position_offset = resolved_offset,
        .skinning_offset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING),
        .stride = stride,
        .weight_format = static_cast<std::uint32_t>(skin_layout_ref.weight_format),
        .weight_offset = skin_layout_ref.weight_offset,
        .index_format = static_cast<std::uint32_t>(skin_layout_ref.index_format),
        .index_offset = skin_layout_ref.index_offset,
    };

    for (auto const& [cached, layout] : g_layout_cache)
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
        // SKINNING 块内布局由自标定给出（权重/索引的格式与字节偏移），
        // 语义名顺序（BLENDWEIGHT / BLENDINDICES）与蒙皮 VS 一致。
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

    ID3DBlob* blob = skinned ? m_vs_skinned_blob : m_vs_static_blob;
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
    g_layout_cache.emplace_back(key, layout);
    return layout;
}

void MaskGeometryPass::calibrate_upload_orientation(
    RE::NiCamera* camera, DirectX::XMFLOAT4X4 const& view_proj, std::vector<MaskDraw> const& draws,
    std::uint32_t width, std::uint32_t height)
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

    // 引擎像素：左下原点归一化输出，port 为像素单位时先归一化（project_engine 内处理）
    float eng_px = 0.0f;
    float eng_py = 0.0f;
    float eng_depth = 0.0f;
    bool const engine_ok = project(camera, anchor, static_cast<float>(width), static_cast<float>(height), eng_px, eng_py, eng_depth);

    // 直传：局部原点 (0,0,0,1) 的裁剪坐标 = mvp 第 4 列；转置上传：= mvp 第 3 行
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
    // 锚点在相机后方/引擎投影失败：下一帧重试（不置位）
}

void MaskGeometryPass::render(ID3D11Device* device, ID3D11DeviceContext* context, DirectX::XMFLOAT4X4 const& view_proj, std::vector<MaskDraw> const& draws)
{
    for (MaskDraw const& draw : draws)
    {
        if (!draw.vertex_buffer || !draw.index_buffer || draw.index_count == 0 || draw.vertex_stride == 0)
            continue;

        // 蒙皮 draw 传标定布局；静态 draw 传 nullptr（行为与既有完全一致）
        ID3D11InputLayout* layout = get_layout(device, draw.skinned, draw.vertex_desc, draw.vertex_stride,
            draw.position_format, draw.position_offset, draw.skinned ? &draw.skin_layout : nullptr);
        if (!layout)
            continue;

        ID3D11VertexShader* vs = draw.skinned ? m_vs_skinned : m_vs_static;
        if (!vs || !m_ps_mask)
            continue;

        // b0：静态 = ViewProj × 世界变换；调色板蒙皮 = ViewProj（世界变换在调色板里）
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
        {
            DirectX::XMStoreFloat4x4(&cb_data.mvp, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&per_draw)));
        }
        else
        {
            cb_data.mvp = per_draw;
        }
        cb_data.corpse_index = draw.corpse_index;
        update_constant_buffer(context, m_per_draw_cb, &cb_data, sizeof(cb_data));
        context->VSSetConstantBuffers(0, 1, &m_per_draw_cb);

        if (draw.skinned)
        {
            RE::NiSkinInstance* skin = draw.skin.get();

            // 调色板：palette[i] = boneWorld[i] 的 4x4 展开 · skinToBone(i) 的 4x4 展开
            //（两因子相乘次序保持 boneWorld 在前）。
            // 消费约定实证（world-variant）：NiTransform 原样消费（展开不转置）
            // 即引擎语义。引擎蒙皮组合为 skin→bone→world（先 StB 后 BW），其列向量矩阵
            // 为 BW_col·StB_col——与引擎行向量记法 v·StB·BW 的列形式 (StB·BW)ᵀ = BWᵀ·StBᵀ
            // 相一致（M_col(X) = X 原样存储），故两因子相乘的次序保持 boneWorld 在前。
            // 调色板按**全局骨骼索引空间**构建（顶点索引即 skin 骨骼数组下标），
            // P = min(skinData 骨骼数, numMatrices) 为其有效长度；不使用 part.bones
            //（分区局部）。未用槽位填充 palette[P-1] 的副本（防越界读取未定义内容），
            // 整块上传 Max_Palette_Bones 个矩阵。
            std::uint32_t const skin_bones = skin->skinData->GetBoneCount();
            std::uint32_t const matrix_count = skin->numMatrices;
            std::uint32_t const palette_count = palette_slot_count(skin);
            if (palette_count == 0 || palette_count > Max_Palette_Bones)
            {
                log_skinned_skip_once(true, draw.node ? draw.node->name.c_str() : nullptr, "palette slot count out of range",
                    fmt::format("partition={} P={} skin_bones={} numMatrices={} budget={}",
                        draw.partition, palette_count, skin_bones, matrix_count, Max_Palette_Bones));
                continue;
            }

            DirectX::XMFLOAT4X4 palette[Max_Palette_Bones];
            bool palette_ok = true;
            for (std::uint32_t i = 0; i < palette_count; ++i)
            {
                // i < numMatrices 与 i < GetBoneCount() 由 P 的定义保证（防越界读取）
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
            for (std::size_t k = palette_count; k < Max_Palette_Bones; ++k)
                palette[k] = palette[palette_count - 1];
            std::size_t const palette_bytes = Max_Palette_Bones * sizeof(DirectX::XMFLOAT4X4);
            if (m_upload_transposed)
            {
                DirectX::XMFLOAT4X4 palette_upload[Max_Palette_Bones];
                for (std::size_t k = 0; k < Max_Palette_Bones; ++k)
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
        // BSTriShape::vertexCount / 分区 vertices 均为 uint16_t：索引恒为 16 位
        context->IASetIndexBuffer(draw.index_buffer, DXGI_FORMAT_R16_UINT, 0);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(m_ps_mask, nullptr, 0);
        context->DrawIndexed(draw.index_count, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// 全屏消费 pass
// ---------------------------------------------------------------------------

bool FullscreenPass::ensure_common(ID3D11Device* device)
{
    if (common_ready())
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
    // 原实现复用 mask-write 的 blend 描述（WriteMask 已是 ALL）；独立零初始化时
    // 必须显式设置——缺省 0 会禁止所有通道写入，消费 pass 将一个像素都画不出来。
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blend, &m_blend_premul_alpha);

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sampler, &m_sampler);

    D3D11_BUFFER_DESC lcb{};
    lcb.Usage = D3D11_USAGE_DYNAMIC;
    lcb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    lcb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    lcb.ByteWidth = static_cast<UINT>(Alpha_Lut_CB_Bytes);
    device->CreateBuffer(&lcb, nullptr, &m_alpha_lut_cb);

    return common_ready();
}

void FullscreenPass::release()
{
    if (m_alpha_lut_cb)
    {
        m_alpha_lut_cb->Release();
        m_alpha_lut_cb = nullptr;
    }
    if (m_blend_premul_alpha)
    {
        m_blend_premul_alpha->Release();
        m_blend_premul_alpha = nullptr;
    }
    if (m_sampler)
    {
        m_sampler->Release();
        m_sampler = nullptr;
    }
    if (m_vertex_shader)
    {
        m_vertex_shader->Release();
        m_vertex_shader = nullptr;
    }
}

void FullscreenPass::update_alpha_lut(ID3D11DeviceContext* context, float const* lut)
{
    update_constant_buffer(context, m_alpha_lut_cb, lut, Alpha_Lut_CB_Bytes);
}

bool SilhouettePass::ensure(ID3D11Device* device)
{
    if (m_ready || m_failed)
        return m_ready;

    if (!ensure_common(device))
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

    // b0：float4（OutlineColor rgb + Silhouette_Fill_Alpha）
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
    m_failed = false;  // 设备变化后允许重建（与原 release_pipeline → 重建行为一致）
    FullscreenPass::release();
}

bool SilhouettePass::draw(
    ID3D11DeviceContext* context, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* mask_srv,
    std::uint32_t width, std::uint32_t height, Color const& color,
    ID3D11DepthStencilState* depth_none, ID3D11RasterizerState* cull_none)
{
    if (!ready())
        return false;

    // 每帧填充常量缓冲（rgb = OutlineColor 解码，a = 填充系数），单色填充静态/蒙皮剪影。
    float const cb_data[4] = {
        color.r(),
        color.g(),
        color.b(),
        Silhouette_Fill_Alpha,
    };
    return draw_fullscreen_triangle(context, target, mask_srv, width, height,
        m_vertex_shader, m_pixel_shader, m_sampler, m_blend_premul_alpha, depth_none, cull_none,
        m_cb, cb_data, sizeof(cb_data), m_alpha_lut_cb);
}

bool OutlinePass::ensure(ID3D11Device* device)
{
    if (m_ready || m_failed)
        return m_ready;

    if (!ensure_common(device))
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

    // b0：float2 texel + float radius + float pad（16 字节）+ float4 color（16 字节）
    D3D11_BUFFER_DESC ocb{};
    ocb.Usage = D3D11_USAGE_DYNAMIC;
    ocb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ocb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ocb.ByteWidth = 32;
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
    m_failed = false;  // 设备变化后允许重建（与原 release_pipeline → 重建行为一致）
    FullscreenPass::release();
}

bool OutlinePass::draw(
    ID3D11DeviceContext* context, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* mask_srv,
    std::uint32_t width, std::uint32_t height, Color const& color, int thickness,
    ID3D11DepthStencilState* depth_none, ID3D11RasterizerState* cull_none)
{
    if (!ready())
        return false;

    // 每帧填充描边常量缓冲。半径 = clamp(round(thickness), 1, 6)
    //（上限控制 PS 采样数 (2r+1)² ≤ 169）；被 clamp 时一次性 INFO。
    int const thickness_rounded = thickness;
    int const radius = std::clamp(thickness_rounded, 1, static_cast<int>(Max_Outline_Radius));
    static bool s_radius_clamp_reported = false;
    if (!s_radius_clamp_reported &&
        (thickness_rounded < 1 || thickness_rounded > static_cast<int>(Max_Outline_Radius)))
    {
        s_radius_clamp_reported = true;
        logger::info("outline mask: outline thickness {} clamped to {} pixels (max keeps the dilate pass at {} taps)",
            thickness_rounded, radius, (2 * Max_Outline_Radius + 1) * (2 * Max_Outline_Radius + 1));
    }
    OutlineCBData const cb_data{
        .texel_x = 1.0f / static_cast<float>(width),
        .texel_y = 1.0f / static_cast<float>(height),
        .radius = static_cast<float>(radius),
        .pad = 0.0f,
        .color_r = color.r(),
        .color_g = color.g(),
        .color_b = color.b(),
        .color_a = Outline_Alpha,
    };
    return draw_fullscreen_triangle(context, target, mask_srv, width, height,
        m_vertex_shader, m_pixel_shader, m_sampler, m_blend_premul_alpha, depth_none, cull_none,
        m_cb, &cb_data, sizeof(cb_data), m_alpha_lut_cb);
}

MASK_NAMESPACE_END
