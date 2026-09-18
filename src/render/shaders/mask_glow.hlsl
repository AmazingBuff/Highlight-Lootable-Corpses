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
Texture2D<float2> g_horizontal : register(t1);
StructuredBuffer<float4> g_styles : register(t2);

cbuffer GlowCB : register(b0)
{
    float4 g_narrow_weights[5];
    float4 g_wide_weights[5];
    uint g_radius;
    uint g_width;
    uint g_height;
    uint g_object_id;
    int4 g_horizontal_rect;
};

float2 ps_glow_horizontal(PS_IN ps_in) : SV_Target
{
    int2 pixel = int2(ps_in.pos.xy);
    if (pixel.x < g_horizontal_rect.x || pixel.x >= g_horizontal_rect.z ||
        pixel.y < g_horizontal_rect.y || pixel.y >= g_horizontal_rect.w ||
        pixel.x < 0 || pixel.x >= int(g_width) || pixel.y < 0 || pixel.y >= int(g_height) || g_object_id == 0)
        return 0.0f;

    float2 coverage = 0.0f;
    [loop]
    for (int dx = -int(g_radius); dx <= int(g_radius); ++dx)
    {
        int sample_x = pixel.x + dx;
        if (sample_x < 0 || sample_x >= int(g_width))
            continue;

        if (g_mask.Load(int3(sample_x, pixel.y, 0)) == g_object_id)
        {
            int distance = abs(dx);
            float4 narrow_slot = g_narrow_weights[distance >> 2];
            float4 wide_slot = g_wide_weights[distance >> 2];
            coverage += float2(narrow_slot[distance & 3], wide_slot[distance & 3]);
        }
    }
    return saturate(coverage);
}

float4 ps_glow_vertical(PS_IN ps_in) : SV_Target
{
    int2 pixel = int2(ps_in.pos.xy);
    if (pixel.x < g_horizontal_rect.x || pixel.x >= g_horizontal_rect.z ||
        pixel.x < 0 || pixel.x >= int(g_width) || pixel.y < 0 || pixel.y >= int(g_height) || g_object_id == 0)
        return 0.0f;

    if (g_mask.Load(int3(pixel, 0)) == g_object_id)
        return 0.0f;

    float2 coverage = 0.0f;
    [loop]
    for (int dy = -int(g_radius); dy <= int(g_radius); ++dy)
    {
        int sample_y = pixel.y + dy;
        if (sample_y < g_horizontal_rect.y || sample_y >= g_horizontal_rect.w || sample_y < 0 || sample_y >= int(g_height))
            continue;

        int distance = abs(dy);
        float4 narrow_slot = g_narrow_weights[distance >> 2];
        float4 wide_slot = g_wide_weights[distance >> 2];
        float2 sample_coverage = g_horizontal.Load(int3(pixel.x, sample_y, 0));
        coverage += sample_coverage * float2(narrow_slot[distance & 3], wide_slot[distance & 3]);
    }

    const float core_alpha = saturate(coverage.x * 3.0f);
    const float halo_alpha = saturate(coverage.y * 1.25f) * 0.65f;
    const float halo_visibility = halo_alpha * (1.0f - core_alpha);
    const float4 style = g_styles[g_object_id - 1];
    const float3 core_color = lerp(style.rgb, float3(1.0f, 1.0f, 1.0f), 0.20f);
    const float combined_alpha = core_alpha + halo_visibility;
    const float3 combined_rgb = core_color * core_alpha + style.rgb * halo_visibility;
    return float4(combined_rgb * style.a, combined_alpha * style.a);
}
