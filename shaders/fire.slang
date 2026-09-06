

// ============================================================
// Optimized procedural fire - Slang / Vulkan
// ============================================================

struct FirePush
{
    uint  width;
    uint  height;
    float time;
    float pad;
};

[[vk::push_constant]]
FirePush push;


struct VSOutput
{
    float4 position : SV_Position;
};


[shader("vertex")]
VSOutput vs_main(uint id : SV_VertexID)
{
    static const float2 v[3] =
    {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    VSOutput o;
    o.position = float4(v[id], 0.0, 1.0);
    return o;
}


[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target0
{
    float2 R = float2(push.width, push.height);
    float  T = push.time;


    float2 C = input.position.xy;
    C.y = R.y - C.y;

    float3 rd = normalize(float3(
        2.0 * C.x - R.x,
        2.0 * C.y - R.y,
        -R.y
    ));


    float z = 0.0;
    float3 O = 0.0;

    // Constants for cos(a + 5), cos(a + 8)
    // cos(a+b) = cos(a)cos(b) - sin(a)sin(b)
    const float C5 =  0.2836621855;
    const float S5 = -0.9589242747;
    const float C8 = -0.1455000338;
    const float S8 =  0.9893582468;

    // Color phase constants.
    const float C1 =  0.5403023059;
    const float S1 =  0.8414709848;
    const float C2 = -0.4161468365;
    const float S2 =  0.9092974268;

    [loop]
    for (int i = 0; i < 48; ++i)
    {

        float3 p = z * rd;

        p.z += 5.0 + cos(T);
        float a = T + p.y * 0.25;

        float sa, ca;
        sincos(a, sa, ca);

        float ca5 = ca * C5 - sa * S5;
        float ca8 = ca * C8 - sa * S8;

        float x = p.x;
        float zz = p.z;

        p.x = x * ca  + zz * ca8;
        p.z = x * ca5 + zz * ca;



        float freq = 2.5;
        float amp  = 0.4;

        [loop]
        for (int j = 0; j < 6; ++j)
        {
            // p.yzx - float3(T,0,0)*8
            float3 q = float3(
                p.y - 8.0 * T,
                p.z,
                p.x
            );

            p += cos(q * freq + T) * amp;

            freq *= 1.25;
            amp  *= 0.8;
        }



        float d = 0.01 + abs  (length(p.xz)
                + p.y * 0.3
                - 1.0
            ) * (1.0 / 9.0);



        float y = p.y * 0.5;

        float sy, cy;
        sincos(y, sy, cy);

        float3 emission =
            float3(
                sy,
                sy * C1 - cy * S1,
                sy * C2 - cy * S2
            ) + 1.1;

        O += emission / d;

        z += d;

        // near/mid-depth region. Stop once we are well past it.
        if (z > 12.0)
            break;
    }



    O = tanh(O * 0.001);

    return float4(O, 1.0);
}
