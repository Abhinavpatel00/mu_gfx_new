/* compact.slang — T3: turn the unordered survivor list into a dense command
   array and a dense visible array (counting sort over (mesh,lod) groups).

   cs_count       histogram survivors by group
   cs_scan_block  per-block exclusive scan of (count, emit) pairs
   cs_scan_blocks scan the per-block totals, producing block offsets
   cs_emit        apply block offsets, write draw commands, zero scatter cursors
   cs_scatter     scatter survivors into their group's contiguous vis range



   The scan carries two lanes because two exclusive scans fall out of one pass:
   vis_base wants the running sum of instance counts, cmd_index wants the
   running sum of "this group is non-empty". Scanning them together halves the
   shared-memory traffic and the barrier count versus running the pass twice.

   The previous version ran this as [numthreads(1,1,1)]: one thread looping over
   every group, serialized between the cull and the scatter. For a scene with
   ~600 groups that is a few microseconds of single-thread latency per view per
   frame. Here SCAN_THREADS * SCAN_ELEMS groups are handled per workgroup.

   Scan shape: a thread owns SCAN_ELEMS groups at the *strided* indices
   lid.x + e*SCAN_THREADS, which keeps the group_count loads coalesced. Those
   indices are not contiguous, so a single block-wide Hillis-Steele over all
   SCAN_BLOCK slots cannot be turned into per-element exclusives by subtraction
   afterwards — index lid.x's inclusive prefix is not this thread's segment.
   The correct shape is scan-then-propagate: Hillis-Steele over the 256
   per-thread aggregates, then each thread walks its own elements serially from
   its exclusive offset. Kept in registers, so the second pass costs no shared
   memory and no second read of group_count.

   Each entry is its own .spv; create_compute_pipeline() only ever binds "main",
   so a file with several kernels needs one .spv per entry. */
#include "common.slang"
#include "../src/three_d/scene_shared.h"

[[vk::push_constant]] SceneCompactPush pc;

#define SCAN_THREADS 256
#define SCAN_ELEMS   4
#define SCAN_BLOCK   (SCAN_THREADS * SCAN_ELEMS)

static uint group_of(uint key)
{
    uint mesh = (key >> 16) & 0x3FFFu;
    uint lod  = key >> 30;
    return mesh * pc.counts[0] + lod;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_count(uint3 tid : SV_DispatchThreadID)
{
    uint n = min(pc.survivor_count[0], pc.counts[2]);
    if (tid.x >= n)
        return;
    uint g = group_of(pc.survivors[tid.x]);
    InterlockedAdd(pc.group_count[g], 1u);
}

/* ------------------------------------------------------------------ *
 * The one piece both scan levels share: an exclusive Hillis-Steele over
 * the SCAN_THREADS per-thread aggregates. Returns this thread's exclusive
 * offset and leaves s_thread holding the inclusive totals.
 * ------------------------------------------------------------------ */
groupshared uint2 s_thread[SCAN_THREADS];

static uint2 thread_exclusive(uint2 v, uint lid)
{
    s_thread[lid] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint off = 1u; off < SCAN_THREADS; off <<= 1u)
    {
        uint2 t = (lid >= off) ? s_thread[lid - off] : uint2(0u, 0u);
        GroupMemoryBarrierWithGroupSync();
        s_thread[lid] += t;
        GroupMemoryBarrierWithGroupSync();
    }
    uint2 incl = s_thread[lid];
    GroupMemoryBarrierWithGroupSync();
    return incl - v;
}

[shader("compute")]
[numthreads(SCAN_THREADS, 1, 1)]
void cs_scan_block(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    uint G    = pc.counts[1];
    uint base = gid.x * SCAN_BLOCK;

    uint2 vals[SCAN_ELEMS];
    uint2 v   = uint2(0u, 0u);
    for (uint e = 0u; e < SCAN_ELEMS; ++e)
    {
uint g = base + lid.x * SCAN_ELEMS + e;

uint c = (g < G) ? pc.group_count[g] : 0u;
        vals[e] = uint2(c, (c != 0u) ? 1u : 0u);
        v += vals[e];
    }

    uint2 run = thread_exclusive(v, lid.x);
    for (uint e = 0u; e < SCAN_ELEMS; ++e)
    {
       uint g = base + lid.x * SCAN_ELEMS + e;


       if (g < G)
        {
            /* block-local for now; cs_emit adds the block offset. vis_base and
               cmd_index are two different scans, so two different arrays —
               cursor[] is reserved for the scatter position and must stay
               available to cs_scatter at zero. */
            pc.vis_base[g] = run.x;
            pc.cmd_index[g] = run.y;
        }
        run += vals[e];
    }

    if (lid.x == 0u)
        pc.scan_aux[gid.x] = s_thread[SCAN_THREADS - 1u];
}

/* Scan the per-block totals. One workgroup: SCAN_BLOCK blocks covers
   SCAN_BLOCK * SCAN_BLOCK groups, which scene_create asserts against. */
[shader("compute")]
[numthreads(SCAN_THREADS, 1, 1)]
void cs_scan_blocks(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    if (gid.x != 0u)
        return;

    /* Level 2 reads the block totals cs_scan_block wrote into region 0. The
       block offsets it produces go to regions 1 and 2, which are disjoint from
       region 0, so read-then-write in one dispatch is safe. */
    uint nb = pc.counts[4];
    uint2 vals[SCAN_ELEMS];
    uint2 v   = uint2(0u, 0u);
    for (uint e = 0u; e < SCAN_ELEMS; ++e)
    {
        uint b = lid.x + e * SCAN_THREADS;
        vals[e] = (b < nb) ? pc.scan_aux[b] : uint2(0u, 0u);
        v += vals[e];
    }

    uint2 run = thread_exclusive(v, lid.x);
    for (uint e = 0u; e < SCAN_ELEMS; ++e)
    {
        uint b = lid.x + e * SCAN_THREADS;
        if (b < nb)
        {
            pc.scan_aux[1u * SCAN_BLOCK + b] = run.x;
            pc.scan_aux[2u * SCAN_BLOCK + b] = run.y;
        }
        run += vals[e];
    }

    if (lid.x == 0u)
    {
        pc.draw_count[0] = s_thread[SCAN_THREADS - 1u].y;
        /* Mirror the command count into the counter ring so the lag-2 readback
           reports it. This is the number a CPU reference can be checked
           against: draw_count must equal the number of non-empty groups. */
        pc.counters[SCENE_COUNTER_DRAWS] = s_thread[SCAN_THREADS - 1u].y;
    }
}

[shader("compute")]
[numthreads(SCAN_THREADS, 1, 1)]
void cs_emit(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    uint G    = pc.counts[1];
    uint base = gid.x * SCAN_BLOCK;
    uint2 off = uint2(pc.scan_aux[1u * SCAN_BLOCK + gid.x].x, pc.scan_aux[2u * SCAN_BLOCK + gid.x].y);

    for (uint e = 0u; e < SCAN_ELEMS; ++e)
    {
        uint g = base + lid.x + e * SCAN_THREADS;
        if (g >= G)
            break; /* gid-major: once past the end every later e is past it too */
        pc.vis_base[g] += off.x;
        uint cmd = pc.cmd_index[g] + off.y;
        pc.cmd_index[g] = cmd;
        pc.cursor[g]    = 0u; /* scatter starts at the group's vis base */
        if (pc.group_count[g] != 0u)
        {
            SceneGpuDraw d = pc.group_static[g];
            d.instance_count = pc.group_count[g];
            d.first_instance = pc.vis_base[g];
            pc.draws[cmd]    = d;
            /* The VS has no DrawIndex in this Slang build, so it recovers the
               group from the draw's own instance base. Both sides derive it from
               vis_base[g], which is uniform across the draw — a broadcast, not
               a per-lane gather. */
            pc.group_base[pc.vis_base[g]] = g;
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_scatter(uint3 tid : SV_DispatchThreadID)
{
    uint n = min(pc.survivor_count[0], pc.counts[2]);
    if (tid.x >= n)
        return;
    uint key   = pc.survivors[tid.x];
    uint g     = group_of(key);
    uint local;
    InterlockedAdd(pc.cursor[g], 1u, local);
    pc.vis[pc.vis_base[g] + local] = key & 0xFFFFu;
}
