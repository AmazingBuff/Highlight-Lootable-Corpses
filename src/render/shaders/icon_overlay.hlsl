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

PS_IN vs_main(VS_IN input)
{
    PS_IN o;
    o.pos = float4(input.pos, 1.0f);
    o.color = input.color;
    return o;
}

float4 ps_main(PS_IN input) : SV_Target
{
    return float4(input.color.rgb, input.color.a);
}