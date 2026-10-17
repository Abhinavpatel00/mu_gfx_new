/* sort.slang — T2.5: depth-bucket sort of the survivor list.

   The opaque draw grants early-Z rejection only to fragments whose depth
   buffer already holds a nearer value, and that requires NEARER instances to
   raster FIRST. The compaction (cs_count..cs_scatter) preserves survivor-list
   order inside each (mesh,lod) group's contiguous range, so reordering ONLY
   the survivor list by camera depth turns every group's instances into a
   front-to-back run with the rest of the pipeline untouched.

   Sort shape: counting sort into 64 linear depth buckets over a 0..256 m
   frustum span (4 m per bucket — intra-bucket pairs are effectively at the
   same depth, so their mutual overdraw stays as-is; that residual is cheap
   relative to length-scale overlaps). Near-first, because reverse-Z makes the
   nearer value GREATER: a later fragment only survives a depth test against a
   value it can beat.

   The camera (position + forward) comes from the global constant block, like
   the cull's LOD metric — data the cull's inputs already carry. Kernels:
     cs_sort_clear   zero hist|cur (128 words, one workgroup)
     cs_sort_hist    per-bucket histogram (workgroup SMEM, one global fan-in)
     cs_sort_scan    exclusive scan of 64 buckets into cur[] (pure SMEM)
     cs_sort_scatter atomic cursor; dst = cur[b] + idx (cur[b] = bucket start)

   Every consumer re-derives the bucket from the instance row, so there is no
   bucket side channel to keep in sync (1NF: one source of truth).
   numthreads match cs_count so adjacent passes do not flip the scheduler's
   wave cadence. Note: the aux layout is hist[SORT_BUCKETS] then cur[],
   128 words total; cross-lane global-write visibility inside one dispatch is
   NOT assumed (group barriers only order shared memory), so the scan keeps
   its prefix entirely in shared memory. */
#include "common.slang"
#include "../src/three_d/scene_shared.h"

[[vk::push_constant]] SceneSortPush pc;

#define SORT_BUCKETS   64
#define SORT_RANGE     256.0f
#define SORT_CUR_BASE  SORT_BUCKETS
#define SORT_AUX_WORDS (SORT_BUCKETS * 2)

static uint bucket_of(uint key)
{
    uint   slot = key & 0xFFFFu;
    float3 delta = float3(pc.instances[slot].pos[0], pc.instances[slot].pos[1], pc.instances[slot].pos[2]) -
                   global_data.camera_pos.xyz;
    /* Signed forward depth; anything behind the near plane sorts first
       (max(.., 0)) where it cannot hurt: it was likely culled anyway. */
    float d = max(dot(delta, global_data.camera_dir.xyz), 0.0f);
    return min((uint)(d * (SORT_BUCKETS / SORT_RANGE)), SORT_BUCKETS - 1u);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void cs_sort_clear(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= SORT_AUX_WORDS)
        return;
    pc.sort_aux[tid.x] = 0u;
}

groupshared uint s_hist[SORT_BUCKETS];
groupshared uint s_scan[SORT_BUCKETS];

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_sort_hist(uint3 tid : SV_DispatchThreadID, uint lid : SV_GroupThreadID)
{
    /* All lanes join: an early return in the partial workgroup would skip a
       group barrier and hang the GPU. Empty lanes just contribute zero. */
    s_hist[lid] = 0u;
    GroupMemoryBarrierWithGroupSync();

    uint n = min(pc.survivor_count[0], pc.counts[0]);
    if (tid.x < n)
        InterlockedAdd(s_hist[bucket_of(pc.survivors[tid.x])], 1u);
    GroupMemoryBarrierWithGroupSync();

    /* One global atomic per lane: a 407-workgroup histogram fans into 64
       counters at group size instead of thread size. */
    InterlockedAdd(pc.sort_aux[lid], s_hist[lid]);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_sort_scan(uint3 tid : SV_DispatchThreadID, uint lid : SV_GroupThreadID)
{
    /* Exactly 64 lanes; exclusive scan stays in shared memory so no cross-
       lane global read (which a group barrier would not order). */
    s_scan[lid] = pc.sort_aux[lid];
    GroupMemoryBarrierWithGroupSync();
    for (uint off = 1u; off < SORT_BUCKETS; off <<= 1u) {
        uint a = 0u;
        if (lid >= off)
            a = s_scan[lid - off];
        GroupMemoryBarrierWithGroupSync();
        s_scan[lid] += a;
        GroupMemoryBarrierWithGroupSync();
    }
    /* exclusive prefix = inclusive scan minus this bucket's own count. */
    uint mine = pc.sort_aux[lid];
    pc.sort_aux[SORT_CUR_BASE + lid] = s_scan[lid] - mine;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_sort_scatter(uint3 tid : SV_DispatchThreadID)
{
    uint n = min(pc.survivor_count[0], pc.counts[0]);
    if (tid.x >= n)
        return;
    uint key = pc.survivors[tid.x];
    uint b   = bucket_of(key);
    uint dst; /* atomic returns the old cursor = bucket start + # placed */
    InterlockedAdd(pc.sort_aux[SORT_CUR_BASE + b], 1u, dst);
    pc.survivors_sorted[dst] = key;
}
