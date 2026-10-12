/* Fullscreen sky gradient, drawn into the HDR target before the scene. The
   scene then loads instead of clearing, so the horizon stays behind geometry
   without needing a depth test.

   Color comes from the environment in GlobalData, not from this file: the same
   block the scene pipelines light against. Shape stays here, because how fast
   the gradient falls off is a look knob, not a fact about the world. */
#include "common.slang"

struct SkyPush
{
  float exponent;      // vertical falloff of the zenith-to-horizon blend
  float glow_strength;
  float glow_power;    // higher = tighter halo around the sun
  float ground_fade;   // how far below the horizon the sky turns to ground
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
  float2 uv  = pixel_to_uv(input.position.xy, global_data.screen_params.xy) * 0.5 + 0.5;
  float2 ndc = uv * 2.0 - 1.0;

  /* Build the view ray from the camera basis rather than unprojecting through
     inv_viewproj. The projection is reverse-Z with an infinite far plane, and
     unprojecting a clip point through it lands on a degenerate w here — the
     direction came back parallel to the horizon and the sky rendered as one
     flat band. inv_view holds the basis directly: its columns are right, up and
     back, so the forward axis is the negated third column. */
  float3 right = global_data.inv_view[0].xyz;
  float3 up    = global_data.inv_view[1].xyz;
  float3 fwd   = -global_data.inv_view[2].xyz;
  /* The hdr target is rendered through a negative-height viewport, so ndc.y
     already counts upward and needs no flip. */
  float  tan_v = abs(1.0 / global_data.projection[1][1]);
  float  tan_h = abs(1.0 / global_data.projection[0][0]);
  float3 dir   = normalize(ndc.x * tan_h * right + ndc.y * tan_v * up + fwd);

  float  t   = pow(saturate(dir.y), push.exponent);
  float3 col = lerp(global_data.env_horizon.rgb, global_data.env_sky.rgb, t);

  /* Below the horizon the sky becomes the ground bounce, so a camera angled
     down sees the world fade out rather than a hard seam at y = 0. */
  col = lerp(col, global_data.env_ground.rgb, saturate(-dir.y * 2.0) * push.ground_fade);

  float  sun  = saturate(dot(dir, normalize(global_data.sun_dir.xyz)));
  float3 glow = global_data.sun_color.rgb * global_data.sun_dir.w *
                (pow(sun, push.glow_power) * push.glow_strength);

  /* Alpha is the per-instance bloom gate: sky never blooms. */
  return float4(col + glow, 0.0);
}
