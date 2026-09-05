import tonemapping;

//===========================================================
// Push constant layout — must match C side exactly
//===========================================================
struct PostPush
{
  // resource IDs
  uint src_texture_id;
  uint output_image_id;
  uint sampler_id;

  // resolution
  uint width;
  uint height;

  uint  frame;
  float exposure;
  // effect enable flags (bitfield)
  uint flags;

  // tonemap selection: 0=neutral 1=aces 2=reinhard 3=reinhard2 4=filmic 5=uncharted2 6=unreal
  uint tonemap_mode;

  // core
  float gamma_value;

  // color grading
  float contrast;
  float saturation;
  float temperature;  // white-balance: negative=cool, positive=warm
  float tint;         // white-balance: green-magenta axis

  // spatial
  float vignette_intensity;
  float vignette_smoothness;
  float sharpen_strength;
  float ca_strength;  // chromatic-aberration

  // noise / output
  float grain_intensity;
  float sepia_intensity;
  float dither_strength;

  // lift / gain (shadows / highlights color wheels)
  float lift_r;
  float lift_g;
  float lift_b;
  float gain_r;
  float gain_g;
  float gain_b;
};

//===========================================================
// Flag bits
//===========================================================
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
#define SLANG_DEFAULT(x) = (x)
__intrinsic_op(cmpGT) public vector<bool, N> greaterThan<T, let N : int>(vector<T, N> x, vector<T, N> y);


inline float3 toSrgb(float3 rgb)
{
  float3 low  = rgb * 12.92f;
  float3 high = fma(pow(rgb, float3(1.0F / 2.4F)), float3(1.055F), float3(-0.055F));
  return lerp(low, high, float3(greaterThan(rgb, float3(0.0031308F))));
}

// Converts a color from sRGB to linear RGB.
inline float3 toLinear(float3 srgb)
{
  float3 low  = srgb / 12.92F;
  float3 high = pow((srgb + float3(0.055F)) / 1.055F, float3(2.4F));
  return lerp(low, high, float3(greaterThan(srgb, float3(0.04045F))));
}

/*-------------------------------------------------------------------------------------------------
# Function `tonemapACES`
> An approximation by Stephen Hill to the Academy Color Encoding System's
> filmic curve for displaying HDR images on LDR output devices.

From https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl,
via https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
-------------------------------------------------------------------------------------------------*/
inline float3 tonemapACES(float3 color)
{
  // Input transform
  const float3x3 ACESInputMat = float3x3(0.59719F, 0.07600F, 0.02840F,   // Row 1
                                         0.35458F, 0.90834F, 0.13383F,   // Row 2
                                         0.04823F, 0.01566F, 0.83777F);  // Row 3
  color                       = mul(color, ACESInputMat);

  // RRT and ODT fit
  float3 a = color * (color + float3(0.0245786F)) - float3(0.000090537F);
  float3 b = color * (float3(0.983729F) * color + float3(0.4329510F)) + float3(0.238081F);
  color    = a / b;  // Always OK because of the large constant term in b's polynomial
  // Output transform
  const float3x3 ACESOutputMat = float3x3(1.60475F, -0.10208F, -0.00327F,  //
                                          -0.53108F, 1.10813F, -0.07276F,  //
                                          -0.07367F, -0.00605F, 1.07602F);
  color                        = mul(color, ACESOutputMat);
  return toLinear(color);
}


//===========================================================
// Bindless resources
//===========================================================
[[vk::push_constant]]
PostPush push;

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::binding(2, 0)]]
RWTexture2D<float4> storage_images[];

[numthreads(16, 16, 1)]
void cs_main(uint3 id: SV_DispatchThreadID)
{
  if(id.x >= push.width || id.y >= push.height)
    return;

  int2 pix = int2(id.xy);

  Texture2D           src = textures[push.src_texture_id];
  RWTexture2D<float4> dst = storage_images[push.output_image_id];

float3 color = src.Load(int3(pix, 0)).rgb;

// exposure control
color *= exp2(push.exposure);

color = tonemapACES(color);


//

  dst[pix] = float4(color, 1.0);
}

