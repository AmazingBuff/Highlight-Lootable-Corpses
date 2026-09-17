// Static VS: row_major float4x4 + uint object_id (80 bytes; PerDrawCBData).
cbuffer PerDrawCB : register(b0)
{
    row_major float4x4 g_world_view_proj;
    uint g_object_id;
    float3 g_pad;
};

struct VS_OUT
{
    float4 pos : SV_Position;
    nointerpolation uint object_id : TEXCOORD0;
};

VS_OUT vs_static_main(float3 pos : POSITION)
{
    VS_OUT o;
    o.pos = mul(g_world_view_proj, float4(pos, 1.0f));
    o.object_id = g_object_id;
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

VS_OUT vs_skinned_main(VS_SKIN_IN skin_in)
{
    float4 p = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i)
        p += skin_in.weights[i] * mul(g_bones[skin_in.indices[i]], float4(skin_in.pos, 1.0f));
    VS_OUT o;
    o.pos = mul(g_world_view_proj, p);
    o.object_id = g_object_id;
    return o;
}

uint ps_main(VS_OUT ps_in) : SV_Target
{
    return ps_in.object_id;
}
