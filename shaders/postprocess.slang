import tonemapping;

/* HDR -> LDR composite and grade. Field order is the C PostPush order: the
   push block is flat scalars, so the two layouts line up word for word. */
struct PostPush
{
  uint src_texture_id;
  uint output_image_id;
  uint sampler_id;

  uint width;
  uint height;

  uint  frame;
  float exposure;
  uint  flags;

  uint  tonemap_mode;
  float gamma_value;

  float contrast;
  float saturation;
  float temperature;
  float tint;

  float vignette_intensity;
  float vignette_smoothness;
  float sharpen_strength;
  float ca_strength;

  float grain_intensity;
  float sepia_intensity;
  float dither_strength;

  float lift_r;
  float lift_g;
  float lift_b;
  float gain_r;
  float gain_g;
  float gain_b;

  uint  lut_texture_id;  /* 0xFFFFFFFF = none */
  uint  lut_size;
  float lut_strength;

  uint  bloom_texture_id; /* 0xFFFFFFFF = none */
  float bloom_strength;
  float time;
};

static const uint FLAG_EXPOSURE      = (1u << 0);
static const uint FLAG_TONEMAP       = (1u << 1);
static const uint FLAG_GAMMA         = (1u << 2);
static const uint FLAG_CONTRAST      = (1u << 3);
static const uint FLAG_SATURATION    = (1u << 4);
static const uint FLAG_WHITE_BALANCE = (1u << 5);
static const uint FLAG_VIGNETTE      = (1u << 6);
static const uint FLAG_GRAIN         = (1u << 7);
static const uint FLAG_SHARPEN       = (1u << 8);
static const uint FLAG_CA            = (1u << 9);
static const uint FLAG_DITHER        = (1u << 10);
static const uint FLAG_SEPIA         = (1u << 11);
static const uint FLAG_LIFT_GAIN     = (1u << 12);

bool has_flag(uint flags, uint bit)
{
  return (flags & bit) != 0;
}

/* Tonemap selection is data, not control flow per pixel: the mode is one
   uniform, so the branch is coherent across the whole dispatch. */
inline float3 tonemap(uint mode, float3 c)
{
  if (mode == 1u) return aces(c);
  if (mode == 2u) return reinhard(c);
  if (mode == 3u) return reinhard2(c);
  if (mode == 4u) return filmic(c);
  if (mode == 5u) return uncharted2(c);
  if (mode == 6u) return unreal(c);
  return neutral(c);
}

/* Strip 3D LUT: n x n cells of n x n texels, blue major. The red axis is
   pulled in by (n-1)/n and the half-texel is applied so bilinear filtering
   never reads across a cell boundary. */
inline float3 sample_lut(Texture2D lut, SamplerState s, float3 c, uint n)
{
  float N = (float)n;
  c       = saturate(c);
  float b = c.b * (N - 1.0);
  float lo = floor(b);
  float f  = b - lo;

  float cell = (c.r * (N - 1.0) + 0.5) / (N * N);
  float v    = (c.g * (N - 1.0) + 0.5) / N;

  float2 uv0 = float2(cell + lo * N / (N * N), v);
  float2 uv1 = float2(cell + min(lo + 1.0, N - 1.0) * N / (N * N), v);
  return lerp(lut.SampleLevel(s, uv0, 0).rgb, lut.SampleLevel(s, uv1, 0).rgb, f);
}

/* Cheap hash for grain and dither; both want a per-pixel value that changes
   with the frame so static geometry does not sit behind a frozen pattern. */
inline float hash21(float2 p, float t)
{
  float h = dot(p, float2(127.1, 311.7)) + t * 74.7;
  return frac(sin(h) * 43758.5453);
}

[[vk::push_constant]]
PostPush push;

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::binding(2, 0)]]
RWTexture2D<float4> storage_images[];

[numthreads(16, 16, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= push.width || id.y >= push.height)
    return;

  int2 pix = int2(id.xy);
  Texture2D           src = textures[push.src_texture_id];
  RWTexture2D<float4> dst = storage_images[push.output_image_id];
  SamplerState        smp = samplers[push.sampler_id];

  float2 uv  = (float2(pix) + 0.5) / float2(push.width, push.height);
  float3 color;

  if (has_flag(push.flags, FLAG_CA) && push.ca_strength > 0.0)
  {
    float2 dir = (uv - 0.5) * push.ca_strength * 0.02;
    color.r = textures[push.src_texture_id].SampleLevel(smp, uv + dir, 0).r;
    color.g = textures[push.src_texture_id].SampleLevel(smp, uv, 0).g;
    color.b = textures[push.src_texture_id].SampleLevel(smp, uv - dir, 0).b;
  }
  else
  {
    color = src.Load(int3(pix, 0)).rgb;
  }

  /* Bloom is added in linear HDR, before exposure and the tonemap, so it
     rolls off with the rest of the highlights instead of clipping. */
  if (push.bloom_strength > 0.0 && push.bloom_texture_id != 0xFFFFFFFFu)
    color += textures[push.bloom_texture_id].SampleLevel(smp, uv, 0).rgb * push.bloom_strength;

  if (has_flag(push.flags, FLAG_EXPOSURE))
    color *= exp2(push.exposure);

  if (has_flag(push.flags, FLAG_WHITE_BALANCE))
    color *= float3(1.0 + push.temperature, 1.0 + push.tint, 1.0 - push.temperature);

  if (has_flag(push.flags, FLAG_TONEMAP))
    color = tonemap(push.tonemap_mode, color);

  color = saturate(color);

  if (has_flag(push.flags, FLAG_LIFT_GAIN))
    color = color * float3(push.gain_r, push.gain_g, push.gain_b) + float3(push.lift_r, push.lift_g, push.lift_b);

  if (has_flag(push.flags, FLAG_CONTRAST))
    color = (color - 0.5) * push.contrast + 0.5;

  if (has_flag(push.flags, FLAG_SATURATION))
  {
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    color      = lerp(float3(luma), color, push.saturation);
  }

  if (push.lut_strength > 0.0 && push.lut_texture_id != 0xFFFFFFFFu)
  {
    float3 graded = sample_lut(textures[push.lut_texture_id], smp, saturate(color), push.lut_size);
    color         = lerp(color, graded, push.lut_strength);
  }

  if (has_flag(push.flags, FLAG_SEPIA) && push.sepia_intensity > 0.0)
  {
    float3 sepia = float3(dot(color, float3(0.393, 0.769, 0.189)), dot(color, float3(0.349, 0.686, 0.168)),
                          dot(color, float3(0.272, 0.534, 0.131)));
    color        = lerp(color, sepia, push.sepia_intensity);
  }

  if (has_flag(push.flags, FLAG_VIGNETTE))
  {
    float2 d    = uv - 0.5;
    float  dist = length(d) * 1.4142;
    float  v    = smoothstep(push.vignette_smoothness, 1.0, dist);
    color *= 1.0 - v * push.vignette_intensity;
  }

  if (has_flag(push.flags, FLAG_SHARPEN) && push.sharpen_strength > 0.0)
  {
    float3 blur = float3(0.0, 0.0, 0.0);
    blur += src.Load(int3(pix + int2( 1,  0), 0)).rgb;
    blur += src.Load(int3(pix + int2(-1,  0), 0)).rgb;
    blur += src.Load(int3(pix + int2( 0,  1), 0)).rgb;
    blur += src.Load(int3(pix + int2( 0, -1), 0)).rgb;
    color += (color - blur * 0.25) * push.sharpen_strength;
  }

  if (has_flag(push.flags, FLAG_GRAIN) && push.grain_intensity > 0.0)
  {
    float n = hash21(float2(pix), push.time + float(push.frame) * 0.017);
    color += (n - 0.5) * push.grain_intensity;
  }

  color = saturate(color);

  /* Dither sits right before the 8-bit store, so it can break up the banding
     the quantize is about to introduce. */
  if (has_flag(push.flags, FLAG_DITHER))
  {
    float n = hash21(float2(pix) * 0.7331 + 17.0, push.time);
    color += (n - 0.5) * (push.dither_strength / 255.0);
  }

  if (has_flag(push.flags, FLAG_GAMMA))
    color = pow(saturate(color), float3(1.0 / max(push.gamma_value, 1e-4)));

  dst[pix] = float4(color, 1.0);
}
