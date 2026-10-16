/* slime_grid.slang — pass 2 of the slime compute chain.
   Builds the uniform collision grid around the camera. The crowd is wrapped to
   stay near the player, so every active slime falls inside this grid and takes
   part in collision; the grid bounds are the only gate. One atomic per slime; a
   full cell drops the overflow rather than growing, which is fine at this
   density. */
#include "common.slang"
#include "../src/three_d/scene_shared.h"

[[vk::push_constant]] SceneSlimePush pc;

[numthreads(64, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= pc.counts[0])
        return;

    SlimeState s = pc.state_in[i];

    int cx = int((s.pos[0] - pc.min_x) * pc.inv_cell);
    int cz = int((s.pos[2] - pc.min_z) * pc.inv_cell);
    if (cx < 0 || cz < 0 || cx >= int(pc.counts[1]) || cz >= int(pc.counts[2]))
        return;

    uint c    = uint(cz) * pc.counts[1] + uint(cx);
    uint slot;
    InterlockedAdd(pc.grid_count[c], 1u, slot);
    if (slot < pc.counts[3])
        pc.grid_items[c * pc.counts[3] + slot] = i;
}
