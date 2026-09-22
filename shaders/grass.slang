#include "common.slang"

struct GrassInst {
    float x, y, z;
    float height;
    float width;
    uint  seed;
};

struct DrawIndexed {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

struct GrassCullPush {
    GrassInst*   instances;
    uint*        visible;
    DrawIndexed* args;
    float4       clip_rows[4];
    uint         instance_count;
    uint         visible_base;
    uint         frame_region;
    uint         pad0;
    float        fade_start;
    float        fade_end;
    float        field_radius;
    uint         pad1;
};

struct GrassDrawPush {
    uint visible_base;
    uint mode; // 0 = blade (instance), 1 = world-space (ground/daisy)
    uint pad0;
    uint pad1;
};

[[vk::push_constant]] GrassCullPush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.instance_count)
        return;

    GrassInst inst = pc.instances[i];
    float3 center = float3(inst.x, inst.y + inst.height * 0.5, inst.z);
    float4 c = float4(center, 1);

    for (uint axis = 0; axis < 3; ++axis) {
        float4 low  = axis == 2 ? pc.clip_rows[2] : pc.clip_rows[3] + pc.clip_rows[axis];
        float4 high = pc.clip_rows[3] - pc.clip_rows[axis];
        if (dot(low, c) < 0 || dot(high, c) < 0)
            return;
    }

    float dist = distance(center, global_data.camera_pos.xyz);
    if (dist > pc.fade_end)
        return;

    uint slot;
    InterlockedAdd(pc.args[pc.frame_region].instanceCount, 1u, slot);
    pc.visible[pc.visible_base + slot] = i;
}

struct VSOutput {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float  dist : TEXCOORD2;
};

[shader("vertex")]
VSOutput vs_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    VSOutput o;

    static const float2 radial[7] = {
        float2(0, 0),
        float2(1, 0), float2(0.5, 0.8660254), float2(-0.5, 0.8660254),
        float2(-1, 0), float2(-0.5, -0.8660254), float2(0.5, -0.8660254)
    };
    static const float2 profile[5] = {
        float2(1.0, 0.0), float2(0.9, 0.4), float2(0.7, 0.75),
        float2(0.4, 0.95), float2(0.0, 1.0)
    };

    uint seg  = vertex_id / 7;
    uint ring = vertex_id % 7;

    // World-space path (ground / daisy): vertex already in world coords.
    // Blade path: rebuild the same local position from radial/profile tables.
    float3 local_pos;
    float  v;
    if (seg >= 5) {
        // Fallback: treat as world-space (ground uses raw positions via SV_VertexID only for blades)
        local_pos = float3(0, 0, 0);
        v = 0;
    } else {
        float2 local = radial[ring] * profile[seg].x * 0.5;
        local_pos = float3(local.x, profile[seg].y, local.y);
        v = profile[seg].y;
    }

    uint blade_idx = pc.visible[pc.visible_base + instance_id];
    GrassInst inst = pc.instances[blade_idx];

    float yaw  = (inst.seed & 0xFFFFu) * (6.2831853 / 65536.0);
    float cs = cos(yaw), sn = sin(yaw);
    float3 rot = float3(
        local_pos.x * cs - local_pos.z * sn,
        local_pos.y,
        local_pos.x * sn + local_pos.z * cs
    );

    float bend_x = sin(global_data.time * 1.1 + inst.x * 0.4 + inst.z * 0.3) * 0.22 * v * v;
    float bend_z = cos(global_data.time * 1.3 + inst.z * 0.5 + inst.seed * 0.001) * 0.18 * v * v;

    float3 world = float3(
        inst.x + rot.x * inst.width * 2.0 + bend_x * inst.height,
        inst.y + rot.y * inst.height,
        inst.z + rot.z * inst.width * 2.0 + bend_z * inst.height
    );

    float4 clip = float4(
        dot(pc.clip_rows[0], float4(world, 1)),
        dot(pc.clip_rows[1], float4(world, 1)),
        dot(pc.clip_rows[2], float4(world, 1)),
        dot(pc.clip_rows[3], float4(world, 1))
    );

    o.position = clip;
    o.normal = float3(0, 1, 0);
    o.uv = float2(0.5, v);
    o.dist = distance(world, global_data.camera_pos.xyz);
    return o;
}

[shader("fragment")]
float4 fs_main(float4 position : SV_Position, float3 normal : TEXCOORD0, float2 uv : TEXCOORD1, float dist : TEXCOORD2) : SV_Target0 {
    float t = saturate(uv.y);
    float3 dark  = float3(0.04, 0.20, 0.06);
    float3 light = float3(0.32, 0.72, 0.25);
    float3 green = lerp(dark, light, t);

    float fade = saturate((pc.fade_end - dist) / max(pc.fade_end - pc.fade_start, 0.001));
    green = lerp(float3(0.35, 0.52, 0.48), green, fade);

    float wrap = 0.55;
    float ndl = saturate((dot(normalize(normal), float3(0.3, 0.9, 0.4)) + wrap) / (1.0 + wrap));
    return float4(green * (0.5 + 0.5 * ndl), 1);
}
