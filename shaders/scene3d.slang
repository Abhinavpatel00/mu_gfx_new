#include "../src/scene3d_shared.h"
[[vk::push_constant]] ScenePush pc;

[shader("compute")]
[numthreads(32, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    uint id = tid.x;
    if (id >= pc.instance_count) return;
    float4 center = float4(pc.instances[id].center_scale.xyz, 1);
    float radius = pc.instances[id].center_scale.w * 1.732051;
    bool visible = true;
    for (uint axis = 0; axis < 3; ++axis) {
        float4 low = axis == 2 ? pc.clip_rows[2] : pc.clip_rows[3] + pc.clip_rows[axis];
        float4 high = pc.clip_rows[3] - pc.clip_rows[axis];
        visible = visible && dot(low, center) >= -radius * length(low.xyz);
        visible = visible && dot(high, center) >= -radius * length(high.xyz);
    }
    SceneDraw draw;
    draw.index_count = 36;
    draw.instance_count = visible ? 1 : 0;
    draw.first_index = 0;
    draw.vertex_offset = 0;
    draw.first_instance = id;
    pc.commands[id] = draw;
}

struct Varying {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 color : COLOR0;
};
[shader("vertex")]
Varying vs_main(uint vertex : SV_VertexID, uint instance : SV_InstanceID,
                uint base_instance : SV_StartInstanceLocation) {
    SceneInstance item = pc.instances[base_instance + instance];
    float4 world = float4(pc.vertices[vertex].xyz * item.center_scale.w + item.center_scale.xyz, 1);
    Varying output;
    output.position = float4(dot(pc.clip_rows[0], world), dot(pc.clip_rows[1], world),
                             dot(pc.clip_rows[2], world), dot(pc.clip_rows[3], world));
    output.world = world.xyz;
    output.color = item.color.xyz;
    return output;
}
[shader("fragment")]
float4 fs_main(Varying input) : SV_Target0 {
    float3 normal = normalize(cross(ddx(input.world), ddy(input.world)));
    float light = 0.25 + 0.75 * abs(dot(normal, normalize(float3(1, 2, 3))));
    return float4(input.color * light, 1);
}
