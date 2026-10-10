/* Fullscreen vertical sky gradient. Drawn into the HDR target before the
   scene, which then loads instead of clearing, so the horizon stays behind
   geometry without needing a depth test. */
#include "common.slang"

struct SkyPush
{
  float4 top;
  float4 bottom;
  float  exponent;
  float  pad0;
  float  pad1;
  float  pad2;
};

[[vk::push_constant]]
SkyPush push;

struct SkyVSOut
{
  float4 position : SV_Position;
};

[shader("vertex")]
SkyVSOut vs_main(uint id : SV_VertexID)
{
  static const float2 v[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
  SkyVSOut o;
  o.position = float4(v[id], 0.0, 1.0);
  return o;
}

[shader("fragment")]
float4 fs_main(SkyVSOut input) : SV_Target0
{
  float2 uv = pixel_to_uv(input.position.xy, global_data.screen_params.xy) * 0.5 + 0.5;
  float  t  = pow(saturate(uv.y), push.exponent);
  return float4(lerp(push.bottom.rgb, push.top.rgb, t), 1.0);
}
