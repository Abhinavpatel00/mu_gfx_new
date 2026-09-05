#define SMAA_CUSTOM_SL
#define SMAA_PRESET_HIGH

struct EdgePush
{
    uint texture_id;
    uint sampler_id;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 offset : TEXCOORD1;
    float4 offset2 : TEXCOORD2;
    float4 offset3 : TEXCOORD3;
};

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::push_constant]]
EdgePush push;

float4 sample_point(Texture2D tex, float2 coord)
{
    uint w, h;
    tex.GetDimensions(w, h);

    int2 pix = int2(coord * float2(w, h));
    pix      = clamp(pix, int2(0, 0), int2(int(w) - 1, int(h) - 1));
    return tex.Load(int3(pix, 0));
}

float4 smaa_rt_metrics()
{
    Texture2D color_tex = textures[NonUniformResourceIndex(push.texture_id)];
    uint      w, h;
    color_tex.GetDimensions(w, h);
    return float4(1.0 / float(w), 1.0 / float(h), float(w), float(h));
}

#define SMAA_RT_METRICS smaa_rt_metrics()
#define SMAA_AREATEX_SELECT(sample) sample.rg
#define SMAA_SEARCHTEX_SELECT(sample) sample.r
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(samplers[NonUniformResourceIndex(push.sampler_id)], coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) sample_point(tex, coord)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(samplers[NonUniformResourceIndex(push.sampler_id)], coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(samplers[NonUniformResourceIndex(push.sampler_id)], coord)
#define SMAASamplePoint(tex, coord) sample_point(tex, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(samplers[NonUniformResourceIndex(push.sampler_id)], coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]

#include "../external/smaa/SMAA.hlsl"

[shader("vertex")]
VSOutput vs_main(uint id : SV_VertexID)
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

    VSOutput outv;
    outv.position = float4(pos[id], 0.0, 1.0);
    outv.texcoord = uv[id];

    float4 offsets[3];
    SMAAEdgeDetectionVS(outv.texcoord, offsets);
    outv.offset  = offsets[0];
    outv.offset2 = offsets[1];
    outv.offset3 = offsets[2];

    return outv;
}

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target0
{
    Texture2D color_tex = textures[NonUniformResourceIndex(push.texture_id)];

    float4 offsets[3] = {input.offset, input.offset2, input.offset3};
    float2 edges      = SMAALumaEdgeDetectionPS(input.texcoord, offsets, color_tex);

    return float4(edges, 0.0, 0.0);
}
