// Fullscreen tonemapped-LDR blit into the swapchain, used when SMAA is off.
// Same vertex convention as the SMAA passes: the negated-Y viewport inverts
// fullscreen raster writes, which is also what un-mirrors the compute-written
// LDR back into screen-upright rows here.
struct BlitSwapchainPush
{
    uint texture_id;
    uint sampler_id;
};

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::push_constant]]
BlitSwapchainPush push;

struct VSOut
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

[shader("vertex")]
VSOut vs_main(uint id : SV_VertexID)
{
    static const float2 pos[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0)
    };

    static const float2 uv[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0)
    };

    VSOut outv;
    outv.position = float4(pos[id], 0.0, 1.0);
    outv.texcoord = uv[id];
    return outv;
}

[shader("fragment")]
float4 fs_main(VSOut input) : SV_Target0
{
    return textures[NonUniformResourceIndex(push.texture_id)].SampleLevel(
        samplers[NonUniformResourceIndex(push.sampler_id)], input.texcoord, 0);
}
