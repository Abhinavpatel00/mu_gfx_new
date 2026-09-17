#include "../src/scene3d_shared.h"
[[vk::push_constant]] ScenePush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    uint candidate_id = tid.x;
    if (candidate_id >= pc.instance_count) return;
    SceneCandidate candidate = pc.candidates[candidate_id];
    float4 sphere = pc.bounds[candidate.instance];
    float4 center = float4(sphere.xyz, 1);
    for (uint axis = 0; axis < 3; ++axis) {
        float4 low = axis == 2 ? pc.clip_rows[2] : pc.clip_rows[3] + pc.clip_rows[axis];
        float4 high = pc.clip_rows[3] - pc.clip_rows[axis];
        if (dot(low, center) < -sphere.w * length(low.xyz) ||
            dot(high, center) < -sphere.w * length(high.xyz)) return;
    }
    uint offset;
    InterlockedAdd(pc.commands[candidate.batch].instance_count, 1, offset);
    pc.visible[pc.commands[candidate.batch].first_instance + offset] = candidate.instance;
}

struct Varying {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float3 color : COLOR0;
};
[shader("vertex")]
Varying vs_main(uint vertex : SV_VertexID, uint instance : SV_InstanceID,
                uint base_instance : SV_StartInstanceLocation) {
    SceneInstance item = pc.instances[pc.visible[base_instance + instance]];
    ScenePackedVertex packed = item.vertices[vertex];
    float4 local = float4(f16tof32(packed.position_xy & 65535), f16tof32(packed.position_xy >> 16),
                          f16tof32(packed.position_z & 65535), 1);
    float4 world = float4(dot(item.rows[0], local), dot(item.rows[1], local), dot(item.rows[2], local), 1);
    float2 oct = max(float2(int(packed.normal_oct << 16) >> 16, int(packed.normal_oct) >> 16) / 32767.0, -1.0);
    float3 normal = float3(oct, 1 - abs(oct.x) - abs(oct.y));
    float fold = max(-normal.z, 0.0);
    normal.xy += float2(normal.x >= 0 ? -fold : fold, normal.y >= 0 ? -fold : fold);
    // Cofactor columns give inverse-transpose up to positive determinant. This
    // supports nonuniform scale and shear without a second per-instance matrix.
    float3 a = item.rows[0].xyz, b = item.rows[1].xyz, c = item.rows[2].xyz;
    float3 transformed = float3(dot(cross(b, c), normal), dot(cross(c, a), normal), dot(cross(a, b), normal));
    float4 tint = float4(item.tint & 255, (item.tint >> 8) & 255,
                        (item.tint >> 16) & 255, item.tint >> 24) / 255.0;
    Varying output;
    output.position = float4(dot(pc.clip_rows[0], world), dot(pc.clip_rows[1], world),
                             dot(pc.clip_rows[2], world), dot(pc.clip_rows[3], world));
    output.normal = normalize(transformed);
    output.color = pc.materials[item.material].xyz * tint.xyz;
    return output;
}
[shader("fragment")]
float4 fs_main(Varying input) : SV_Target0 {
    float light = pc.sun.w + (1 - pc.sun.w) * max(dot(normalize(input.normal), normalize(pc.sun.xyz)), 0.0);
    return float4(input.color * light, 1);
}
