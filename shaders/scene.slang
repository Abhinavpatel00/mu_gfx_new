#include "../src/three_d/scene_shared.h"
#include "common.slang"

#define MAX_MESHES_PER_SET 16
#define MAX_COMMANDS 65536

/* Shader-side kill switch for the skin-deform block. Kept at 0 in the tree;
   flip to 1 locally (or pass -DPETS_DEBUG_NOSKIN=1 to slangc) to force the
   rigid VS path without touching the asset/palette uploads. */
#ifndef PETS_DEBUG_NOSKIN
#define PETS_DEBUG_NOSKIN 0
#endif

[[vk::push_constant]] SceneViewPush pc;

float3 quat_to_basis(uint q, out float3x3 m) {
    float x = (float)((q >> 0) & 0x3FF) / 511.0 - 1.0;
    float y = (float)((q >> 10) & 0x3FF) / 511.0 - 1.0;
    float z = (float)((q >> 20) & 0x3FF) / 511.0 - 1.0;
    float w = (float)((q >> 30) & 0x3) / 1.5 - 1.0;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;
    m[0][0] = 1 - 2 * (yy + zz); m[0][1] = 2 * (xy - wz);     m[0][2] = 2 * (xz + wy);
    m[1][0] = 2 * (xy + wz);     m[1][1] = 1 - 2 * (xx + zz); m[1][2] = 2 * (yz - wx);
    m[2][0] = 2 * (xz - wy);     m[2][1] = 2 * (yz + wx);     m[2][2] = 1 - 2 * (xx + yy);
    return float3(x, y, z);
}

float3 instance_pos(SceneGpuInstance inst) {
    return float3(f16tof32(inst.pos_xy & 0xFFFF), f16tof32((inst.pos_xy >> 16) & 0xFFFF),
                  f16tof32(inst.pos_z__scale & 0xFFFF));
}

float instance_scale(SceneGpuInstance inst) {
    return f16tof32((inst.pos_z__scale >> 16) & 0xFFFF);
}

uint instance_mesh_set(SceneGpuInstance inst) {
    return (inst.flags >> 24) & 0xFF;
}

float3 unpack_center(uint xy, uint zr) {
    return float3(f16tof32(xy & 65535), f16tof32((xy >> 16) & 65535), f16tof32(zr & 65535));
}

float unpack_radius(uint zr) {
    return f16tof32((zr >> 16) & 65535);
}

void clip_rows(out float4 r0, out float4 r1, out float4 r2, out float4 r3) {
    r0 = float4(pc.view.rows[0][0], pc.view.rows[0][1], pc.view.rows[0][2], pc.view.rows[0][3]);
    r1 = float4(pc.view.rows[1][0], pc.view.rows[1][1], pc.view.rows[1][2], pc.view.rows[1][3]);
    r2 = float4(pc.view.rows[2][0], pc.view.rows[2][1], pc.view.rows[2][2], pc.view.rows[2][3]);
    r3 = float4(pc.view.rows[3][0], pc.view.rows[3][1], pc.view.rows[3][2], pc.view.rows[3][3]);
}

bool sphere_outside_frustum(float3 c, float r, float4 r0, float4 r1, float4 r2, float4 r3) {
    float4 p = float4(c, 1);
    float cx = dot(r0, p);
    float cy = dot(r1, p);
    float cz = dot(r2, p);
    float cw = dot(r3, p);
    float nx = length(r0.xyz);
    float ny = length(r1.xyz);
    float nz = length(r2.xyz);
    float nw = length(r3.xyz);
    if (cw + r * nw < 0.0) return true;
    if (abs(cx) > cw + r * nx) return true;
    if (abs(cy) > cw + r * ny) return true;
    if (cz > cw + r * nz) return true;
    if (cz < -r * nz) return true;
    return false;
}

bool hiz_occluded(float3 c, float r, float4 r0, float4 r1, float4 r2, float4 r3, uint hiz_base,
                  uint hiz_levels, uint2 hiz_size) {
    if (hiz_levels == 0) return false;
    float4 center = float4(c, 1);
    float nw = length(r3.xyz);
    float cw = dot(r3, center);
    float w_near = cw - r * nw;
    if (w_near < 1e-3) return false;
    float nz = length(r2.xyz);
    float depth_max = (dot(r2, center) + r * nz) / w_near;
    if (depth_max >= 1.0 || depth_max <= 0.0) return false;
    float nx = length(r0.xyz);
    float ny = length(r1.xyz);
    float inv_near = 1.0 / w_near;
    float inv_far = 1.0 / (cw + r * nw);
    float2 ndc_lo = float2(dot(r0, center) - r * nx, dot(r1, center) - r * ny) * inv_near;
    float2 ndc_hi = float2(dot(r0, center) + r * nx, dot(r1, center) + r * ny) * inv_far;
    if (ndc_lo.x >= 1.0 || ndc_hi.x <= -1.0 || ndc_lo.y >= 1.0 || ndc_hi.y <= -1.0) return false;
    float2 uv_lo = float2(ndc_lo.x * 0.5 + 0.5, 0.5 - ndc_hi.y * 0.5);
    float2 uv_hi = float2(ndc_hi.x * 0.5 + 0.5, 0.5 - ndc_lo.y * 0.5);
    float2 level0 = float2(hiz_size);
    float span = max((uv_hi.x - uv_lo.x) * level0.x, (uv_hi.y - uv_lo.y) * level0.y);
    uint level = (uint)clamp((int)ceil(log2(max(span, 1.0))), 0, (int)hiz_levels - 1);
    float2 size = max(floor(level0 / exp2((float)level)), 1.0);
    int2 lo = (int2)floor(uv_lo * size);
    int2 hi = (int2)floor(uv_hi * size);
    if (hi.x - lo.x > 1 || hi.y - lo.y > 1) return false;
    int2 last = int2(size) - 1;
    lo = clamp(lo, int2(0, 0), last);
    hi = clamp(hi, int2(0, 0), last);
    uint tex = hiz_base + level;
    float m = min(min(textures[tex].Load(int3(lo, 0)).x, textures[tex].Load(int3(hi, 0)).x),
                  min(textures[tex].Load(int3(lo.x, hi.y, 0)).x,
                      textures[tex].Load(int3(hi.x, lo.y, 0)).x));
    return m >= depth_max;
}

uint select_lod(float dist, float scale, float lod_target, uint lod_group) {
    float threshold = dist * lod_target / scale;
    uint lod = 0;
    while (lod + 1 < 4 && pc.view.lod_table[lod_group * 8 + lod + 1].error < threshold) ++lod;
    return lod;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_cull(uint3 tid : SV_DispatchThreadID) {
    uint iid = tid.x / MAX_MESHES_PER_SET;
    uint local_m = tid.x % MAX_MESHES_PER_SET;

    SceneGpuInstance inst = pc.view.instances[iid];
    uint set_id = instance_mesh_set(inst);
    SceneMeshSet set = pc.view.mesh_sets[set_id];
    if (local_m >= set.count) return;

    uint mesh_id = set.first + local_m;
    SceneGpuMesh mesh = pc.meshes[mesh_id];

    float scale = instance_scale(inst);
    float3x3 basis;
    quat_to_basis(inst.quat, basis);

    float3 local_center = unpack_center(mesh.local_center, mesh.local_radius);
    float local_radius = unpack_radius(mesh.local_radius);
    float3 world_center = mul(basis, scale * local_center) + instance_pos(inst);
    float world_radius = local_radius * scale;

    float4 r0, r1, r2, r3;
    clip_rows(r0, r1, r2, r3);

    if (sphere_outside_frustum(world_center, world_radius, r0, r1, r2, r3)) {
        InterlockedAdd(pc.view.counters[0], 1);
        return;
    }

    uint hiz_levels = (pc.misc >> 8) & 0xFF;
    uint hiz_base = pc.misc & 0xFFFF;
    if (hiz_levels != 0) {
        uint2 hiz_size = uint2(pc.view.hiz_size[0], pc.view.hiz_size[1]);
        if (hiz_occluded(world_center, world_radius, r0, r1, r2, r3, hiz_base, hiz_levels, hiz_size)) {
            InterlockedAdd(pc.view.counters[1], 1);
            return;
        }
    }

    float view_z = -(r3.x * world_center.x + r3.y * world_center.y + r3.z * world_center.z + r3.w);
    float dist = max(view_z - world_radius, 0.001);
    uint lod = select_lod(dist, scale, pc.view.lod_target, mesh.lod_group);

    uint dci;
    InterlockedAdd(pc.view.command_count[0], 1, dci);
    if (dci >= MAX_COMMANDS) {
        InterlockedAdd(pc.view.counters[2], 1);
        return;
    }

    SceneLodRow lod_row = pc.view.lod_table[mesh.lod_group * 8 + lod];
    pc.view.draws[dci].index_count = lod_row.index_count;
    pc.view.draws[dci].instance_count = 1;
    pc.view.draws[dci].first_index = lod_row.first_index;
    pc.view.draws[dci].vertex_offset = lod_row.vertex_offset;
    pc.view.draws[dci].first_instance = dci;
    pc.view.draws[dci].draw_id = mesh_id;
    pc.view.visible[dci] = (iid & 0xFFFF) | ((mesh_id & 0xFFFF) << 16);
    InterlockedAdd(pc.view.counters[3], 1);
    InterlockedAdd(pc.view.counters[4 + lod], 1);
}

struct Varying {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float3 color : COLOR0;
    float2 uv : TEXCOORD1;
    uint mat : TEXCOORD2;
};

[shader("vertex")]
Varying vs_main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    // SV_InstanceID includes firstInstance, which is the compacted command index.
    uint entry = pc.view.visible[instance];
    uint iid = entry & 0xFFFF;
    uint mesh_id = entry >> 16;

    SceneGpuInstance inst = pc.view.instances[iid];
    SceneGpuMesh mesh = pc.meshes[mesh_id];

    float scale = instance_scale(inst);
    float3x3 basis;
    quat_to_basis(inst.quat, basis);

    float3 pos = instance_pos(inst);

    ScenePackedVertex packed = mesh.vertex_stream[vertex];

    float3 local = float3(f16tof32(packed.position_xy & 65535),
                          f16tof32(packed.position_xy >> 16),
                          f16tof32(packed.position_z & 65535));

    float2 oct = max(float2(int(packed.normal_oct << 16) >> 16, int(packed.normal_oct) >> 16) / 32767.0, -1.0);
    float3 normal = float3(oct, 1 - abs(oct.x) - abs(oct.y));
    float fold = max(-normal.z, 0.0);
    normal.xy += float2(normal.x >= 0 ? -fold : fold, normal.y >= 0 ? -fold : fold);

    SceneSkinRecord sr = pc.skin_table[mesh_id];
    if (PETS_DEBUG_NOSKIN == 0 && uint64_t(sr.skin_stream) != 0ull) {
        uint phase = iid & 7u;
        uint anim = (iid >> 3) & 1u;
        uint sw = vertex * 8u;
        uint4 j = uint4(sr.skin_stream[sw + 0u], sr.skin_stream[sw + 1u], sr.skin_stream[sw + 2u],
                        sr.skin_stream[sw + 3u]);
        float4 w = float4(sr.skin_stream[sw + 4u], sr.skin_stream[sw + 5u], sr.skin_stream[sw + 6u],
                          sr.skin_stream[sw + 7u]) * (1.0 / 65535.0);
        j = min(j, max(sr.joint_count, 1u) - 1u);
        uint pbase = (anim * 8u + phase) * sr.joint_count;
        /* CPU palettes are row-major (mesh_world * joint_world * inverse_bind),
           Slang stores buffer float4x4 column-major, so the row-vector form
           mul(v, m) applies the intended matrix; mul(m, v) would transpose it. */
        float4 lp = float4(local, 1.0);
        float4 sp = mul(lp, sr.palette[pbase + j.x].m) * w.x + mul(lp, sr.palette[pbase + j.y].m) * w.y +
                    mul(lp, sr.palette[pbase + j.z].m) * w.z + mul(lp, sr.palette[pbase + j.w].m) * w.w;
        local = sp.xyz;
        float3 sn = mul(normal, (float3x3)sr.palette[pbase + j.x].m) * w.x +
                    mul(normal, (float3x3)sr.palette[pbase + j.y].m) * w.y +
                    mul(normal, (float3x3)sr.palette[pbase + j.z].m) * w.z +
                    mul(normal, (float3x3)sr.palette[pbase + j.w].m) * w.w;
        normal = sn;
    }

    float3 world = mul(basis, scale * local) + pos;
    normal = mul(basis, normal);

    float4 r0, r1, r2, r3;
    clip_rows(r0, r1, r2, r3);

    Varying output;
    output.position = float4(dot(r0, float4(world, 1)),
                             dot(r1, float4(world, 1)),
                             dot(r2, float4(world, 1)),
                             dot(r3, float4(world, 1)));
    output.normal = normalize(normal);
    output.color = float3(1, 1, 1);
    output.uv = float2(f16tof32(packed.uv & 65535), f16tof32(packed.uv >> 16));
    output.mat = (mesh.pack >> 20) & 0xFFF;
    return output;
}

[shader("fragment")]
float4 fs_main(Varying input) : SV_Target0 {
    SceneGpuMaterial mat = pc.view.materials[input.mat];
    float4 base = float4(mat.base_color & 255, (mat.base_color >> 8) & 255,
                         (mat.base_color >> 16) & 255, mat.base_color >> 24) / 255.0;
    uint albedo = mat.texture & 0xFFFF;
    float3 albedo_rgb = base.rgb;
    if (albedo != 0xFFFF)
        albedo_rgb *= textures[albedo].Sample(samplers[pc.view.sampler], input.uv).rgb;
    float3 sun_dir = float3(pc.view.sun[0], pc.view.sun[1], pc.view.sun[2]);
    float ambient = pc.view.sun[3];
    float light = ambient + (1 - ambient) * max(dot(normalize(input.normal), normalize(sun_dir)), 0.0);
    return float4(albedo_rgb * light, 1);
}
