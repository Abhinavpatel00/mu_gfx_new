struct BloomPush
{
    uint  src_texture_id;
    uint  dst_image_id;
    uint  sampler_id;
    uint  width;
    uint  height;
    uint  pass;   /* 0 = extract+downsample from HDR, 1 = mip downsample, 2 = mip upsample+accumulate */
    float radius; /* upsample spread, destination texels */
    float threshold;
    float soft_knee;
    uint  prefilter;
};

[[vk::push_constant]]
BloomPush push;

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::binding(2, 0)]]
RWTexture2D<float4> storage_images[];

// Soft-knee bright pass. Input RGB is linear HDR before the tonemap; the
// knee smooths the transition around the threshold.
float3 BloomPrefilter(float3 c)
{
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));

    float knee = max(push.soft_knee, 1e-4);
    float x = lum - push.threshold;

    float soft = clamp(x + knee, 0.0, 2.0 * knee);
    soft = (soft * soft) / (4.0 * knee);

    float contribution = max(soft, x);

    // Scale RGB uniformly to preserve its hue and saturation.
    return c * (contribution / max(lum, 1e-4));
}

// Extract on pass 0 only: later passes blur already-extracted light.
// HDR alpha carries the per-instance bloom strength: 0 excludes the pixel,
// >0 includes it with that weight. This is branchless data selection, not a
// per-pixel flag branch.
float3 BloomExtract(
    Texture2D src,
    SamplerState smp,
    float2 uv)
{
    float4 s = src.SampleLevel(smp, uv, 0);
    float3 c = s.rgb * s.a;

    if (push.prefilter != 0u)
        c = BloomPrefilter(c);

    return c;
}

float3 Downsample13(
    Texture2D src,
    SamplerState smp,
    float2 uv,
    float2 texel)
{
    // Jorge Jimenez. Next Generation Post Processing in Call of Duty:
    // Advanced Warfare. Weights sum to 1.
    float3 result = src.SampleLevel(smp, uv, 0).rgb * 0.125;

    result += src.SampleLevel(smp, uv + texel * float2(+0.5, +0.5), 0).rgb * (0.5 / 4.0);
    result += src.SampleLevel(smp, uv + texel * float2(+0.5, -0.5), 0).rgb * (0.5 / 4.0);
    result += src.SampleLevel(smp, uv + texel * float2(-0.5, +0.5), 0).rgb * (0.5 / 4.0);
    result += src.SampleLevel(smp, uv + texel * float2(-0.5, -0.5), 0).rgb * (0.5 / 4.0);

    result += src.SampleLevel(smp, uv + texel * float2(+1.0, +1.0), 0).rgb * (0.125 / 4.0);
    result += src.SampleLevel(smp, uv + texel * float2(+1.0, -1.0), 0).rgb * (0.125 / 4.0);
    result += src.SampleLevel(smp, uv + texel * float2(-1.0, +1.0), 0).rgb * (0.125 / 4.0);
    result += src.SampleLevel(smp, uv + texel * float2(-1.0, -1.0), 0).rgb * (0.125 / 4.0);

    result += src.SampleLevel(smp, uv + texel * float2(+1.0, 0.0), 0).rgb * (0.125 / 2.0);
    result += src.SampleLevel(smp, uv + texel * float2(-1.0, 0.0), 0).rgb * (0.125 / 2.0);
    result += src.SampleLevel(smp, uv + texel * float2(0.0, +1.0), 0).rgb * (0.125 / 2.0);
    result += src.SampleLevel(smp, uv + texel * float2(0.0, -1.0), 0).rgb * (0.125 / 2.0);

    return result;
}

float3 Upsample9(
    Texture2D src,
    SamplerState smp,
    float2 uv,
    float2 texel)
{
    // Same source: 3x3 tent with center weight 4, edges 2, corners 1.
    float  r      = max(push.radius, 1e-4);
    float2 spread = texel * r;

    float3 result = src.SampleLevel(smp, uv, 0).rgb * (4.0 / 16.0);

    result += src.SampleLevel(smp, uv + spread * float2(+1.0, 0.0), 0).rgb * (2.0 / 16.0);
    result += src.SampleLevel(smp, uv + spread * float2(-1.0, 0.0), 0).rgb * (2.0 / 16.0);
    result += src.SampleLevel(smp, uv + spread * float2(0.0, +1.0), 0).rgb * (2.0 / 16.0);
    result += src.SampleLevel(smp, uv + spread * float2(0.0, -1.0), 0).rgb * (2.0 / 16.0);

    result += src.SampleLevel(smp, uv + spread * float2(+1.0, +1.0), 0).rgb * (1.0 / 16.0);
    result += src.SampleLevel(smp, uv + spread * float2(+1.0, -1.0), 0).rgb * (1.0 / 16.0);
    result += src.SampleLevel(smp, uv + spread * float2(-1.0, +1.0), 0).rgb * (1.0 / 16.0);
    result += src.SampleLevel(smp, uv + spread * float2(-1.0, -1.0), 0).rgb * (1.0 / 16.0);

    return result;
}

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.width || id.y >= push.height)
        return;

    int2 pix = int2(id.xy);

    Texture2D         src = textures[push.src_texture_id];
    RWTexture2D<float4> dst = storage_images[push.dst_image_id];
    SamplerState      smp = samplers[push.sampler_id];

    // Destination-space texel size. On pass 0 this also establishes the
    // half-resolution sampling footprint in the full-resolution source.
    float2 texel = 1.0 / float2(push.width, push.height);
    float2 uv    = (float2(pix) + 0.5) * texel;

    if (push.pass == 0u)
    {
        // Extract emissive from full-res HDR and downsample to half-res. The
        // offsets below are in destination texels, which land on the four
        // source pixel centers exactly like the reference.
        float3 e0 = BloomExtract(src, smp, uv + texel * float2(-0.25, -0.25));
        float3 e1 = BloomExtract(src, smp, uv + texel * float2(+0.25, -0.25));
        float3 e2 = BloomExtract(src, smp, uv + texel * float2(-0.25, +0.25));
        float3 e3 = BloomExtract(src, smp, uv + texel * float2(+0.25, +0.25));

        dst[pix] = float4((e0 + e1 + e2 + e3) * 0.25, 0.0);
    }
    else if (push.pass == 1u)
    {
        dst[pix] = float4(Downsample13(src, smp, uv, texel), 0.0);
    }
    else
    {
        // Accumulate onto what is already there. The destination lives in
        // GENERAL, so the same thread can read back its own output without a
        // second sampled view of the mip.
        float3 base = dst[pix].rgb;
        dst[pix]    = float4(base + Upsample9(src, smp, uv, texel), 0.0);
    }
}

