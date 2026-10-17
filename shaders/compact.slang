/* compact.slang — T3: turn the unordered survivor list into a dense command
   array and a dense visible array (counting sort over (mesh,lod) groups).

   cs_count       histogram survivors by group
   cs_scan_block  per-block exclusive scan of (count, emit) pairs
   cs_scan_blocks scan the per-block totals, producing block offsets
   cs_emit        apply block offsets, write draw commands, zero scatter cursors
   cs_scatter     scatter survivors into their group's contiguous vis range



   The scan carries three lanes because three exclusive scans fall out of one
   pass: vis_base wants the running sum of instance counts, cmd_index wants the
   running sum of "this group is non-empty", and the opaque/alpha route split
   wants that second scan twice — once restricted to alpha-test materials
   (late-Z pass) and once to the rest (early-Z pass). The two emit lanes are
   mutually exclusive, so each non-empty group takes its slot number from the
   lane matching its material class and cmd_index ends up holding the
   route-local index directly. Scanning the lanes together halves the
   shared-memory traffic and the barrier count versus running the pass twice.
   In combined mode (sort path, counts[6] == 0) the alpha lane stays empty and
   every non-empty group lands in the one draw list, exactly as before.

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

/* Thread t owns the strided lanes t + e*SCAN_THREADS, matching cs_emit. */
#define GROUP_IDX(base, lid, e) ((base) + (lid) + (e) * SCAN_THREADS)

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
groupshared uint3 s_thread[SCAN_THREADS];

static uint3 thread_exclusive(uint3 v, uint lid)
{
    s_thread[lid] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint off = 1u; off < SCAN_THREADS; off <<= 1u)
    {
        uint3 t = (lid >= off) ? s_thread[lid - off] : uint3(0u, 0u, 0u);
        GroupMemoryBarrierWithGroupSync();
        s_thread[lid] += t;
        GroupMemoryBarrierWithGroupSync();
    }
    uint3 incl = s_thread[lid];
    GroupMemoryBarrierWithGroupSync();
    return incl - v;
}

[shader("compute")]
[numthreads(SCAN_THREADS, 1, 1)]
void cs_scan_block(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    uint G    = pc.counts[1];
    uint base = gid.x * SCAN_BLOCK;

    uint3 vals[SCAN_ELEMS];
    uint3 v   = uint3(0u, 0u, 0u);
    uint  sp  = pc.counts[6];
    for (uint e = 0u; e < SCAN_ELEMS; ++e)
    {
        uint g = GROUP_IDX(base, lid.x, e);

        uint c = (g < G) ? pc.group_count[g] : 0u;
        /* Route split: alpha-test materials take the late-Z pass, the rest the
         * early-Z one. The two emit lanes are mutually exclusive, so
         * cmd_index[g] stores whichever index the group's own route produced.
         * Grouped into one bool here so out-of-range tail groups cannot fetch
         * alpha_bits past G. */
        bool am = (g < G) && (sp != 0u) && (pc.alpha_bits[g] != 0u);
        bool nz = (c != 0u);
        vals[e] = uint3(c, (nz && !am) ? 1u : 0u, (nz && am) ? 1u : 0u);
        v += vals[e];
    }

    uint3 run = thread_exclusive(v, lid.x);
    for (uint e = 0u; e < SCAN_ELEMS; ++e)
    {
       uint g = GROUP_IDX(base, lid.x, e);


       if (g < G)
        {
            /* block-local for now; cs_emit adds the block offset. vis_base and
               cmd_index are two different scans, so two different arrays —
               cursor[] is reserved for the scatter position and must stay
               available to cs_scatter at zero. */
            pc.vis_base[g] = run.x;
            pc.cmd_index[g] = ((sp != 0u) && (pc.alpha_bits[g] != 0u)) ? run.z : run.y;
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
       block offsets it produces go to regions 1..3, which are disjoint from
       region 0, so read-then-write in one dispatch is safe. */
    uint nb = pc.counts[4];
    uint3 vals[SCAN_ELEMS];
    uint3 v   = uint3(0u, 0u, 0u);
    for (uint e = 0u; e < SCAN_ELEMS; ++e)
    {
        uint b = lid.x + e * SCAN_THREADS;
        vals[e] = (b < nb) ? pc.scan_aux[b] : uint3(0u, 0u, 0u);
        v += vals[e];
    }

    uint3 run = thread_exclusive(v, lid.x);
    for (uint e = 0u; e < SCAN_ELEMS; ++e)
    {
        uint b = lid.x + e * SCAN_THREADS;
        if (b < nb)
        {
            /* Region 1: vis (instance count) offsets, broadcast. Region 2:
               opaque draw offsets. Region 3: alpha draw offsets. */
            pc.scan_aux[1u * SCAN_BLOCK + b] = uint3(run.x, run.x, run.x);
            pc.scan_aux[2u * SCAN_BLOCK + b] = uint3(run.y, run.y, run.y);
            pc.scan_aux[3u * SCAN_BLOCK + b] = uint3(run.z, run.z, run.z);
        }
        run += vals[e];
    }

    if (lid.x == 0u)
    {
        /* The inclusive totals the scan already produced answer for every
           counter the cull used to accumulate with an atomic. Drawn is the sum
           of the per-group instance counts; dropped was the one count with no
           place in this pass, so the cull still bumps it and it is folded in
           here. Frustum-rejected is then just what is left over, and Hi-Z is
           not implemented, so it is a constant.

           Doing it this way is the difference between one atomic per candidate
           and one write per frame, on a counter block small enough that every
           thread in the dispatch shares a cache line. */
        uint3 total = s_thread[SCAN_THREADS - 1u];
        uint  drawn = total.x;
        uint  drop  = pc.counters[SCENE_COUNTER_DROPPED];
        pc.counters[SCENE_COUNTER_DRAWN]   = drawn;
        pc.counters[SCENE_COUNTER_FRUSTUM] = pc.counts[3] - drawn - drop;
        pc.counters[SCENE_COUNTER_HIZ]     = 0u;
        pc.counters[SCENE_COUNTER_DRAWS]   = total.y + total.z;
        pc.draw_count[0]                   = total.y;
        pc.draw_count_alpha[0]             = total.z;

        /* The per-LOD totals are accumulated by cs_emit, which runs after this
           dispatch, so this is where they start from. Storing DRAWN and friends
           above is an overwrite; these four need clearing because cs_emit adds
           into them and several groups share one bucket. Clearing them here
           costs one thread's worth of stores and saves a per-frame fill of the
           whole counter block. */
        pc.counters[SCENE_COUNTER_LOD0 + 0u] = 0u;
        pc.counters[SCENE_COUNTER_LOD0 + 1u] = 0u;
        pc.counters[SCENE_COUNTER_LOD0 + 2u] = 0u;
        pc.counters[SCENE_COUNTER_LOD0 + 3u] = 0u;
    }
}

[shader("compute")]
[numthreads(SCAN_THREADS, 1, 1)]
void cs_emit(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    uint G    = pc.counts[1];
    uint base = gid.x * SCAN_BLOCK;
    uint off  = pc.scan_aux[1u * SCAN_BLOCK + gid.x].x;
    uint offO = pc.scan_aux[2u * SCAN_BLOCK + gid.x].x;
    uint offA = pc.scan_aux[3u * SCAN_BLOCK + gid.x].x;
    uint sp   = pc.counts[6];

    for (uint e = 0u; e < SCAN_ELEMS; ++e)
    {
        uint g = GROUP_IDX(base, lid.x, e);
        if (g >= G)
            continue; /* strided: later e can still be in range */
        pc.vis_base[g] += off;
        pc.cursor[g]    = 0u; /* scatter starts at the group's vis base */
        if (pc.group_count[g] != 0u)
        {
            /* One prefix per route: the group's cmd_index already holds the
               route-local block-local index, so it adds the matching block
               offset and lands in that route's command array. */
            bool am         = (sp != 0u) && (pc.alpha_bits[g] != 0u);
            uint cmd        = pc.cmd_index[g] + (am ? offA : offO);
            pc.cmd_index[g] = cmd;

            SceneGpuDraw d = pc.group_static[g];
            d.instance_count = pc.group_count[g];
            d.first_instance = pc.vis_base[g];
            if (am)
                pc.draws_alpha[cmd] = d;
            else
                pc.draws[cmd] = d;
            /* The VS has no DrawIndex in this Slang build, so it recovers the
               group from the draw's own instance base. Both sides derive it from
               vis_base[g], which is uniform across the draw — a broadcast, not
               a per-lane gather. */
            pc.group_base[pc.vis_base[g]] = g;
            /* Per-LOD totals are G atomic adds at most, against one per drawn
               candidate in the cull: same information, one thousandth of the
               contention on the counter line. */
            InterlockedAdd(pc.counters[SCENE_COUNTER_LOD0 + min(g % pc.counts[0], 3u)], pc.group_count[g]);
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
/* Scatter also builds the per-survivor SceneVsRow: the draw's vertex shader
   then does ONE dependent table load per vertex instead of chasing seven
   per-slot tables keyed on the slot. The random loads here run once per
   survivor; in the VS they ran once per vertex — 100x more often. */
void cs_scatter(uint3 tid : SV_DispatchThreadID)
{
    uint n = min(pc.survivor_count[0], pc.counts[2]);
    if (tid.x >= n)
        return;
    uint key = pc.survivors[tid.x];
    uint g   = group_of(key);
    uint local;
    InterlockedAdd(pc.cursor[g], 1u, local);
    uint dst  = pc.vis_base[g] + local;
    uint slot = key & 0xFFFFu;

    SceneInstance inst = pc.instances[slot];
    SceneVsRow    row;
    row.pos[0]      = inst.pos[0];
    row.pos[1]      = inst.pos[1];
    row.pos[2]      = inst.pos[2];
    row.quat       = inst.quat;
    row.scale_sq   = (uint)inst.scale | (f32tof16(pc.squash[slot]) << 16u);
    row.st_wg      = f32tof16(pc.stretch[slot]) | (f32tof16(pc.wiggle[slot]) << 16u);
    row.bloom_face = f32tof16(pc.bloom[slot]) | ((pc.face_id[slot] << 16u));
    row.pair       = slot >> 1u; /* body+face share one jiggle phase */
    pc.vs_rows[dst] = row;
}
