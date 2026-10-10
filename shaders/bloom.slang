struct BloomPush
{
    uint  src_texture_id;
    uint  dst_image_id;
    uint  sampler_id;
    uint  width;
    uint  height;
    float dir_x;
    float dir_y;
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

// Symmetric, normalized 9-tap Gaussian-like kernel.
static const float kWeights[5] =
{
    0.2270270,
    0.1945946,
    0.1216216,
    0.0540540,
    0.0162160
};

// Soft-knee bright pass.
//
// Input RGB must be linear HDR, before tone mapping.
// The knee smooths the transition around the threshold.
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

// Apply bright-pass extraction only during pass 1.
// Pass 2 uses the already-extracted image and only blurs it.
float3 BloomSample(
    Texture2D src,
    SamplerState smp,
    float2 uv)
{
    float3 c = src.SampleLevel(smp, uv, 0).rgb;

    if (push.prefilter != 0u)
        c = BloomPrefilter(c);

    return c;
}

[numthreads(16, 16, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.width || id.y >= push.height)
        return;

    int2 pix = int2(id.xy);

    Texture2D src = textures[push.src_texture_id];
    RWTexture2D<float4> dst = storage_images[push.dst_image_id];
    SamplerState smp = samplers[push.sampler_id];

    // Destination-space texel size.
    // On the first pass, this also establishes the half-resolution
    // sampling footprint in the full-resolution source.
    float2 texel = 1.0 / float2(push.width, push.height);
    float2 uv = (float2(pix) + 0.5) * texel;

    // Center sample is thresholded when prefilter is enabled.
    float3 sum = BloomSample(src, smp, uv) * kWeights[0];

    // Four positive and four negative offsets.
    // Every sample follows the same bright-pass policy.
    float2 step_uv = float2(push.dir_x, push.dir_y) * texel;

    [unroll]
    for (uint i = 1u; i < 5u; ++i)
    {
        float2 offset = step_uv * float(i);

        float3 a = BloomSample(src, smp, uv + offset);
        float3 b = BloomSample(src, smp, uv - offset);

        sum += (a + b) * kWeights[i];
    }

    dst[pix] = float4(sum, 1.0);
}
