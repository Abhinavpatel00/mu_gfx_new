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


#endif
