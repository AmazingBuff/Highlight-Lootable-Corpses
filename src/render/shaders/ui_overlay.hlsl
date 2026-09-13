// UI 覆盖层管线（icon 模式的屏幕空间图元）。
// 顶点位置在 CPU 端已直接换算为 NDC，VS 仅做透传。
//
// 混合约定（重要）：OM 绑定的混合状态是 (SrcBlend=ONE, DestBlend=INV_SRC_ALPHA)
// 的"预乘 alpha"混合，因此 PS 必须输出 rgb*a 的预乘颜色。此前 PS 直出直 alpha
// 颜色，rgb 以全强度（×ONE）叠加，alpha 只控制背景透出比例，导致 alpha=0.5 的
// 形状视觉上仍是实心——这就是"边框透明度不对/距离衰减无效"的根因（本机后备
// 缓冲为 R10G10B10A2_UNORM 时另受 2-bit alpha 量化限制，见 back_buffer_target）。

struct VS_IN
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct PS_IN
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};

PS_IN vs_main(VS_IN a_input)
{
    PS_IN o;
    o.pos = float4(a_input.pos, 1.0f);
    o.color = a_input.color;
    return o;
}

float4 ps_main(PS_IN a_input) : SV_Target
{
    return float4(a_input.color.rgb * a_input.color.a, a_input.color.a);
}
