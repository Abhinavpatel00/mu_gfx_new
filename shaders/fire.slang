#include "common.slang"


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

float lp_norm(float2 v, float p)
{
    return pow(pow(abs(v.x), p) + pow(abs(v.y), p), 1.0 / p);
}



[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target0
{
    float2 R = global_data.screen_params.xy;
    float  T = global_data.time;
    float2 uv = pixel_to_uv(input.position.xy, R);
 //   float r = length(uv);
    //float color = dot(uv, uv) <=  0.2;
    //float color = abs(uv.x) +abs(uv.y) <= 0.4;
//    float color = max(abs(uv.x),abs(uv.y)) <= 0.4;
/*
 cool rounded shape form lp spaces experiment the more u increase p the less  rounded sq will become
float p = 3.0;
float r = 0.6;
float c = pow(abs(uv.x), p) + pow(abs(uv.y), p) <= pow(r, p);
*/
//       - 0.5 is coordinate shift to center
//
/*

float2 g = abs(frac( 6*uv) - 0.5);

//float line = min(g.x, g.y);
float line = 0.5 - min(g.x, g.y);   // 0.5 at midline, 0 at boundary
// float c = smoothstep(0.0, 0.5, line); // invert: bright near boundary
//
//
// float color =c;
//

line += sin(T*line);
    // --- line mask + glow ---
    float thickness = 0.08;           // core line half-width in cell units
    float core  = smoothstep(thickness, 0.0, line);      // 1 on line, 0 off
    float glow  = exp(-line * 12.0);                     // soft bloom falloff

    // --- neon blue ---
    float3 lineColor = float3(0.20, 0.70, 1.00);
    float3 bgColor   = float3(0.02, 0.03, 0.08);

    float3 color = bgColor
                 + lineColor * glow * 0.8     // bloom
                 + lineColor * core * 1.5;    // hot core (over-bright)

return float4( color, 1.0);
*/
float2 cell = frac(6.0 * uv) - 0.5;         // per-cell coords, keeps full-screen grid
float d = max(abs(cell.x), abs(cell.y));    // Chebyshev: 0 at cell center, 0.5 at edge

// --- three rings cycling outward ---
float speed = 0.35;
float r1 = frac(T * speed + 0.0 / 3.0) * 0.5;
float r2 = frac(T * speed + 1.0 / 3.0) * 0.5;
float r3 = frac(T * speed + 2.0 / 3.0) * 0.5;

// distance to nearest of the three square outlines
float line = min(min(abs(d - r1), abs(d - r2)), abs(d - r3));

// keep the original wobble
line += 0.01 * sin(T * line);

// --- line mask + glow ---
float thickness = 0.012;
float core  = smoothstep(thickness, 0.0, line);
float glow  = exp(-line * 40.0);

// fade out as rings approach the cell edge so they don't pop hard
float edgeFade = smoothstep(0.5, 0.35, d);

// --- neon blue (same palette) ---
float3 lineColor = float3(0.20, 0.70, 1.00);
float3 bgColor   = float3(0.02, 0.03, 0.08);

float3 color = bgColor
             + lineColor * glow * 0.8 * edgeFade
             + lineColor * core * 1.5 * edgeFade;

return float4(color, 1.0);
}

