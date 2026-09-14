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

float2 pixel_to_uv(float2 pixel, float2 screen)
{
    float2 uv = (pixel / screen) * 2.0 - 1.0;
    uv.x *= screen.x / screen.y;
    return uv;
}
#endif
