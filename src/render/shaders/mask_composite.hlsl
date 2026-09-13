// mask 的两个全屏消费 pass（SV_VertexID 全屏三角形，无需顶点缓冲）。
//
// alpha LUT（b1）：256 个 corpse_alpha 按目标序号打包为 64 个 float4；mask B 通道
// 存尸体索引，查表得到该尸体的距离衰减不透明度。
//
// 两个 PS 均为预乘 alpha 混合（ONE/INV_SRC_ALPHA）约定，必须输出 rgb*a。

// ---- 全屏三角形 VS ----
struct PS_IN
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

PS_IN vs_main(uint a_id : SV_VertexID)
{
    PS_IN o;
    float2 uv = float2(float((a_id << 1u) & 2u), float(a_id & 2u));
    o.uv = uv;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}

// ---- silhouette 模式：剪影内部填充 ----
Texture2D g_mask : register(t0);
SamplerState g_mask_sampler : register(s0);

cbuffer CompositeCB : register(b0)
{
    float4 g_color;  // rgb + 填充系数
};
cbuffer AlphaLutCB : register(b1)
{
    float4 g_alpha_lut[64];  // 256 个 corpse_alpha（按目标序号）
};

float4 ps_silhouette_main(PS_IN a_in) : SV_Target
{
    // 注意：HLSL/FXC 不接受 "float const"（east const 仅适用于 C++ 代码，不适用于着色器串）
    const float4 m = g_mask.Sample(g_mask_sampler, a_in.uv);
    // B 通道 = 尸体索引，查 per-frame alpha LUT（单次消费无复合）
    const uint idx = min(255u, (uint)round(m.b * 255.0f));
    const float a = max(m.r, m.g) * g_alpha_lut[idx / 4][idx % 4] * g_color.a;
    return float4(g_color.rgb * a, a);
}

// ---- outline 模式：对 mask 做圆盘膨胀，仅在剪影外侧画色带 ----
// （中心覆盖度 > 0.5 即输出透明，内部不填充——内部显示由 silhouette 填充或无负责）

cbuffer OutlineCB : register(b0)
{
    float2 g_texel;         // (1/W, 1/H)
    float g_radius;         // 圆盘半径（像素）
    float g_pad;
    float4 g_outline_color; // rgb + a
    // 成员名不能与 CompositeCB 的 g_color 重名：同一文件里的全部入口共享
    // 一个编译单元，cbuffer 成员占用全局命名空间，重名即重定义错误。
};

float coverage(float2 a_uv)
{
    const float4 m = g_mask.Sample(g_mask_sampler, a_uv);
    return max(m.r, m.g);
}

float4 ps_outline_main(PS_IN a_in) : SV_Target
{
    if (coverage(a_in.uv) > 0.5f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);  // 剪影内部：不画

    // 命中样本按 B 通道索引查 alpha LUT，取最大值
    const int r = (int)g_radius;
    float hit_alpha = 0.0f;
    [loop]
    for (int dy = -r; dy <= r; ++dy)
    {
        [loop]
        for (int dx = -r; dx <= r; ++dx)
        {
            const float4 m = g_mask.Sample(g_mask_sampler, a_in.uv + float2(float(dx), float(dy)) * g_texel);
            if (max(m.r, m.g) > 0.5f)
            {
                const uint idx = min(255u, (uint)round(m.b * 255.0f));
                hit_alpha = max(hit_alpha, g_alpha_lut[idx / 4][idx % 4]);
            }
        }
    }
    const float final_a = hit_alpha * g_outline_color.a;
    return float4(g_outline_color.rgb * final_a, final_a);
}
