#include "common.slang"

struct UiVertex {
    float2 position;
    float2 uv;
    uint color;
};

struct UiPush {
    UiVertex* vertices;
    float2 scale;
    uint texture_id;
    uint sampler_id;
};
[[vk::push_constant]] UiPush pc;

struct UiVarying {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
UiVarying vs_main(uint id : SV_VertexID) {
    UiVertex vertex = pc.vertices[id];
    UiVarying output;
    // The renderer uses a negative-height viewport.
    output.position = float4(vertex.position * pc.scale + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = vertex.uv;
    output.color = float4(vertex.color & 255, (vertex.color >> 8) & 255,
                          (vertex.color >> 16) & 255, vertex.color >> 24) / 255.0;
    return output;
}

[shader("fragment")]
float4 fs_main(UiVarying input) : SV_Target0 {
    return input.color * textures[pc.texture_id].Sample(samplers[pc.sampler_id], input.uv);
}
