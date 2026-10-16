/* slime_resolve.slang — pass 3 of the slime compute chain.
   Reads the integrated snapshot (state_in) read-only, pushes near grounded
   slimes apart using the grid, and commits the frame: the collision-corrected
   state to state_out (next frame's input) plus the body/face instance rows and
   the squash/face tables the draw reads. Only slimes inside the collide radius
   and in the grid touch a neighbour; everything else is a straight copy, so the
   cost scales with what the player can actually see. */
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

    float dxc = s.pos[0] - pc.view.x;
    float dzc = s.pos[2] - pc.view.z;
    bool  near = (dxc * dxc + dzc * dzc) <= pc.view.w * pc.view.w;

    if (near && (s.flags & SLIME_FLAG_AIR) == 0u)
    {
        int cx = int((s.pos[0] - pc.min_x) * pc.inv_cell);
        int cz = int((s.pos[2] - pc.min_z) * pc.inv_cell);
        if (cx >= 0 && cz >= 0 && cx < int(pc.counts[1]) && cz < int(pc.counts[2]))
        {
            uint  nx = pc.counts[1], nz = pc.counts[2], cap = pc.counts[3];
            float px = 0.0f, pz = 0.0f;
            uint  hits = 0u;
            for (int oz = -1; oz <= 1; ++oz)
            {
                int z = cz + oz;
                if (z < 0 || z >= int(nz)) continue;
                for (int ox = -1; ox <= 1; ++ox)
                {
                    int x = cx + ox;
                    if (x < 0 || x >= int(nx)) continue;
                    uint c = uint(z) * nx + uint(x);
                    uint n = min(pc.grid_count[c], cap);
                    for (uint k = 0u; k < n; ++k)
                    {
                        uint j = pc.grid_items[c * cap + k];
                        /* state_in is never written by this pass, so reading a
                           neighbour is safe; only this thread's own row goes out. */
                        SlimeState o = pc.state_in[j];
                        if ((o.flags & SLIME_FLAG_AIR) != 0u) continue;
                        float ddx = s.pos[0] - o.pos[0];
                        float ddz = s.pos[2] - o.pos[2];
                        float d2  = ddx * ddx + ddz * ddz;
                        float rr  = 0.55f * (s.scale + o.scale);
                        if (d2 > rr * rr || d2 < 1e-6f) continue;
                        float d    = sqrt(d2);
                        float push = (rr - d) * 0.5f;
                        px += (ddx / d) * push;
                        pz += (ddz / d) * push;
                        hits++;
                    }
                }
            }
            if (hits > 0u)
            {
                s.pos[0] += px * 0.9f;
                s.pos[2] += pz * 0.9f;
                s.wobble = min(s.wobble + 0.02f * float(hits), 0.22f);
            }
        }
    }

    /* Commit: squash channel (wobble rides it), face tile, and both instance
       rows of the pair. The body centre rides at bottom + 0.8*squash*scale,
       matching the VS's local extent. */
    float sq = clamp(s.squash - s.wobble, 0.5f, 1.6f);
    float st = clamp(s.stretch, 0.7f, 1.6f);
    float cy = s.pos[1] + 0.8f * sq * s.scale;

    float hy   = sin(s.yaw * 0.5f);
    int   yi   = clamp(int(round(hy * 511.0f)), -511, 511);
    uint  quat = (uint(yi) & 0x3FFu) << 10; /* 10-10-10-2: x=z=0, y in bits 10..19 */

    SceneInstance inst;
    inst.pos[0]   = s.pos[0];
    inst.pos[1]   = cy;
    inst.pos[2]   = s.pos[2];
    inst.quat     = quat;
    inst.scale    = uint16_t(f32tof16(s.scale));
    inst.lod_bias = int16_t(0);

    uint bslot = i * 2u;
    pc.instances[bslot]      = inst;
    pc.instances[bslot + 1u] = inst;
    pc.squash[bslot]         = sq;
    pc.squash[bslot + 1u]    = sq;
    pc.stretch[bslot]        = st;
    pc.stretch[bslot + 1u]   = st;
    pc.face[bslot + 1u]      = s.face_id;

    pc.state_out[i] = s;
}
