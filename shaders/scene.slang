/* scene.slang — the GPU-driven 3D scene: cull (T2), vertex and fragment.
   No mesh shaders: indexed indirect draw + vertex pull.
   No data-dependent branch on any row field: the only exits are the dispatch
   bounds guard and the visibility tests. */
#include "common.slang"
#include "../src/three_d/scene_shared.h"

[[vk::push_constant]] ScenePush pc;

/* ---- quaternion (10-10-10-2) ---- */
static float3 quat_rotate(SceneU32 q, float3 v)
{
    int3   i  = int3((int)(q << 22) >> 22, (int)(q << 12) >> 22, (int)(q << 2) >> 22);
    float3 qv = float3(i) * (1.0f / 511.0f);
    float  qw = sqrt(max(0.0f, 1.0f - dot(qv, qv)));
    if ((q >> 30) & 1u)
        qw = -qw;
    float3 t = 2.0f * cross(qv, v);
    return v + qw * t + cross(qv, t);
}

/* ---- decode helpers ---- */
static float2 unpack_half2(SceneU32 xy) { return float2(f16tof32(xy & 0xFFFFu), f16tof32(xy >> 16)); }

static float3 unpack_oct(SceneU32 o)
{
    float2 e = max(float2((int)(o << 16) >> 16, (int)o >> 16) * (1.0f / 32767.0f), -1.0f);
    float3 n = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
    float  t = max(-n.z, 0.0f);
    n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);
    return n;
}

/* ---- exact world sphere under uniform scale ---- */
static void world_sphere(SceneInstance inst, SceneCullMesh cm, out float3 c, out float r)
{
    float3 local = float3(unpack_half2(cm.local_sphere_xy), f16tof32(cm.local_sphere_zr & 0xFFFFu));
    float  s     = f16tof32(inst.scale);
    float3 ipos  = float3(inst.pos[0], inst.pos[1], inst.pos[2]);
    c            = ipos + quat_rotate(inst.quat, local * s);
    r            = s * f16tof32(cm.local_sphere_zr >> 16);
}

/* ---- 6 frustum planes derived from the clip rows ---- */
static bool frustum_visible(float3 c, float r)
{
    for (uint p = 0; p < 6u; ++p)
    {
        uint  axis = p >> 1;
        float sign = (p & 1u) ? -1.0f : 1.0f;
        float4 row = pc.clip_rows[axis];
        float4 plane = float4(pc.clip_rows[3].xyz + sign * row.xyz, pc.clip_rows[3].w + sign * row.w);
        if (dot(plane.xyz, c) + plane.w < -r * length(plane.xyz))
            return false;
    }
    return true;
}

/* ---- T2: cull. One thread per candidate. ---- */
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_cull(uint3 tid : SV_DispatchThreadID)
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
        InterlockedAdd(pc.counters[SCENE_COUNTER_FRUSTUM], 1u);
        return;
    }

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
    int biased = clamp((int)lod + (int)inst.lod_bias, 0, (int)cm.lod_count - 1);
    lod        = (uint)biased;

    uint key = (lod << 30) | ((uint)cr.mesh << 16) | cr.slot;

    uint idx;
    InterlockedAdd(pc.survivor_count[0], 1u, idx);
    if (idx < pc.counts[2])
    {
        pc.survivors[idx] = key;
        InterlockedAdd(pc.counters[SCENE_COUNTER_DRAWN], 1u);
        InterlockedAdd(pc.counters[SCENE_COUNTER_LOD0 + min(lod, 3u)], 1u);
    }
    else
    {
        InterlockedAdd(pc.counters[SCENE_COUNTER_DROPPED], 1u);
    }
}

/* ---- vertex: indexed indirect draw, identity resolved from vis[] ---- */
struct SceneVOut {
    float4               position : SV_Position;
    float3               normal   : TEXCOORD0;
    float2               uv       : TEXCOORD1;
    nointerpolation uint material : TEXCOORD2;
};

[shader("vertex")]
SceneVOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    /* iid is gl_InstanceIndex: absolute, already includes the group's
       firstInstance == vis base. */
    SceneU32 entry = pc.vis[iid];
    SceneU32 slot  = entry & 0xFFFFu;
    SceneU32 mesh  = (entry >> 16) & 0x3FFFu;

    SceneInstance     inst = pc.instances[slot];
    SceneShadeMesh    sm   = pc.shade_meshes[mesh];
    ScenePackedVertex v    = sm.vertex_stream[vid];

    float3 local = float3(unpack_half2(v.position_xy), f16tof32(v.position_z & 0xFFFFu));
    float  s     = f16tof32(inst.scale);
    float3 ipos  = float3(inst.pos[0], inst.pos[1], inst.pos[2]);
    float3 world = ipos + quat_rotate(inst.quat, local * s);
    float3 nrm   = quat_rotate(inst.quat, unpack_oct(v.normal_oct));

    SceneVOut o;
    o.position = float4(dot(pc.clip_rows[0], float4(world, 1.0f)), dot(pc.clip_rows[1], float4(world, 1.0f)),
                        dot(pc.clip_rows[2], float4(world, 1.0f)), dot(pc.clip_rows[3], float4(world, 1.0f)));
    o.normal   = nrm;
    o.uv       = unpack_half2(v.uv);
    o.material = sm.material;
    return o;
}

[shader("fragment")]
float4 fs_main(SceneVOut i) : SV_Target0
{
    SceneGpuMaterial m   = pc.materials[i.material];
    float3           alb = float3(m.base_color & 255u, (m.base_color >> 8) & 255u, (m.base_color >> 16) & 255u) *
                           (1.0f / 255.0f);
    float light = pc.sun.w + (1.0f - pc.sun.w) * max(dot(normalize(i.normal), normalize(pc.sun.xyz)), 0.0f);
    return float4(alb * light, 1.0f);
}
