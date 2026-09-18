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
cbuffer CompositeCB : register(b0)
{
    float g_radius;
    float g_fill;
    float2 g_pad;
};

float4 styled_pixel(uint id, float factor)
{
    if (id == 0)
        return 0.0f;
    const float4 style = g_styles[id - 1];
    const float alpha = style.a * factor;
    return float4(style.rgb * alpha, alpha);
}

float4 ps_silhouette_main(PS_IN ps_in) : SV_Target
{
    return styled_pixel(g_mask.Load(int3(int2(ps_in.pos.xy), 0)), g_fill);
}

// Legacy comparison-only entry; production outline rendering uses mask_glow.hlsl.
float4 ps_outline_main(PS_IN ps_in) : SV_Target
{
    const int2 pixel = int2(ps_in.pos.xy);
    if (g_mask.Load(int3(pixel, 0)) != 0)
        return 0.0f;

    const int r = (int)g_radius;
    [loop]
    for (int dy = -r; dy <= r; ++dy)
    {
        [loop]
        for (int dx = -r; dx <= r; ++dx)
        {
            const uint id = g_mask.Load(int3(pixel + int2(dx, dy), 0));
            if (id != 0)
                return styled_pixel(id, 1.0f);
        }
    }
    return 0.0f;
}
