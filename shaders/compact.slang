/* compact.slang — T3: turn the unordered survivor list into a dense command
   array and a dense visible array (counting sort over (mesh,lod) groups).

   cs_count   histogram survivors by group
   cs_prefix  exclusive scan -> vis base + compacted command emit + draw_count
   cs_compact scatter survivors into their group's vis range

   Each entry is its own .spv; create_compute_pipeline binds entry "main". */
#include "common.slang"
#include "../src/three_d/scene_shared.h"

[[vk::push_constant]] SceneCompactPush pc;

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

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_prefix(uint3 tid : SV_DispatchThreadID)
{
    uint G   = pc.counts[1];
    uint run = 0u;
    uint cmd = 0u;
    for (uint g = 0u; g < G; ++g)
    {
        uint c        = pc.group_count[g];
        pc.vis_base[g] = run;
        pc.cursor[g]   = 0u;
        if (c != 0u)
        {
            SceneGpuDraw d = pc.group_static[g];
            d.instance_count = c;
            d.first_instance = run;
            pc.draws[cmd]    = d;
            ++cmd;
        }
        run += c;
    }
    pc.draw_count[0] = cmd;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_compact(uint3 tid : SV_DispatchThreadID)
{
    uint n = min(pc.survivor_count[0], pc.counts[2]);
    if (tid.x >= n)
        return;
    uint key   = pc.survivors[tid.x];
    uint g     = group_of(key);
    uint local;
    InterlockedAdd(pc.cursor[g], 1u, local);
    pc.vis[pc.vis_base[g] + local] = key;
}
