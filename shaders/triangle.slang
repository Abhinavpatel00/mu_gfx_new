struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VSOutput vs_main(uint id : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(0.0, -0.65),
        float2(0.65, 0.55),
        float2(-0.65, 0.55)
    };

    static const float3 colors[3] = {
        float3(1.0, 0.18, 0.12),
        float3(0.12, 0.85, 0.32),
        float3(0.15, 0.38, 1.0)
    };

    VSOutput outv;
    outv.position = float4(positions[id], 0.0, 1.0);
    outv.color = colors[id];
    return outv;
}

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
