// Integer IDs are categorical: zero is background, styles[id - 1] is RGBA.
// Both consumers output premultiplied alpha for ONE / INV_SRC_ALPHA blending.
struct PS_IN
{
    float4 pos : SV_Position;
};

PS_IN vs_main(uint id : SV_VertexID)
{
    PS_IN o;
    float2 uv = float2(float((id << 1u) & 2u), float(id & 2u));
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}

Texture2D<uint> g_mask : register(t0);
StructuredBuffer<float4> g_styles : register(t1);

float4 styled_pixel(uint id)
{
    if (id == 0)
        return 0.0f;
    return g_styles[id - 1];
}

float4 ps_silhouette_main(PS_IN ps_in) : SV_Target
{
    return styled_pixel(g_mask.Load(int3(int2(ps_in.pos.xy), 0)));
}
