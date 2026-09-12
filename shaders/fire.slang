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

/*
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
*/

// Compactness: every open cover has a finite subcover.
// Visual: K = closed disk. Cover = open disks B(c_n, r_n) indexed by n.
// Show: a finite set of them already covers K.

float2 uv2 = (uv - 0.5) * float2(R.x/R.y, 1.0) * 2.2;

// --- K : the compact set (closed disk) ---
float2 kC = float2(0.0);
float  kR = 0.85;
float  dK = length(uv2 - kC);

// --- open cover: family of open disks whose union ⊇ K ---
// parametrize by integer n; use ~24 sample disks, animate their centers
float cover = 0.0;          // 1 if point is inside at least one open disk
float used  = 0.0;          // 1 if inside one of the FINITE subcover disks
int   N     = 24;           // total (pretend-infinite) family size shown

// phase: how many of the family we currently "activate" as the finite subcover
float activeF = 6.0 + 6.0 * (0.5 + 0.5*sin(T*0.6)); // cycles 6..12
int   active  = int(activeF);

for (int n = 0; n < N; n++) {
    float fn = float(n);
    // centers spiral around K, all at radius ~1.15 so their union covers K
    float a  = fn * 2.399963;                 // golden angle
    float rr = 1.15 + 0.15*sin(T*0.3 + fn);
    float2 c = float2(cos(a), sin(a)) * rr;

    // radii chosen so disks overlap generously and reach K's interior
    float r  = 0.75;

    float d = length(uv2 - c);
    float inside = smoothstep(r, r-0.01, d);  // soft edge

    cover = max(cover, inside);
    if (n < active) used = max(used, inside);
}

// --- drawing ---
float3 bg        = float3(0.02, 0.03, 0.08);

// K outline (compact set boundary)
float kEdge = smoothstep(0.015, 0.0, abs(dK - kR));

// point inside K ?
float inK = smoothstep(kR, kR-0.01, dK);

// colors matching your neon palette
float3 kColor    = float3(1.00, 0.85, 0.30);   // compact set = gold
float3 coverCol  = float3(0.20, 0.70, 1.00);   // open cover = neon blue
float3 subCol    = float3(0.45, 1.00, 0.55);   // finite subcover = green

// show the open cover disks faintly
float3 col = bg;
col += coverCol * cover * 0.10;

// highlight the finite subcover
col += subCol * used * 0.22;

// draw K boundary strongly
col += kColor * kEdge * 1.5;

// fill K slightly
col += kColor * inK * 0.10;

// outline each active subcover disk
for (int n = 0; n < N; n++) {
    float fn = float(n);
    float a  = fn * 2.399963;
    float rr = 1.15 + 0.15*sin(T*0.3 + fn);
    float2 c = float2(cos(a), sin(a)) * rr;
    float r  = 0.75;
    float ring = smoothstep(0.008, 0.0, abs(length(uv2-c) - r));
    float on = step(fn, activeF - 0.5);
    col += subCol * ring * on * 1.2;
    col += coverCol * ring * (1.0 - on) * 0.15;
}

return float4(col, 1.0);














return float4(color, 1.0);
}

