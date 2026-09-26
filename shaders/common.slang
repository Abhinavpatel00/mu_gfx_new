#ifndef ENGINE_COMMON_SLANG
#define ENGINE_COMMON_SLANG

#include "../src/slangtypes.h"

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::binding(2, 0)]]
RWTexture2D<float4> storage_images[];

[[vk::binding(3, 0)]]
ConstantBuffer<GlobalData> global_data;

[[vk::binding(4, 0)]]
StructuredBuffer<GameEntity> game_entities;

/* y-down pixel position -> clip space for a pass that renders into hdr_color.
   begin_pass installs a negative-height viewport, and the hdr -> swapchain chain
   flips Y an odd number of times (see README, "Coordinate conventions"), so both
   axes get the same scale-and-translate. Passes that draw straight into the
   swapchain need the opposite sign; nuklear.slang carries that form inline. */
float2 pixel_to_uv(float2 pixel, float2 screen)
{
    return (pixel / screen) * 2.0 - 1.0;
}
#endif
