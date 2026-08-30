//
// Created by AmazingBuff on 2026/08/20.
//

#include "pch.h"
#include "mesh_outline.h"

#include "shader_compile.h"

#include <string>

namespace
{
    // SSE 蒙皮分区的骨骼上限；常量缓冲按此预留（每骨 3 行 float4）
    constexpr std::uint32_t Max_Palette_Bones = 80;
    constexpr std::uint32_t Max_Dilate_Radius = 8;

    // ---------------------------------------------------------------------------
    // 采集（游戏线程）
    //
    // 部件 = 一次 DrawIndexed 所需的全部信息。引擎对象一律用 NiPointer 保活：
    // - geometry：其析构会释放 rendererData，故刚体部件靠它保住 GPU 缓冲；
    // - partition：蒙皮分区的 GPU 缓冲（buffData）属于它；
    // - bones：绘制当帧读取 world 变换，必须保证节点存活。
    // ---------------------------------------------------------------------------
    struct Part
    {
        bool skinned{ false };
        bool full_precision{ false };
        std::uint32_t stride{ 0 };
        std::uint32_t index_count{ 0 };
        std::uint32_t skin_offset{ 0 };  // 蒙皮属性在顶点内的字节偏移（作为第二个 IA 槽的缓冲偏移）
        RE::NiPointer<RE::NiAVObject> geometry;
        RE::NiPointer<RE::NiSkinPartition> partition;
        RE::BSGraphics::TriShape* buffer{ nullptr };
        std::vector<RE::NiPointer<RE::NiAVObject>> bones;
        std::vector<RE::NiTransform> skin_to_bone;
    };

    // 顶点步长：位置按 VF_FULLPREC 取 float4 / half4，其余属性宽度固定。
    // 不用 VertexDesc::GetSize()——它对位置恒按 float4 计，缺 VF_FULLPREC 时会多算 8 字节。
    [[nodiscard]] std::uint32_t vertex_stride(RE::BSGraphics::VertexDesc const& a_desc)
    {
        using Vertex = RE::BSGraphics::Vertex;

        std::uint32_t stride = 0;
        if (a_desc.HasFlag(Vertex::VF_VERTEX))
            stride += a_desc.HasFlag(Vertex::VF_FULLPREC) ? 16u : 8u;
        if (a_desc.HasFlag(Vertex::VF_UV))
            stride += 4u;
        if (a_desc.HasFlag(Vertex::VF_UV_2))
            stride += 4u;
        if (a_desc.HasFlag(Vertex::VF_NORMAL))
        {
            stride += 4u;
            if (a_desc.HasFlag(Vertex::VF_TANGENT))
                stride += 4u;
        }
        if (a_desc.HasFlag(Vertex::VF_COLORS))
            stride += 4u;
        if (a_desc.HasFlag(Vertex::VF_SKINNED))
            stride += 12u;  // 4 个 half 权重 + 4 个 uint8 骨骼索引
        if (a_desc.HasFlag(Vertex::VF_EYEDATA))
            stride += 4u;

        return stride;
    }

    // 只接受可按三角形拓扑安全绘制的类型（粒子/线段等拓扑不同，按三角形画会得到垃圾）
    [[nodiscard]] bool is_drawable_type(RE::BSGeometry* a_geometry)
    {
        switch (a_geometry->GetType().get())
        {
        case RE::BSGeometry::Type::kTriShape:
        case RE::BSGeometry::Type::kDynamicTriShape:
        case RE::BSGeometry::Type::kSubIndexTriShape:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool has_position(RE::BSGraphics::VertexDesc const& a_desc)
    {
        return a_desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX);
    }

    void collect_rigid(RE::BSGeometry* a_geometry, std::vector<Part>& a_parts)
    {
        RE::BSGraphics::TriShape* const buffer = a_geometry->GetGeometryRuntimeData().rendererData;
        if (!buffer || !buffer->vertexBuffer || !buffer->indexBuffer || !has_position(buffer->vertexDesc))
            return;

        std::uint32_t const indices = static_cast<std::uint32_t>(static_cast<RE::BSTriShape*>(a_geometry)->GetTrishapeRuntimeData().triangleCount) * 3u;
        std::uint32_t const stride = vertex_stride(buffer->vertexDesc);
        if (indices == 0 || stride == 0)
            return;

        Part part;
        part.full_precision = buffer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
        part.stride = stride;
        part.index_count = indices;
        part.geometry = RE::NiPointer<RE::NiAVObject>(a_geometry);
        part.buffer = buffer;
        a_parts.push_back(std::move(part));
    }

    void collect_skinned(RE::BSGeometry* a_geometry, RE::NiSkinInstance* a_skin, std::vector<Part>& a_parts)
    {
        RE::NiSkinPartition* const partition = a_skin->skinPartition.get();
        RE::NiSkinData* const data = a_skin->skinData.get();
        if (!partition || !data || !a_skin->bones)
            return;

        RE::NiSkinPartition::Partition const* const entries = partition->partitions.data();
        if (!entries)
            return;

        std::uint32_t const bone_count = data->GetBoneCount();
        for (std::uint32_t i = 0; i < partition->numPartitions; ++i)
        {
            RE::NiSkinPartition::Partition const& source = entries[i];
            if (!source.buffData || !source.buffData->vertexBuffer || !source.buffData->indexBuffer)
                continue;
            if (!source.bones || source.numBones == 0 || source.numBones > Max_Palette_Bones || source.triangles == 0)
                continue;
            if (!has_position(source.vertexDesc) || !source.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED))
                continue;

            std::uint32_t const stride = vertex_stride(source.vertexDesc);
            if (stride == 0)
                continue;

            Part part;
            part.skinned = true;
            part.full_precision = source.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
            part.stride = stride;
            part.index_count = static_cast<std::uint32_t>(source.triangles) * 3u;
            part.skin_offset = source.vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
            part.geometry = RE::NiPointer<RE::NiAVObject>(a_geometry);
            part.partition = RE::NiPointer<RE::NiSkinPartition>(partition);
            part.buffer = source.buffData;
            part.bones.reserve(source.numBones);
            part.skin_to_bone.reserve(source.numBones);

            bool complete = true;
            for (std::uint16_t bone = 0; bone < source.numBones; ++bone)
            {
                std::uint16_t const skin_index = source.bones[bone];
                RE::NiAVObject* const node = skin_index < bone_count ? a_skin->bones[skin_index] : nullptr;
                if (!node)
                {
                    complete = false;
                    break;
                }
                part.bones.emplace_back(node);
                part.skin_to_bone.push_back(data->GetBoneDataSkinToBone(skin_index));
            }

            if (complete)
                a_parts.push_back(std::move(part));
        }
    }

    void collect_node(RE::NiAVObject* a_node, std::vector<Part>& a_parts)
    {
        if (!a_node || a_node->flags.any(RE::NiAVObject::Flag::kHidden))
            return;

        if (RE::BSGeometry* const geometry = a_node->AsGeometry())
        {
            if (is_drawable_type(geometry))
            {
                if (RE::NiSkinInstance* const skin = geometry->GetGeometryRuntimeData().skinInstance.get())
                    collect_skinned(geometry, skin, a_parts);
                else
                    collect_rigid(geometry, a_parts);
            }
        }
        if (RE::NiNode* const node = a_node->AsNode())
        {
            for (RE::NiPointer<RE::NiAVObject> const& child : node->children)
            {
                if (child)
                    collect_node(child.get(), a_parts);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // 着色器（运行时编译，与 esp_renderer 的自绘管线同款约定）
    //
    // b0 里的矩阵是调用方透传的 NiCamera::worldToCam——引擎的世界->裁剪
    // view-projection 矩阵（行主序：clip = M * p；w 行是前向距离，故相机背后几何
    // 由 w <= 0 裁掉；反 Z 的 z 行与本管线无关，因为深度写死 z = w*0.5）。
    // 注意：worldToCam 不是仿射视图矩阵，而是完整的世界->裁剪矩阵，直接可用。
    // ---------------------------------------------------------------------------
    constexpr char const* Mask_Common_Hlsl = R"(
        cbuffer MaskCB : register(b0)
        {
            row_major float4x4 g_world_to_cam;
            float4 g_params;          // x = alpha
            float4 g_rows[240];       // 每骨 3 行 float4（3x4 矩阵）；刚体部件用 [0..2]
        };

        float3 transform_rows(uint a_base, float3 a_position)
        {
            float4 r0 = g_rows[a_base + 0];
            float4 r1 = g_rows[a_base + 1];
            float4 r2 = g_rows[a_base + 2];
            return float3(
                dot(r0.xyz, a_position) + r0.w,
                dot(r1.xyz, a_position) + r1.w,
                dot(r2.xyz, a_position) + r2.w);
        }

        float4 to_clip(float3 a_world)
        {
            float4 clip = mul(g_world_to_cam, float4(a_world, 1.0f));
            return float4(clip.x, clip.y, clip.w * 0.5f, clip.w);
        }
    )";

    constexpr char const* Mask_Rigid_Vs_Hlsl = R"(
        float4 main(float4 position : POSITION) : SV_Position
        {
            return to_clip(transform_rows(0u, position.xyz));
        }
    )";

    constexpr char const* Mask_Skinned_Vs_Hlsl = R"(
        float4 main(float4 position : POSITION, float4 weights : BLENDWEIGHT, uint4 indices : BLENDINDICES) : SV_Position
        {
            float3 skinned = float3(0.0f, 0.0f, 0.0f);
            float total = 0.0f;
            [unroll]
            for (int i = 0; i < 4; ++i)
            {
                float weight = weights[i];
                uint base = min(indices[i], 79u) * 3u;
                skinned += weight * transform_rows(base, position.xyz);
                total += weight;
            }
            if (total > 1e-4f)
                skinned /= total;

            return to_clip(skinned);
        }
    )";

    constexpr char const* Mask_Ps_Hlsl = R"(
        float main(float4 position : SV_Position) : SV_Target
        {
            return g_params.x;
        }
    )";

    constexpr char const* Fullscreen_Vs_Hlsl = R"(
        void main(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
        {
            float2 corner = float2((id == 2u) ? 3.0f : -1.0f, (id == 1u) ? 3.0f : -1.0f);
            position = float4(corner, 0.5f, 1.0f);
            uv = float2(corner.x * 0.5f + 0.5f, 0.5f - corner.y * 0.5f);
        }
    )";

    constexpr char const* Edge_Common_Hlsl = R"(
        cbuffer EdgeCB : register(b0)
        {
            float4 g_texel;   // xy = 1/尺寸，z = 膨胀半径（像素）
            float4 g_color;   // rgb = 描边颜色
        };

        Texture2D<float> g_mask : register(t0);
        Texture2D<float> g_dilated : register(t1);
        SamplerState g_sampler : register(s0);
    )";

    // 可分离膨胀第一趟：横向 max
    constexpr char const* Dilate_Ps_Hlsl = R"(
        float main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
        {
            int radius = (int)g_texel.z;
            float value = 0.0f;
            for (int i = -radius; i <= radius; ++i)
                value = max(value, g_mask.SampleLevel(g_sampler, uv + float2(i * g_texel.x, 0.0f), 0));

            return value;
        }
    )";

    // 第二趟：纵向 max + 边缘判定（遮罩内部丢弃 -> 描边落在剪影外沿一圈）
    constexpr char const* Outline_Ps_Hlsl = R"(
        float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
        {
            if (g_mask.SampleLevel(g_sampler, uv, 0) > 0.004f)
                discard;

            int radius = (int)g_texel.z;
            float value = 0.0f;
            for (int i = -radius; i <= radius; ++i)
                value = max(value, g_dilated.SampleLevel(g_sampler, uv + float2(0.0f, i * g_texel.y), 0));

            if (value <= 0.004f)
                discard;

            return float4(g_color.rgb * value, value);
        }
    )";

    struct MaskConstants
    {
        float world_to_cam[16];
        float params[4];
        float rows[Max_Palette_Bones * 3][4];
    };
    static_assert(sizeof(MaskConstants) % 16 == 0);

    struct EdgeConstants
    {
        float texel[4];
        float color[4];
    };

    ID3D11VertexShader* g_rigid_vs = nullptr;
    ID3D11VertexShader* g_skinned_vs = nullptr;
    ID3D11PixelShader* g_mask_ps = nullptr;
    ID3D11VertexShader* g_fullscreen_vs = nullptr;
    ID3D11PixelShader* g_dilate_ps = nullptr;
    ID3D11PixelShader* g_outline_ps = nullptr;
    ID3D11InputLayout* g_layouts[4] = {};
    ID3D11Buffer* g_mask_cb = nullptr;
    ID3D11Buffer* g_edge_cb = nullptr;
    ID3D11BlendState* g_max_blend = nullptr;
    ID3D11BlendState* g_opaque_blend = nullptr;
    ID3D11BlendState* g_alpha_blend = nullptr;
    ID3D11DepthStencilState* g_depth_none = nullptr;
    ID3D11RasterizerState* g_cull_none = nullptr;
    ID3D11SamplerState* g_point_clamp = nullptr;

    ID3D11Texture2D* g_mask_texture = nullptr;
    ID3D11RenderTargetView* g_mask_rtv = nullptr;
    ID3D11ShaderResourceView* g_mask_srv = nullptr;
    ID3D11Texture2D* g_dilate_texture = nullptr;
    ID3D11RenderTargetView* g_dilate_rtv = nullptr;
    ID3D11ShaderResourceView* g_dilate_srv = nullptr;

    std::uint32_t g_width = 0;
    std::uint32_t g_height = 0;
    bool g_pipeline_ready = false;
    bool g_failed = false;
    std::uint32_t g_draw_count = 0;
    D3D11_VIEWPORT g_saved_viewport{};
    UINT g_saved_viewport_count = 0;

    [[nodiscard]] std::size_t layout_index(bool a_skinned, bool a_full_precision)
    {
        return (a_skinned ? 2u : 0u) + (a_full_precision ? 1u : 0u);
    }

    template <class T>
    void release_object(T*& a_object)
    {
        if (a_object)
        {
            a_object->Release();
            a_object = nullptr;
        }
    }

    void release_targets()
    {
        release_object(g_mask_srv);
        release_object(g_mask_rtv);
        release_object(g_mask_texture);
        release_object(g_dilate_srv);
        release_object(g_dilate_rtv);
        release_object(g_dilate_texture);
        g_width = 0;
        g_height = 0;
    }

    void release_pipeline()
    {
        release_object(g_rigid_vs);
        release_object(g_skinned_vs);
        release_object(g_mask_ps);
        release_object(g_fullscreen_vs);
        release_object(g_dilate_ps);
        release_object(g_outline_ps);
        for (ID3D11InputLayout*& layout : g_layouts)
            release_object(layout);
        release_object(g_mask_cb);
        release_object(g_edge_cb);
        release_object(g_max_blend);
        release_object(g_opaque_blend);
        release_object(g_alpha_blend);
        release_object(g_depth_none);
        release_object(g_cull_none);
        release_object(g_point_clamp);
        g_pipeline_ready = false;
    }

    [[nodiscard]] bool create_layouts(ID3D11Device* a_device, ID3DBlob* a_rigid_blob, ID3DBlob* a_skinned_blob)
    {
        // 位置固定在槽 0 偏移 0；蒙皮属性通过"同一缓冲绑到槽 1 + 运行时缓冲偏移"供给，
        // 这样每个精度只需一套固定偏移的布局，不必为每种顶点布局各建一个。
        for (std::uint32_t full = 0; full < 2; ++full)
        {
            DXGI_FORMAT const position_format = full != 0 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;

            D3D11_INPUT_ELEMENT_DESC const rigid[] = {
                { .SemanticName = "POSITION", .SemanticIndex = 0, .Format = position_format, .InputSlot = 0, .AlignedByteOffset = 0, .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA, .InstanceDataStepRate = 0 },
            };
            D3D11_INPUT_ELEMENT_DESC const skinned[] = {
                { .SemanticName = "POSITION", .SemanticIndex = 0, .Format = position_format, .InputSlot = 0, .AlignedByteOffset = 0, .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA, .InstanceDataStepRate = 0 },
                { .SemanticName = "BLENDWEIGHT", .SemanticIndex = 0, .Format = DXGI_FORMAT_R16G16B16A16_FLOAT, .InputSlot = 1, .AlignedByteOffset = 0, .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA, .InstanceDataStepRate = 0 },
                { .SemanticName = "BLENDINDICES", .SemanticIndex = 0, .Format = DXGI_FORMAT_R8G8B8A8_UINT, .InputSlot = 1, .AlignedByteOffset = 8, .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA, .InstanceDataStepRate = 0 },
            };

            if (FAILED(a_device->CreateInputLayout(rigid, 1, a_rigid_blob->GetBufferPointer(), a_rigid_blob->GetBufferSize(), &g_layouts[layout_index(false, full != 0)])))
                return false;
            if (FAILED(a_device->CreateInputLayout(skinned, 3, a_skinned_blob->GetBufferPointer(), a_skinned_blob->GetBufferSize(), &g_layouts[layout_index(true, full != 0)])))
                return false;
        }
        return true;
    }

    [[nodiscard]] bool create_states(ID3D11Device* a_device)
    {
        D3D11_BLEND_DESC max_desc{};
        max_desc.RenderTarget[0].BlendEnable = TRUE;
        max_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        max_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        max_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
        max_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        max_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        max_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
        max_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(a_device->CreateBlendState(&max_desc, &g_max_blend)))
            return false;

        D3D11_BLEND_DESC opaque_desc{};
        opaque_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(a_device->CreateBlendState(&opaque_desc, &g_opaque_blend)))
            return false;

        // 预乘 alpha（与 esp_renderer 的 quad 管线同约定）
        D3D11_BLEND_DESC alpha_desc{};
        alpha_desc.RenderTarget[0].BlendEnable = TRUE;
        alpha_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        alpha_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        alpha_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        alpha_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        alpha_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        alpha_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        alpha_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(a_device->CreateBlendState(&alpha_desc, &g_alpha_blend)))
            return false;

        D3D11_DEPTH_STENCIL_DESC depth_desc{};
        depth_desc.DepthEnable = FALSE;
        depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depth_desc.StencilEnable = FALSE;
        if (FAILED(a_device->CreateDepthStencilState(&depth_desc, &g_depth_none)))
            return false;

        D3D11_RASTERIZER_DESC raster_desc{};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_NONE;
        raster_desc.DepthClipEnable = TRUE;
        if (FAILED(a_device->CreateRasterizerState(&raster_desc, &g_cull_none)))
            return false;

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        return SUCCEEDED(a_device->CreateSamplerState(&sampler_desc, &g_point_clamp));
    }

    [[nodiscard]] bool create_constant_buffer(ID3D11Device* a_device, std::size_t a_size, ID3D11Buffer** a_buffer)
    {
        D3D11_BUFFER_DESC desc{};
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.ByteWidth = static_cast<UINT>(a_size);
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(a_device->CreateBuffer(&desc, nullptr, a_buffer));
    }

    [[nodiscard]] bool ensure_pipeline(ID3D11Device* a_device)
    {
        if (g_pipeline_ready)
            return true;

        std::string const rigid_source = std::string(Mask_Common_Hlsl) + Mask_Rigid_Vs_Hlsl;
        std::string const skinned_source = std::string(Mask_Common_Hlsl) + Mask_Skinned_Vs_Hlsl;
        std::string const mask_ps_source = std::string(Mask_Common_Hlsl) + Mask_Ps_Hlsl;
        std::string const dilate_source = std::string(Edge_Common_Hlsl) + Dilate_Ps_Hlsl;
        std::string const outline_source = std::string(Edge_Common_Hlsl) + Outline_Ps_Hlsl;

        ID3DBlob* rigid_blob = ShaderCompile::compile(rigid_source.c_str(), "vs_5_0", "silhouette rigid");
        ID3DBlob* skinned_blob = ShaderCompile::compile(skinned_source.c_str(), "vs_5_0", "silhouette skinned");
        ID3DBlob* mask_ps_blob = ShaderCompile::compile(mask_ps_source.c_str(), "ps_5_0", "silhouette mask");
        ID3DBlob* fullscreen_blob = ShaderCompile::compile(Fullscreen_Vs_Hlsl, "vs_5_0", "silhouette fullscreen");
        ID3DBlob* dilate_blob = ShaderCompile::compile(dilate_source.c_str(), "ps_5_0", "silhouette dilate");
        ID3DBlob* outline_blob = ShaderCompile::compile(outline_source.c_str(), "ps_5_0", "silhouette outline");

        bool ok = rigid_blob && skinned_blob && mask_ps_blob && fullscreen_blob && dilate_blob && outline_blob;
        if (ok)
        {
            ok = SUCCEEDED(a_device->CreateVertexShader(rigid_blob->GetBufferPointer(), rigid_blob->GetBufferSize(), nullptr, &g_rigid_vs)) &&
                 SUCCEEDED(a_device->CreateVertexShader(skinned_blob->GetBufferPointer(), skinned_blob->GetBufferSize(), nullptr, &g_skinned_vs)) &&
                 SUCCEEDED(a_device->CreatePixelShader(mask_ps_blob->GetBufferPointer(), mask_ps_blob->GetBufferSize(), nullptr, &g_mask_ps)) &&
                 SUCCEEDED(a_device->CreateVertexShader(fullscreen_blob->GetBufferPointer(), fullscreen_blob->GetBufferSize(), nullptr, &g_fullscreen_vs)) &&
                 SUCCEEDED(a_device->CreatePixelShader(dilate_blob->GetBufferPointer(), dilate_blob->GetBufferSize(), nullptr, &g_dilate_ps)) &&
                 SUCCEEDED(a_device->CreatePixelShader(outline_blob->GetBufferPointer(), outline_blob->GetBufferSize(), nullptr, &g_outline_ps)) &&
                 create_layouts(a_device, rigid_blob, skinned_blob) &&
                 create_states(a_device) &&
                 create_constant_buffer(a_device, sizeof(MaskConstants), &g_mask_cb) &&
                 create_constant_buffer(a_device, sizeof(EdgeConstants), &g_edge_cb);
        }

        release_object(rigid_blob);
        release_object(skinned_blob);
        release_object(mask_ps_blob);
        release_object(fullscreen_blob);
        release_object(dilate_blob);
        release_object(outline_blob);

        if (!ok)
        {
            release_pipeline();
            return false;
        }

        g_pipeline_ready = true;
        logger::info("Silhouette pipeline ready");
        return true;
    }

    [[nodiscard]] bool create_target(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height, ID3D11Texture2D** a_texture, ID3D11RenderTargetView** a_rtv, ID3D11ShaderResourceView** a_srv)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = a_width;
        desc.Height = a_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(a_device->CreateTexture2D(&desc, nullptr, a_texture)))
            return false;
        if (FAILED(a_device->CreateRenderTargetView(*a_texture, nullptr, a_rtv)))
            return false;

        return SUCCEEDED(a_device->CreateShaderResourceView(*a_texture, nullptr, a_srv));
    }

    [[nodiscard]] bool ensure_targets(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height)
    {
        if (g_mask_rtv && g_dilate_rtv && g_width == a_width && g_height == a_height)
            return true;

        release_targets();
        if (!create_target(a_device, a_width, a_height, &g_mask_texture, &g_mask_rtv, &g_mask_srv) ||
            !create_target(a_device, a_width, a_height, &g_dilate_texture, &g_dilate_rtv, &g_dilate_srv))
        {
            release_targets();
            return false;
        }

        g_width = a_width;
        g_height = a_height;
        logger::info("Silhouette mask targets: {}x{}", a_width, a_height);
        return true;
    }

    void set_full_viewport(ID3D11DeviceContext* a_context)
    {
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(g_width);
        viewport.Height = static_cast<float>(g_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        a_context->RSSetViewports(1, &viewport);
    }

    void pack_transform(RE::NiTransform const& a_transform, float a_rows[3][4])
    {
        RE::NiMatrix3 const& rotate = a_transform.rotate;
        float const scale = a_transform.scale;
        for (std::size_t row = 0; row < 3; ++row)
        {
            a_rows[row][0] = rotate.entry[row][0] * scale;
            a_rows[row][1] = rotate.entry[row][1] * scale;
            a_rows[row][2] = rotate.entry[row][2] * scale;
        }
        a_rows[0][3] = a_transform.translate.x;
        a_rows[1][3] = a_transform.translate.y;
        a_rows[2][3] = a_transform.translate.z;
    }

    void draw_part(ID3D11DeviceContext* a_context, Part const& a_part, float const a_world_to_clip[4][4], float a_alpha)
    {
        auto* const vertex_buffer = reinterpret_cast<ID3D11Buffer*>(a_part.buffer->vertexBuffer);
        auto* const index_buffer = reinterpret_cast<ID3D11Buffer*>(a_part.buffer->indexBuffer);
        RE::NiAVObject* const geometry = a_part.geometry.get();
        if (!vertex_buffer || !index_buffer || !geometry)
            return;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(a_context->Map(g_mask_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;

        auto* const constants = static_cast<MaskConstants*>(mapped.pData);
        std::memcpy(constants->world_to_cam, a_world_to_clip, sizeof(constants->world_to_cam));
        constants->params[0] = a_alpha;
        constants->params[1] = 0.0f;
        constants->params[2] = 0.0f;
        constants->params[3] = 0.0f;
        if (a_part.skinned)
        {
            for (std::size_t bone = 0; bone < a_part.bones.size(); ++bone)
            {
                RE::NiAVObject const* const node = a_part.bones[bone].get();
                RE::NiTransform const skin_to_world = node->world * a_part.skin_to_bone[bone];
                pack_transform(skin_to_world, reinterpret_cast<float(*)[4]>(constants->rows[bone * 3]));
            }
        }
        else
            pack_transform(geometry->world, reinterpret_cast<float(*)[4]>(constants->rows[0]));

        a_context->Unmap(g_mask_cb, 0);

        UINT const stride = a_part.stride;
        if (a_part.skinned)
        {
            ID3D11Buffer* const buffers[2]{ vertex_buffer, vertex_buffer };
            UINT const strides[2]{ stride, stride };
            UINT const offsets[2]{ 0, a_part.skin_offset };
            a_context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
        }
        else
        {
            UINT const offset = 0;
            a_context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
        }

        a_context->IASetIndexBuffer(index_buffer, DXGI_FORMAT_R16_UINT, 0);
        a_context->IASetInputLayout(g_layouts[layout_index(a_part.skinned, a_part.full_precision)]);
        a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        a_context->VSSetShader(a_part.skinned ? g_skinned_vs : g_rigid_vs, nullptr, 0);
        a_context->PSSetShader(g_mask_ps, nullptr, 0);
        a_context->VSSetConstantBuffers(0, 1, &g_mask_cb);
        a_context->PSSetConstantBuffers(0, 1, &g_mask_cb);
        a_context->DrawIndexed(a_part.index_count, 0, 0);
    }
}

namespace MeshOutline
{
    struct DrawList
    {
        std::vector<Part> parts;
    };

    DrawListPtr collect(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref)
            return {};

        RE::NiAVObject* const root = a_ref->Get3D();
        if (!root)
            return {};

        std::vector<Part> parts;
        collect_node(root, parts);
        if (parts.empty())
            return {};

        auto list = std::make_shared<DrawList>();
        list->parts = std::move(parts);
        return list;
    }

    std::size_t part_count(DrawListPtr const& a_list) noexcept
    {
        return a_list ? a_list->parts.size() : 0;
    }

    // 遮罩趟的世界->裁剪矩阵由调用方整份透传 NiCamera::worldToCam
    // （引擎的世界->裁剪 view-projection 矩阵），这里不再做任何组合。

    bool begin_frame(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::uint32_t a_width, std::uint32_t a_height)
    {
        if (g_failed || !a_device || !a_context || a_width == 0 || a_height == 0)
            return false;

        if (!ensure_pipeline(a_device) || !ensure_targets(a_device, a_width, a_height))
        {
            g_failed = true;
            release();
            logger::error("Silhouette resources unavailable, falling back to bounding box outline");
            return false;
        }

        g_saved_viewport_count = 1;
        a_context->RSGetViewports(&g_saved_viewport_count, &g_saved_viewport);

        constexpr float Clear[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
        a_context->ClearRenderTargetView(g_mask_rtv, Clear);

        ID3D11ShaderResourceView* const no_resources[2]{ nullptr, nullptr };
        a_context->PSSetShaderResources(0, 2, no_resources);
        a_context->OMSetRenderTargets(1, &g_mask_rtv, nullptr);
        a_context->OMSetBlendState(g_max_blend, nullptr, 0xFFFFFFFF);
        a_context->OMSetDepthStencilState(g_depth_none, 0);
        a_context->RSSetState(g_cull_none);
        set_full_viewport(a_context);

        g_draw_count = 0;
        return true;
    }

    void draw(ID3D11DeviceContext* a_context, DrawListPtr const& a_list, float const a_world_to_clip[4][4], float a_alpha)
    {
        if (!a_context || !a_list || !g_pipeline_ready)
            return;

        for (Part const& part : a_list->parts)
        {
            draw_part(a_context, part, a_world_to_clip, a_alpha);
            ++g_draw_count;
        }
    }

    void resolve(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, float a_thickness, float a_red, float a_green, float a_blue)
    {
        if (!a_context || !g_pipeline_ready)
            return;

        if (g_draw_count != 0 && a_target)
        {
            std::uint32_t const radius = std::clamp(static_cast<std::uint32_t>(a_thickness + 0.5f), 1u, Max_Dilate_Radius);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(a_context->Map(g_edge_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                auto* const constants = static_cast<EdgeConstants*>(mapped.pData);
                constants->texel[0] = 1.0f / static_cast<float>(g_width);
                constants->texel[1] = 1.0f / static_cast<float>(g_height);
                constants->texel[2] = static_cast<float>(radius);
                constants->texel[3] = 0.0f;
                constants->color[0] = a_red;
                constants->color[1] = a_green;
                constants->color[2] = a_blue;
                constants->color[3] = 1.0f;
                a_context->Unmap(g_edge_cb, 0);

                a_context->IASetInputLayout(nullptr);
                a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                a_context->VSSetShader(g_fullscreen_vs, nullptr, 0);
                a_context->PSSetConstantBuffers(0, 1, &g_edge_cb);
                a_context->PSSetSamplers(0, 1, &g_point_clamp);

                // 横向膨胀：mask -> dilate
                a_context->OMSetRenderTargets(1, &g_dilate_rtv, nullptr);
                a_context->OMSetBlendState(g_opaque_blend, nullptr, 0xFFFFFFFF);
                a_context->PSSetShader(g_dilate_ps, nullptr, 0);
                a_context->PSSetShaderResources(0, 1, &g_mask_srv);
                a_context->Draw(3, 0);

                // 纵向膨胀 + 边缘判定：dilate/mask -> 目标
                ID3D11ShaderResourceView* const no_resources[2]{ nullptr, nullptr };
                a_context->PSSetShaderResources(0, 2, no_resources);
                a_context->OMSetRenderTargets(1, &a_target, nullptr);
                a_context->OMSetBlendState(g_alpha_blend, nullptr, 0xFFFFFFFF);
                a_context->PSSetShader(g_outline_ps, nullptr, 0);
                ID3D11ShaderResourceView* const resources[2]{ g_mask_srv, g_dilate_srv };
                a_context->PSSetShaderResources(0, 2, resources);
                a_context->Draw(3, 0);
                a_context->PSSetShaderResources(0, 2, no_resources);
            }
        }

        if (g_saved_viewport_count != 0)
            a_context->RSSetViewports(g_saved_viewport_count, &g_saved_viewport);

        g_draw_count = 0;
    }

    void release() noexcept
    {
        release_targets();
        release_pipeline();
    }
}
