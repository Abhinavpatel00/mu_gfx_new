// Hierarchical depth (Hi-Z) pyramid build, one level per dispatch.
//
// Reverse-Z puts the near plane at 1 and infinity at 0, so the conservative
// "farthest surface in this tile" bound is the tile MIN. A tile touching
// background (cleared depth 0) reduces to 0 and then never proves occlusion,
// which is the safe direction. Each dispatch reads the previous level through a
// plain texture fetch and writes one texel per 2x2 source texels.
#include "common.slang"

struct HizPush {
    uint  src;      // bindless sampled index: scene depth for level 0, else level-1
    uint  dst;      // bindless storage index of the level being written
    uint2 src_size; // texels of the source level
    uint2 dst_size; // texels of this level
    uint2 pad;
};
[[vk::push_constant]] HizPush hiz;

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_hiz(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= hiz.dst_size.x || tid.y >= hiz.dst_size.y) return;

    int2 base = int2(tid.xy) * 2;
    int2 last = int2(hiz.src_size) - 1;
    // 1.0 is the largest depth reverse-Z can store, so it is the identity for
    // the min reduction.
    float m = 1.0;
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            int2 s = min(base + int2(x, y), last);
            m = min(m, textures[hiz.src].Load(int3(s, 0)).x);
        }
    }
    storage_images[hiz.dst][int2(tid.xy)] = float4(m, m, m, 1.0);
}
