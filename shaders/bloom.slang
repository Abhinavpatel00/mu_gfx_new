/* Separable bloom: one 9-tap pass per axis. Pass 1 also applies the bright
   threshold so the glow only comes from highlights; pass 2 is a pure blur of
   that result. Two dispatches, one shader, alternating the half-res targets. */
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

static const float kWeights[5] = {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216};

[numthreads(16, 16, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= push.width || id.y >= push.height)
    return;

  int2 pix = int2(id.xy);
  Texture2D           src = textures[push.src_texture_id];
  RWTexture2D<float4> dst = storage_images[push.dst_image_id];
  SamplerState        smp = samplers[push.sampler_id];

  float2 texel = 1.0 / float2(push.width, push.height);
  float2 uv    = (float2(pix) + 0.5) * texel;

    /* The first pass reads the full-resolution HDR image while the bloom
      targets are half-resolution; normalized sampling keeps both passes in
      the same image space. */
    float3 c = src.SampleLevel(smp, uv, 0).rgb;

  if (push.prefilter != 0u)
  {
    float lum  = dot(c, float3(0.2126, 0.7152, 0.0722));
    float knee = max(push.soft_knee, 1e-4);
    float soft = clamp(lum - push.threshold + knee, 0.0, 2.0 * knee);
    soft       = soft * soft / (4.0 * knee);
    c *= max(soft, lum - push.threshold) / max(lum, 1e-4);
  }

  float2 step = float2(push.dir_x, push.dir_y) * texel;
  float3 sum  = c * kWeights[0];
  [unroll]
  for (uint i = 1u; i < 5u; ++i)
  {
    float2 o = step * float(i);
    sum += src.SampleLevel(smp, uv + o, 0).rgb * kWeights[i];
    sum += src.SampleLevel(smp, uv - o, 0).rgb * kWeights[i];
  }
  dst[pix] = float4(sum, 1.0);
}
