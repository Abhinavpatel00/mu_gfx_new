#ifndef MU_LIGHTING_SLANG
#define MU_LIGHTING_SLANG

/* lighting.slang — the environment both scene pipelines shade against.

   It reads what the renderer publishes in GlobalData, so the sky pass and the
   PBR and toon fragment stages all describe the same sun and the same ambient
   sky. Keeping the numbers in one published block is the point: a scene lit by
   an ambient sky that is not the sky it stands in front of reads as a cutout. */

#include "common.slang"

static const float LIGHT_INV_PI = 0.3183098861837907;

// Direction toward the sun, normalized. The cull never needs it; the shading does.
float3 env_sun_dir() { return normalize(global_data.sun_dir.xyz); }

// Sun radiance: tint times intensity, the radiance the direct term scales by.
float3 env_sun_radiance() { return global_data.sun_color.rgb * global_data.sun_dir.w; }

float3 env_sky_color() { return global_data.env_sky.rgb; }

// Hemisphere ambient. Sky above, ground bounce below, blended by the normal's
// elevation; env_sky.w scales the whole term so a preset can trade brightness
// for color without touching either stop. ao is the screen-space occlusion.
float3 env_ambient(float3 n, float ao) {
    float  up = saturate(n.y * 0.5 + 0.5);
    float3 c  = lerp(global_data.env_ground.rgb, global_data.env_sky.rgb, up);
    return c * (global_data.env_sky.w * ao);
}

// Hemispheric diffuse irradiance, no occlusion applied: the caller owns where
// the ao multiplies in, so the same irradiance can feed the diffuse and a
// separate specular term without baking ao into both.
float3 env_diffuse_irradiance(float3 n) {
    float up = saturate(n.y * 0.5 + 0.5);
    return lerp(global_data.env_ground.rgb, global_data.env_sky.rgb, up) * global_data.env_sky.w;
}

// Approximate prefiltered reflection: the sharp sky/horizon/ground gradient
// along the reflected direction, blended toward the environment average as
// roughness rises. The cheapest stand-in for a roughness-blurred cubemap, and
// it reads the same palette the sky pass draws.
float3 env_reflection_color(float3 r, float roughness) {
    float  up    = saturate(r.y * 0.5 + 0.5);
    float3 sharp = lerp(global_data.env_ground.rgb, global_data.env_horizon.rgb, saturate(up * 2.0));
    sharp        = lerp(sharp, global_data.env_sky.rgb, saturate(up * 2.0 - 1.0));
    float3 avg   = (global_data.env_sky.rgb + global_data.env_horizon.rgb + global_data.env_ground.rgb) * (1.0 / 3.0);
    return lerp(sharp, avg, roughness) * global_data.env_sky.w;
}

// Analytic split-sum environment BRDF (Karis "EnvBRDFApprox"): a fitted
// polynomial returning (A, B) with the specular irradiance response
// f0 * A + B. A fit instead of a lookup texture, so no extra bindless slot.
float2 env_brdf_approx(float roughness, float ndv) {
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4       r  = roughness * c0 + c1;
    float        a  = min(r.x * r.x, exp2(-9.28f * ndv)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a + r.zw;
}

// Frostbite horizon specular occlusion: a metal highlight in a crease (low ao)
// is hidden while one on a surface facing the camera survives.
float env_specular_occlusion(float ao, float ndv, float roughness) {
    return saturate(pow(ndv + ao, exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao);
}

#endif
