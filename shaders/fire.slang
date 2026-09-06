// Fire shader in Slang (HLSL-like)
// Based on the golfed GLSL shader

struct FirePush
{
    uint width;
    uint height;
    float time;
    float pad;
};

[[vk::push_constant]]
FirePush push;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vs_main(uint id : SV_VertexID)
{
    // Fullscreen triangle
    static const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    static const float2 uvs[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0)
    };

    VSOutput outv;
    outv.position = float4(positions[id], 0.0, 1.0);
    outv.uv = uvs[id];
    return outv;
}

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target0
{
    float2 resolution = float2(push.width, push.height);
    float2 C = input.position.xy;               // pixel coordinate (top-left origin)
    C.y = resolution.y - C.y;                   // flip Y so flames rise upward
    float time = push.time;

    // Raymarching state
    float z = 0.0;      // distance along ray
    float d = 0.0;      // step size / density weight
    float4 O = float4(0.0, 0.0, 0.0, 0.0);      // accumulated color

    // Camera ray direction (perspective)
    float3 ray_dir = normalize(float3(2.0 * C.x - resolution.x,
                                      2.0 * C.y - resolution.y,
                                      resolution.y));

    // Main raymarching loop (70 steps)
    for (int i = 0; i < 70; ++i)
    {
        float3 p = z * ray_dir;                 // current position along ray
        p.z += 5.0 + cos(time);                 // push volume away and add slight motion

        // Rotation in xz‑plane (swirl)
        float angle = time + p.y * 0.25;        // base angle depends on time and height
        float4 rot_vec = cos(angle + float4(0.0, 8.0, 5.0, 0.0));
        p.xz = float2(
            p.x * rot_vec.x + p.z * rot_vec.z,
            p.x * rot_vec.y + p.z * rot_vec.w
        );

        // Domain warping – 6 iterations of fractal displacement
        d = 2.0;
        for (int j = 0; j < 6; ++j)
        {
            d /= 0.8;                           // increase frequency, decrease amplitude
            float3 shifted = float3(p.y - 8.0 * time, p.z, p.x); // swizzle and time offset
            p += cos(shifted * d + time) / d;
        }

        // Proximity to a cone‑like surface (used as density)
        d = 0.01 + abs(length(p.xz) + p.y * 0.3 - 1.0) / 9.0;
        z += d;                                 // advance ray

        // Accumulate color weighted by inverse distance
        O += (sin(p.y * 0.5 - float4(0.0, 1.0, 2.0, 0.0)) + 1.1) / d;
    }

    // Tone mapping
    float3 fire = tanh(O.rgb / 10.0);

    return float4(fire, 1.0);
}




