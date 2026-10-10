/* cull.slang — T2: the GPU frustum cull.
   No mesh shaders: the kernel's only job is to decide which (instance, mesh)
   pairs survive, and to hand the compaction stage a packed key per survivor.
   Its own push constant, separate from the draw's, so the six normalized
   frustum planes fit without pushing the draw's block past the range the
   pipeline layout declares. */
#include "common.slang"
#include "../src/three_d/scene_shared.h"

[[vk::push_constant]] SceneCullPush pc;

/* ---- quaternion (10-10-10-2) ---- */
static float3 quat_rotate(SceneU32 q, float3 v)
{
    int3   i  = int3((int)(q << 22) >> 22, (int)(q << 12) >> 22, (int)(q << 2) >> 22);
    float3 qv = float3(i) * (1.0f / 511.0f);
    float  qw = sqrt(max(0.0f, 1.0f - dot(qv, qv)));
    /* The sign bit is data-dependent, so select rather than branch. */
    qw = (q & (1u << 30)) != 0u ? -qw : qw;
    float3 t = 2.0f * cross(qv, v);
    return v + qw * t + cross(qv, t);
}

static float2 unpack_half2(SceneU32 xy) { return float2(f16tof32(xy & 0xFFFFu), f16tof32(xy >> 16)); }

/* ---- exact world sphere under uniform scale ---- */
static void world_sphere(SceneInstance inst, SceneCullMesh cm, out float3 c, out float r)
{
    float3 local = float3(unpack_half2(cm.local_sphere_xy), f16tof32(cm.local_sphere_zr & 0xFFFFu));
    float  s     = f16tof32(inst.scale);
    float3 ipos  = float3(inst.pos[0], inst.pos[1], inst.pos[2]);
    c            = ipos + quat_rotate(inst.quat, local * s);
    r            = s * f16tof32(cm.local_sphere_zr >> 16);
}

/* Reject a sphere against a plane the CPU has already normalized: positive
   distance means the centre is inside. There is no length() here. The norm is
   a property of the plane, the plane is uniform across the dispatch, and the
   old per-candidate version paid six square roots and six divides to divide by
   a constant. */
static inline bool outside_plane(float4 plane, float3 center, float radius)
{
    return dot(plane.xyz, center) + plane.w < -radius;
}

static inline bool frustum_visible(float3 c, float r)
{
    [unroll]
    for (uint i = 0u; i < 6u; ++i)
        if (outside_plane(pc.frustum[i], c, r))
            return false;
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_cull(uint3 gid : SV_GroupID, uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.counts[1])
        return;

    SceneCullRow  cr   = pc.cull_rows[tid.x];
    SceneInstance inst = pc.instances[cr.slot];
    SceneCullMesh cm   = pc.cull_meshes[cr.mesh];

    float3 c;
    float  r;
    world_sphere(inst, cm, c, r);

    if (!frustum_visible(c, r))
    {
        if (pc.counts[5] != 0u) /* DEBUG: identity of the rejected candidate */
            printf("[CULLED] cand=%u slot=%u mesh=%u center=(%.4f, %.4f, %.4f) r=%.4f\n",
                   tid.x, cr.slot, cr.mesh, c.x, c.y, c.z, r);
        /* The rejected count is not accumulated here. It is
           candidates - drawn - dropped, and the compaction stage already knows
           all three, so cs_scan_blocks writes it instead of every rejected
           candidate hammering one 36-byte counter line. */
        return;
    }

    if (pc.counts[5] != 0u) /* DEBUG: sphere that passed, plus the true origin */
        printf("[CULL] cand=%u slot=%u mesh=%u center=(%.4f, %.4f, %.4f) r=%.4f "
               "origin=(%.4f, %.4f, %.4f)\n",
               tid.x, cr.slot, cr.mesh, c.x, c.y, c.z, r,
               float(inst.pos[0]), float(inst.pos[1]), float(inst.pos[2]));

    /* LOD is arithmetic on data the visibility test already produced. The
       ladder walk is a no-op until the asset carries rungs (lod_count == 1). */
    uint lod = 0u;
    if (cm.lod_count > 1u)
    {
        float dist   = max(length(c - pc.viewparams.xyz) - r, 0.0f);
        float metric = dist * pc.viewparams.w / max(f16tof32(inst.scale), 1e-4f);
        while (lod + 1u < cm.lod_count && pc.lod_rows[cm.lod_first + lod + 1u].error < metric)
            ++lod;
    }
    lod = (uint)clamp((int)lod + (int)inst.lod_bias, 0, (int)cm.lod_count - 1);

    uint key = (lod << 30) | ((uint)cr.mesh << 16) | cr.slot;

    uint idx;
    InterlockedAdd(pc.survivor_count[0], 1u, idx);
    if (idx < pc.counts[2])
        pc.survivors[idx] = key;
    else
        InterlockedAdd(pc.counters[SCENE_COUNTER_DROPPED], 1u);
}