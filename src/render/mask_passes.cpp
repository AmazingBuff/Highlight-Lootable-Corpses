//
// Created by AmazingBuff on 2026/9/13.
//

#include "mask_passes.h"

#include "mask_geometry.h"
#include "render/shader_sources.h"
#include "screen_projector.h"
#include "d3d11_util.h"

#include <RE/Skyrim.h>

#include <algorithm>
#include <cstring>

PLUGIN_NAMESPACE_BEGIN

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
        MaskMat4 mvp;        // 静态=ViewProj*World，蒙皮=ViewProj；上传经 oriented()
        float corpse_index;  // 目标序号/255
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
        ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, ID3D11ShaderResourceView* a_mask_srv,
        std::uint32_t a_width, std::uint32_t a_height,
        ID3D11VertexShader* a_vs, ID3D11PixelShader* a_ps, ID3D11SamplerState* a_sampler, ID3D11BlendState* a_blend,
        ID3D11DepthStencilState* a_depth_none, ID3D11RasterizerState* a_cull_none,
        ID3D11Buffer* a_cb0, void const* a_cb0_data, std::size_t a_cb0_bytes,
        ID3D11Buffer* a_alpha_lut_cb)
    {
        if (!a_target || !a_mask_srv || !a_vs || !a_ps || !a_sampler || !a_blend || !a_cb0 || !a_alpha_lut_cb)
            return false;

        D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(a_width), static_cast<float>(a_height), 0.0f, 1.0f };
        a_context->OMSetRenderTargets(1, &a_target, nullptr);
        a_context->OMSetBlendState(a_blend, nullptr, 0xFFFFFFFF);
        a_context->OMSetDepthStencilState(a_depth_none, 0);
        a_context->RSSetState(a_cull_none);
        a_context->RSSetViewports(1, &vp);
        a_context->IASetInputLayout(nullptr);  // SV_VertexID 全屏三角形，无需布局
        a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* const no_vb = nullptr;
        UINT const zero = 0;
        a_context->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
        a_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        a_context->VSSetShader(a_vs, nullptr, 0);
        a_context->PSSetShader(a_ps, nullptr, 0);
        a_context->PSSetShaderResources(0, 1, &a_mask_srv);
        a_context->PSSetSamplers(0, 1, &a_sampler);

        ID3D11Buffer* prev_ps_cbs[2] = {};
        a_context->PSGetConstantBuffers(0, 2, prev_ps_cbs);
        ID3D11Buffer* const ps_cbs[2] = { a_cb0, a_alpha_lut_cb };
        update_constant_buffer(a_context, a_cb0, a_cb0_data, a_cb0_bytes);
        a_context->PSSetConstantBuffers(0, 2, ps_cbs);

        a_context->Draw(3, 0);
        a_context->PSSetConstantBuffers(0, 2, prev_ps_cbs);
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

bool MaskRenderTarget::matches(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height) const
{
    return m_device == a_device && m_width == a_width && m_height == a_height && m_srv;
}

void MaskRenderTarget::release_views()
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
    m_device = nullptr;
}

void MaskRenderTarget::release()
{
    release_views();
}

bool MaskRenderTarget::ensure(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height)
{
    if (matches(a_device, a_width, a_height))
        return true;

    release_views();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = a_width;
    td.Height = a_height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT const tex_hr = a_device->CreateTexture2D(&td, nullptr, &m_texture);
    if (FAILED(tex_hr) || !m_texture)
    {
        logger::error("outline mask: failed to create mask texture ({:X})", static_cast<unsigned int>(tex_hr));
        release_views();
        return false;
    }
    HRESULT const rtv_hr = a_device->CreateRenderTargetView(m_texture, nullptr, &m_rtv);
    HRESULT const srv_hr = a_device->CreateShaderResourceView(m_texture, nullptr, &m_srv);
    if (FAILED(rtv_hr) || !m_rtv || FAILED(srv_hr) || !m_srv)
    {
        logger::error("outline mask: failed to create mask views (rtv={:X}, srv={:X})", static_cast<unsigned int>(rtv_hr), static_cast<unsigned int>(srv_hr));
        release_views();
        return false;
    }

    m_device = a_device;
    m_width = a_width;
    m_height = a_height;
    return true;
}

// ---------------------------------------------------------------------------
// MaskGeometryPass
// ---------------------------------------------------------------------------

bool MaskGeometryPass::ensure(ID3D11Device* a_device)
{
    if (m_ready || m_failed)
        return m_ready;

    m_ready = create_pipeline(a_device);
    if (!m_ready)
    {
        m_failed = true;
        release();
        logger::error("outline mask pipeline creation failed, mask rendering disabled");
    }
    return m_ready;
}

bool MaskGeometryPass::create_pipeline(ID3D11Device* a_device)
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

    a_device->CreateVertexShader(m_vs_static_blob->GetBufferPointer(), m_vs_static_blob->GetBufferSize(), nullptr, &m_vs_static);
    a_device->CreateVertexShader(m_vs_skinned_blob->GetBufferPointer(), m_vs_skinned_blob->GetBufferSize(), nullptr, &m_vs_skinned);
    a_device->CreatePixelShader(ps_mask_blob->GetBufferPointer(), ps_mask_blob->GetBufferSize(), nullptr, &m_ps_mask);
    ps_mask_blob->Release();

    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = 80;  // PerDrawCBData：Mat4 + corpse_index + pad[3]
    a_device->CreateBuffer(&cb, nullptr, &m_per_draw_cb);
    cb.ByteWidth = static_cast<UINT>(Palette_CB_Bytes);
    a_device->CreateBuffer(&cb, nullptr, &m_palette_cb);

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
    a_device->CreateBlendState(&blend, &m_blend_mask_write);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;  // 深度测试关闭 —— mask 穿墙
    a_device->CreateDepthStencilState(&depth, &m_depth_disabled);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;  // 剪影不受三角形绕序影响
    raster.DepthClipEnable = FALSE;
    a_device->CreateRasterizerState(&raster, &m_rasterizer);

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
    ID3D11Device* a_device, bool a_skinned, RE::BSGraphics::VertexDesc const& a_desc, std::uint32_t a_stride,
    DXGI_FORMAT a_position_format, std::uint32_t a_position_offset, MaskSkinLayout const* a_skin_layout)
{
    // 位置格式/偏移：静态路径为标定结果（UNKNOWN 表示按 desc 推导）；蒙皮路径为
    // 属性偏移间距判定结果（绝不为 UNKNOWN）。
    DXGI_FORMAT const resolved_format = (a_position_format == DXGI_FORMAT_UNKNOWN)
                                            ? (a_desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT)
                                            : a_position_format;
    std::uint32_t const resolved_offset = (a_position_format == DXGI_FORMAT_UNKNOWN)
                                              ? a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION)
                                              : a_position_offset;
    // 蒙皮权重/索引布局由标定结果给出；静态路径无（nullptr）。
    MaskSkinLayout const skin_layout = a_skin_layout ? *a_skin_layout : MaskSkinLayout{};
    LayoutKey const key{
        .skinned = a_skinned,
        .full_prec = a_desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC),
        .position_format = static_cast<std::uint32_t>(resolved_format),
        .position_offset = resolved_offset,
        .skinning_offset = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING),
        .stride = a_stride,
        .weight_format = static_cast<std::uint32_t>(skin_layout.weight_format),
        .weight_offset = skin_layout.weight_offset,
        .index_format = static_cast<std::uint32_t>(skin_layout.index_format),
        .index_offset = skin_layout.index_offset,
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
    if (a_skinned)
    {
        // SKINNING 块内布局由自标定给出（权重/索引的格式与字节偏移），
        // 语义名顺序（BLENDWEIGHT / BLENDINDICES）与蒙皮 VS 一致。
        elements[count++] = {
            .SemanticName = "BLENDWEIGHT",
            .SemanticIndex = 0,
            .Format = skin_layout.weight_format,
            .InputSlot = 0,
            .AlignedByteOffset = skin_layout.weight_offset,
            .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
            .InstanceDataStepRate = 0
        };
        elements[count++] = {
            .SemanticName = "BLENDINDICES",
            .SemanticIndex = 0,
            .Format = skin_layout.index_format,
            .InputSlot = 0,
            .AlignedByteOffset = skin_layout.index_offset,
            .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
            .InstanceDataStepRate = 0
        };
    }

    ID3DBlob* blob = a_skinned ? m_vs_skinned_blob : m_vs_static_blob;
    ID3D11InputLayout* layout = nullptr;
    HRESULT const hr = a_device->CreateInputLayout(elements, count, blob->GetBufferPointer(), blob->GetBufferSize(), &layout);
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
    RE::NiCamera* a_camera, MaskMat4 const& a_view_proj, std::vector<MaskDraw> const& a_draws,
    std::uint32_t a_width, std::uint32_t a_height)
{
    static bool s_checked = false;
    if (s_checked || a_draws.empty() || !a_draws.front().node)
        return;

    RE::NiPoint3 const anchor = a_draws.front().node->world.translate;
    MaskMat4 const model = MaskMat4::from_transform(a_draws.front().node->world);
    MaskMat4 const mvp = a_view_proj * model;

    // 引擎像素：左下原点归一化输出，port 为像素单位时先归一化（project_engine 内处理）
    float eng_px = 0.0f;
    float eng_py = 0.0f;
    float eng_depth = 0.0f;
    bool const engine_ok = project_engine(a_camera, anchor, static_cast<float>(a_width), static_cast<float>(a_height), eng_px, eng_py, eng_depth);

    // 直传：局部原点 (0,0,0,1) 的裁剪坐标 = mvp 第 4 列；转置上传：= mvp 第 3 行
    float const cw = mvp.m[3][3];
    if (engine_ok && cw > 1e-5f)
    {
        s_checked = true;
        float const w = static_cast<float>(a_width);
        float const h = static_cast<float>(a_height);
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

void MaskGeometryPass::render(ID3D11Device* a_device, ID3D11DeviceContext* a_context, MaskMat4 const& a_view_proj, std::vector<MaskDraw> const& a_draws)
{
    for (MaskDraw const& draw : a_draws)
    {
        if (!draw.vertex_buffer || !draw.index_buffer || draw.index_count == 0 || draw.vertex_stride == 0)
            continue;

        // 蒙皮 draw 传标定布局；静态 draw 传 nullptr（行为与既有完全一致）
        ID3D11InputLayout* layout = get_layout(a_device, draw.skinned, draw.vertex_desc, draw.vertex_stride,
            draw.position_format, draw.position_offset, draw.skinned ? &draw.skin_layout : nullptr);
        if (!layout)
            continue;

        ID3D11VertexShader* vs = draw.skinned ? m_vs_skinned : m_vs_static;
        if (!vs || !m_ps_mask)
            continue;

        // b0：静态 = ViewProj × 世界变换；调色板蒙皮 = ViewProj（世界变换在调色板里）
        MaskMat4 const per_draw = draw.skinned ? a_view_proj : a_view_proj * MaskMat4::from_transform(draw.node->world);
        PerDrawCBData cb_data{};
        cb_data.mvp = m_upload_transposed ? per_draw.transposed() : per_draw;
        cb_data.corpse_index = draw.corpse_index;
        update_constant_buffer(a_context, m_per_draw_cb, &cb_data, sizeof(cb_data));
        a_context->VSSetConstantBuffers(0, 1, &m_per_draw_cb);

        if (draw.skinned)
        {
            RE::NiSkinInstance* skin = draw.skin.get();

            // 调色板：palette[i] = from_transform(boneWorld[i]) × from_transform(skinToBone(i))。
            // 消费约定实证（world-variant）：NiTransform 原样消费（from_transform 不转置）
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

            MaskMat4 palette[Max_Palette_Bones];
            bool palette_ok = true;
            for (std::uint32_t i = 0; i < palette_count; ++i)
            {
                // i < numMatrices 与 i < GetBoneCount() 由 P 的定义保证（防越界读取）
                if (!skin->boneWorldTransforms[i])
                {
                    palette_ok = false;
                    break;
                }
                palette[i] = MaskMat4::from_transform(*skin->boneWorldTransforms[i]) *
                             MaskMat4::from_transform(skin->skinData->GetBoneDataSkinToBone(i));
            }
            if (!palette_ok)
            {
                log_skinned_skip_once(true, draw.node ? draw.node->name.c_str() : nullptr, "null bone world transform in palette range",
                    fmt::format("partition={} P={} skin_bones={} numMatrices={}", draw.partition, palette_count, skin_bones, matrix_count));
                continue;
            }
            for (std::size_t k = palette_count; k < Max_Palette_Bones; ++k)
                palette[k] = palette[palette_count - 1];
            std::size_t const palette_bytes = Max_Palette_Bones * sizeof(MaskMat4);
            if (m_upload_transposed)
            {
                MaskMat4 palette_upload[Max_Palette_Bones];
                for (std::size_t k = 0; k < Max_Palette_Bones; ++k)
                    palette_upload[k] = palette[k].transposed();
                update_constant_buffer(a_context, m_palette_cb, palette_upload, palette_bytes);
            }
            else
            {
                update_constant_buffer(a_context, m_palette_cb, palette, palette_bytes);
            }
            a_context->VSSetConstantBuffers(1, 1, &m_palette_cb);
        }

        a_context->IASetInputLayout(layout);
        a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT const stride = draw.vertex_stride;
        UINT const offset = 0;
        ID3D11Buffer* vb = draw.vertex_buffer;
        a_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        // BSTriShape::vertexCount / 分区 vertices 均为 uint16_t：索引恒为 16 位
        a_context->IASetIndexBuffer(draw.index_buffer, DXGI_FORMAT_R16_UINT, 0);
        a_context->VSSetShader(vs, nullptr, 0);
        a_context->PSSetShader(m_ps_mask, nullptr, 0);
        a_context->DrawIndexed(draw.index_count, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// 全屏消费 pass
// ---------------------------------------------------------------------------

bool FullscreenPass::ensure_common(ID3D11Device* a_device)
{
    if (common_ready())
        return true;

    ID3DBlob* vs_blob = compile_shader(render_shaders::MaskComposite, "vs_main", "vs_5_0", "outline mask fullscreen", "outline mask");
    if (vs_blob)
    {
        a_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vertex_shader);
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
    a_device->CreateBlendState(&blend, &m_blend_premul_alpha);

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    a_device->CreateSamplerState(&sampler, &m_sampler);

    D3D11_BUFFER_DESC lcb{};
    lcb.Usage = D3D11_USAGE_DYNAMIC;
    lcb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    lcb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    lcb.ByteWidth = static_cast<UINT>(Alpha_Lut_CB_Bytes);
    a_device->CreateBuffer(&lcb, nullptr, &m_alpha_lut_cb);

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

void FullscreenPass::update_alpha_lut(ID3D11DeviceContext* a_context, float const* a_lut)
{
    update_constant_buffer(a_context, m_alpha_lut_cb, a_lut, Alpha_Lut_CB_Bytes);
}

bool SilhouettePass::ensure(ID3D11Device* a_device)
{
    if (m_ready || m_failed)
        return m_ready;

    if (!ensure_common(a_device))
    {
        m_failed = true;
        return false;
    }

    ID3DBlob* ps_blob = compile_shader(render_shaders::MaskComposite, "ps_silhouette_main", "ps_5_0", "outline mask silhouette", "outline mask");
    if (ps_blob)
    {
        a_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_pixel_shader);
        ps_blob->Release();
    }

    // b0：float4（OutlineColor rgb + Silhouette_Fill_Alpha）
    D3D11_BUFFER_DESC ccb{};
    ccb.Usage = D3D11_USAGE_DYNAMIC;
    ccb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ccb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ccb.ByteWidth = 16;
    a_device->CreateBuffer(&ccb, nullptr, &m_cb);

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
    ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, ID3D11ShaderResourceView* a_mask_srv,
    std::uint32_t a_width, std::uint32_t a_height, Color const& a_color,
    ID3D11DepthStencilState* a_depth_none, ID3D11RasterizerState* a_cull_none)
{
    if (!ready())
        return false;

    // 每帧填充常量缓冲（rgb = OutlineColor 解码，a = 填充系数），单色填充静态/蒙皮剪影。
    float const cb_data[4] = {
        a_color.r(),
        a_color.g(),
        a_color.b(),
        Silhouette_Fill_Alpha,
    };
    return draw_fullscreen_triangle(a_context, a_target, a_mask_srv, a_width, a_height,
        m_vertex_shader, m_pixel_shader, m_sampler, m_blend_premul_alpha, a_depth_none, a_cull_none,
        m_cb, cb_data, sizeof(cb_data), m_alpha_lut_cb);
}

bool OutlinePass::ensure(ID3D11Device* a_device)
{
    if (m_ready || m_failed)
        return m_ready;

    if (!ensure_common(a_device))
    {
        m_failed = true;
        return false;
    }

    ID3DBlob* ps_blob = compile_shader(render_shaders::MaskComposite, "ps_outline_main", "ps_5_0", "outline mask outline", "outline mask");
    if (ps_blob)
    {
        a_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_pixel_shader);
        ps_blob->Release();
    }

    // b0：float2 texel + float radius + float pad（16 字节）+ float4 color（16 字节）
    D3D11_BUFFER_DESC ocb{};
    ocb.Usage = D3D11_USAGE_DYNAMIC;
    ocb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ocb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ocb.ByteWidth = 32;
    a_device->CreateBuffer(&ocb, nullptr, &m_cb);

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
    ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, ID3D11ShaderResourceView* a_mask_srv,
    std::uint32_t a_width, std::uint32_t a_height, Color const& a_color, int a_thickness,
    ID3D11DepthStencilState* a_depth_none, ID3D11RasterizerState* a_cull_none)
{
    if (!ready())
        return false;

    // 每帧填充描边常量缓冲。半径 = clamp(round(thickness), 1, 6)
    //（上限控制 PS 采样数 (2r+1)² ≤ 169）；被 clamp 时一次性 INFO。
    int const thickness_rounded = a_thickness;
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
        .texel_x = 1.0f / static_cast<float>(a_width),
        .texel_y = 1.0f / static_cast<float>(a_height),
        .radius = static_cast<float>(radius),
        .pad = 0.0f,
        .color_r = a_color.r(),
        .color_g = a_color.g(),
        .color_b = a_color.b(),
        .color_a = Outline_Alpha,
    };
    return draw_fullscreen_triangle(a_context, a_target, a_mask_srv, a_width, a_height,
        m_vertex_shader, m_pixel_shader, m_sampler, m_blend_premul_alpha, a_depth_none, a_cull_none,
        m_cb, &cb_data, sizeof(cb_data), m_alpha_lut_cb);
}

PLUGIN_NAMESPACE_END
