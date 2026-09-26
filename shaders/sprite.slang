#include "common.slang"
#include "../src/two_d/sprite_data.h"

// Must match SPRITE_CPU_BINDING / SPRITE_GPU_BINDING in src/constant.h.
[[vk::binding(5, 0)]] RWByteAddressBuffer sprite_stream;
[[vk::binding(6, 0)]] RWByteAddressBuffer sprite_out;

[[vk::push_constant]] SpritePush pc;

struct SpriteVOut {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct SpriteFIn {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

static float4 unpack_rgba8(uint c)
{
    return float4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, (c >> 24) & 0xFFu) * (1.0f / 255.0f);
}

static float2 unpack_uv(uint packed)
{
    return float2((packed & 0xFFFFu) * (1.0f / 65535.0f), ((packed >> 16) & 0xFFFFu) * (1.0f / 65535.0f));
}

[shader("vertex")]
SpriteVOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    SpriteVOut o;

    uint bases = SPRITE_BATCH_BASE_OFF(pc.cpu_off, pc.block_count, pc.batch_count);
    uint index = sprite_stream.Load(bases + pc.batch * 4u) + iid;
    uint base  = pc.gpu_off + index * SPRITE_INST_BYTES;

    uint4 a = sprite_out.Load4(base);
    uint4 b = sprite_out.Load4(base + 16u);

    float x = asfloat(a.x);
    float y = asfloat(a.y);
    float w = asfloat(a.z);
    float h = asfloat(a.w);

    // 4-vertex triangle strip quad: 0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1)
    float cu = (vid & 1u) != 0u ? 1.0f : 0.0f;
    float cv = vid >= 2u ? 1.0f : 0.0f;

    // Mirror in place: the quad spans the same rect, only the UVs walk
    // backwards. Costs two sign tests per vertex and no CPU-side flip.
    float2 corner = float2(w < 0.0f ? 1.0f - cu : cu, h < 0.0f ? 1.0f - cv : cv);

    float2 pixel = float2(x + cu * abs(w), y + cv * abs(h));
    float2 ndc   = pixel_to_uv(pixel, float2(pc.viewport[0], pc.viewport[1]));

    o.position = float4(ndc, 0.0f, 1.0f);
    o.uv       = lerp(unpack_uv(b.x), unpack_uv(b.y), corner);
    o.color    = unpack_rgba8(b.z);
    return o;
}

[shader("fragment")]
float4 fs_main(SpriteFIn i) : SV_Target
{
    return textures[pc.texture_id].Sample(samplers[pc.sampler_id], i.uv) * i.color;
}
