// mask 几何 pass：把尸体 mesh 画进离屏 mask RT。
// 静态 VS 的世界变换已在 CPU 端组合进 ViewProj*World；蒙皮 VS 使用分区调色板
// 常量缓冲（每分区一次 draw，palette 容量受限，见 mask_types.h）。
// 尸体索引经 nointerpolation 语义从 VS 传给 PS 写入 B 通道（同一 draw 恒定），
// MAX 混合在重叠区取较高索引，消费 pass 据此查 per-frame alpha LUT。

// ---- 静态 VS：row_major float4x4 + 尸体索引（80 字节，见 MaskPerDrawCBData）----
cbuffer PerDrawCB : register(b0)
{
    row_major float4x4 g_world_view_proj;
    float g_corpse_index;
    float3 g_pad;
};

struct VS_OUT
{
    float4 pos : SV_Position;
    nointerpolation float corpse_index : TEXCOORD0;
};

VS_OUT vs_static_main(float3 a_pos : POSITION)
{
    VS_OUT o;
    o.pos = mul(g_world_view_proj, float4(a_pos, 1.0f));
    o.corpse_index = g_corpse_index;
    return o;
}

// ---- 蒙皮 VS：VB 自带混合权重/索引，索引指向分区调色板 g_bones ----
// 槽位数须与 mask_types.h 的 Max_Palette_Bones 一致（128 × 64B = 8KB）。
cbuffer PaletteCB : register(b1)
{
    row_major float4x4 g_bones[128];
};

struct VS_SKIN_IN
{
    float3 pos : POSITION;
    float4 weights : BLENDWEIGHT;
    uint4 indices : BLENDINDICES;
};

VS_OUT vs_skinned_main(VS_SKIN_IN a_in)
{
    float4 p = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i)
        p += a_in.weights[i] * mul(g_bones[a_in.indices[i]], float4(a_in.pos, 1.0f));
    VS_OUT o;
    o.pos = mul(g_world_view_proj, p);
    o.corpse_index = g_corpse_index;
    return o;
}

// ---- mask PS：R/G 恒为 1（覆盖度由消费 pass 取 max(R,G)），B = 尸体索引 ----
struct PS_IN
{
    float4 pos : SV_Position;
    nointerpolation float corpse_index : TEXCOORD0;
};

float4 ps_main(PS_IN a_in) : SV_Target
{
    return float4(1.0f, 1.0f, a_in.corpse_index, 1.0f);
}
