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

#endif
