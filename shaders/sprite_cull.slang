// GPU cull chain for the sprite stream: count, prefix, compact.
// Each kernel is its own entry point so each one gets its own .spv;
// create_compute_pipeline() binds the SPIR-V entry point named "main".

#include "common.slang"
#include "../src/two_d/sprite_data.h"

// Must match SPRITE_CPU_BINDING / SPRITE_GPU_BINDING in src/constant.h.
[[vk::binding(5, 0)]] RWByteAddressBuffer sprite_stream;
[[vk::binding(6, 0)]] RWByteAddressBuffer sprite_out;

[[vk::push_constant]] SpritePush pc;

// Negative w/h mirror the sprite in place; the quad still occupies
// [x, x+abs(w)] so the caller's anchor does not move.
static bool sprite_visible(float x, float y, float w, float h)
{
    w = abs(w);
    h = abs(h);
    if (w == 0.0f || h == 0.0f)
        return false;
    if (x + w <= 0.0f || x >= pc.viewport[0])
        return false;
    if (y + h <= 0.0f || y >= pc.viewport[1])
        return false;
    return true;
}

// One workgroup per 256-sprite block: how many sprites survive the view test.
groupshared uint g_block[256];

[shader("compute")]
[numthreads(256, 1, 1)]
void cs_count(uint3 gid : SV_GroupID, uint gtid : SV_GroupIndex)
{
    uint block = gid.x;
    uint src   = pc.cpu_off + (block * SPRITE_BLOCK_SIZE + gtid) * SPRITE_INST_BYTES;

    uint4 a = sprite_stream.Load4(src);
    bool vis = sprite_visible(asfloat(a.x), asfloat(a.y), asfloat(a.z), asfloat(a.w));

    g_block[gtid] = vis ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();

    for (uint offset = 128u; offset != 0u; offset >>= 1u)
    {
        if (gtid < offset)
            g_block[gtid] += g_block[gtid + offset];
        GroupMemoryBarrierWithGroupSync();
    }

    if (gtid == 0u)
        sprite_stream.Store(SPRITE_VIS_OFF(pc.cpu_off, pc.block_count) + block * 4u, g_block[0]);
}

// One workgroup for the whole flush. Phase A turns the per-block counts into
// block_base[]. Phase B derives each batch's output base and its indirect
// command from block_base at the batch's padded block boundary.
groupshared uint g_chunk[64];

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_prefix(uint3 gid : SV_GroupID, uint gtid : SV_GroupIndex)
{
    uint bc = pc.block_count;
    uint nb = pc.batch_count;

    uint vis_off    = SPRITE_VIS_OFF(pc.cpu_off, bc);
    uint block_off  = SPRITE_BLOCK_BASE_OFF(pc.cpu_off, bc);
    uint fblock_off = SPRITE_FBLOCK_OFF(pc.cpu_off, bc);
    uint bases_off  = SPRITE_BATCH_BASE_OFF(pc.cpu_off, bc, nb);
    uint indir_off  = SPRITE_INDIRECT_OFF(pc.cpu_off, bc, nb);

    uint chunk = (bc + 63u) / 64u;
    uint lo    = gtid * chunk;
    uint hi    = min(lo + chunk, bc);

    uint sum = 0u;
    for (uint i = lo; i < hi; ++i)
        sum += sprite_stream.Load(vis_off + i * 4u);
    g_chunk[gtid] = sum;
    GroupMemoryBarrierWithGroupSync();

    // Exclusive scan of the 64 chunk sums (inclusive result lives in g_chunk).
    for (uint offset = 1u; offset < 64u; offset <<= 1u)
    {
        uint cur  = g_chunk[gtid];
        uint add  = gtid >= offset ? g_chunk[gtid - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        g_chunk[gtid] = cur + add;
        GroupMemoryBarrierWithGroupSync();
    }

    uint prefix = gtid != 0u ? g_chunk[gtid - 1u] : 0u;
    uint total  = g_chunk[63u];
    uint local  = 0u;
    for (uint i = lo; i < hi; ++i)
    {
        sprite_stream.Store(block_off + i * 4u, prefix + local);
        local += sprite_stream.Load(vis_off + i * 4u);
    }
    if (gtid == 0u)
        sprite_stream.Store(block_off + bc * 4u, total);

    // Every thread finished writing block_base; publish it before phase B reads
    // it back through storage.
    AllMemoryBarrierWithGroupSync();

    for (uint b = gtid; b < nb; b += 64u)
    {
        uint fb = sprite_stream.Load(fblock_off + b * 4u);
        uint fe = sprite_stream.Load(fblock_off + (b + 1u) * 4u);
        uint s  = sprite_stream.Load(block_off + fb * 4u);
        uint e  = sprite_stream.Load(block_off + fe * 4u);
        uint o  = indir_off + b * 16u;

        sprite_stream.Store(bases_off + b * 4u, s);
        sprite_stream.Store(o + 0u, 4u);  // vertexCount, triangle strip quad
        sprite_stream.Store(o + 4u, e - s); // instanceCount
        sprite_stream.Store(o + 8u, 0u);  // firstVertex
        sprite_stream.Store(o + 12u, 0u); // firstInstance
    }
}

// One workgroup per block: pack the survivors of this block behind the
// survivors of all earlier blocks. block_base[g] + local prefix is monotonic in
// g, so painter order survives compaction untouched.
groupshared uint g_scan[256];

[shader("compute")]
[numthreads(256, 1, 1)]
void cs_compact(uint3 gid : SV_GroupID, uint gtid : SV_GroupIndex)
{
    uint block = gid.x;
    uint src   = pc.cpu_off + (block * SPRITE_BLOCK_SIZE + gtid) * SPRITE_INST_BYTES;

    uint4 a = sprite_stream.Load4(src);
    uint4 b = sprite_stream.Load4(src + 16u);

    bool vis = sprite_visible(asfloat(a.x), asfloat(a.y), asfloat(a.z), asfloat(a.w));

    g_scan[gtid] = vis ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();

    for (uint offset = 1u; offset < 256u; offset <<= 1u)
    {
        uint cur = g_scan[gtid];
        uint add = gtid >= offset ? g_scan[gtid - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        g_scan[gtid] = cur + add;
        GroupMemoryBarrierWithGroupSync();
    }

    if (vis)
    {
        uint prefix   = gtid != 0u ? g_scan[gtid - 1u] : 0u;
        uint base     = sprite_stream.Load(SPRITE_BLOCK_BASE_OFF(pc.cpu_off, pc.block_count) + block * 4u);
        uint dst      = pc.gpu_off + (base + prefix) * SPRITE_INST_BYTES;
        sprite_out.Store4(dst, a);
        sprite_out.Store4(dst + 16u, b);
    }
}
