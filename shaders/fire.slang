
#include "common.slang"
struct Quat { float4 v; };  // v.x = w, v.yzw = (x, y, z)

Quat q_identity() { return Quat(float4(1, 0, 0, 0)); }

Quat q_mul(Quat a, Quat b) {
    float aw = a.v.x, ax = a.v.y, ay = a.v.z, az = a.v.w;
    float bw = b.v.x, bx = b.v.y, by = b.v.z, bz = b.v.w;
    return Quat(float4(
        aw * bw - ax * bx - ay * by - az * bz,
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw
    ));
}

Quat q_conj(Quat q) { return Quat(float4(q.v.x, -q.v.yzw)); }

float q_len(Quat q) { return length(q.v); }

Quat q_normalize(Quat q) { return Quat(q.v / q_len(q)); }

// Rotate a 3D vector. q must be unit length.
float3 q_rotate(Quat q, float3 v) {
    // v' = q * (0, v) * q^-1, expanded for efficiency
    float3 u = q.v.yzw;
    float  s = q.v.x;
    return v + 2.0 * cross(u, cross(u, v) + s * v);
}

// Axis-angle → quaternion
Quat q_from_axis_angle(float3 axis, float angle) {
    float h = angle * 0.5;
    float s = sin(h);
    return Quat(float4(cos(h), normalize(axis) * s));
}

// Slerp — useful to see the "shortest arc" property
Quat q_slerp(Quat a, Quat b, float t) {
    float d = dot(a.v, b.v);
    if (d < 0.0) { b = Quat(-b.v); d = -d; }
    if (d > 0.9995) return q_normalize(Quat(a.v + (b.v - a.v) * t));
    float th0 = acos(clamp(d, -1.0, 1.0));
    float th  = th0 * t;
    float s0  = sin(th0 - th) / sin(th0);
    float s1  = sin(th) / sin(th0);
    return Quat(a.v * s0 + b.v * s1);
}

// ===================== HOPF FIBRATION =====================
// Every glowing circle is the stereographic image of one Hopf fiber,
// i.e. the circle { q * (cos a, sin a, 0, 0) : a } of unit quaternions.

float3 pal(float t)   // shared cosine palette
{
    return 0.55 + 0.45*cos(6.28318*(t*0.9 + float3(0.0, 0.28, 0.55)));
}

float3 shader_hopf(float2 uv, float T)
{
    const int   N    = 9;      // fibers — raise for density
    const float GLOW = 700.0;   // line sharpness

    // slowly orbiting camera
    float ca = 0.28*T;
    float el = 0.45 + 0.18*sin(0.19*T);
    float3 ro = 3.4*float3(cos(el)*sin(ca), sin(el), cos(el)*cos(ca));
    float3 fw = normalize(-ro);
    float3 rt = normalize(cross(fw, float3(0,1,0)));
    float3 up = cross(rt, fw);
    float3 rd = normalize(uv.x*rt + uv.y*up + 1.75*fw);

    float3 col = float3(0.010, 0.014, 0.032);

    [loop]
    for (int i = 0; i < N; i++)
    {
        float f = ((float)i + 0.5) / (float)N;

        // base points spread over S^2 (cosine-distributed), precessing
        float theta = clamp(acos(clamp(1.0 - 2.0*f, -1.0, 1.0)), 0.07, 3.07);
        float phi   = f * 2.39996323 + 0.5*T;          // golden angle

        // analytic projected fiber (derivation above)
        float ct = cos(theta*0.5), st = sin(theta*0.5);
        float3 C = (st/ct) * float3(-sin(phi), cos(phi), 0.0);
        float3 n = float3(-st*cos(phi), -st*sin(phi), ct);
        float  r = 1.0/ct;

        // ray vs. the circle's plane -> distance to circle
        float dn = dot(rd, n);
        if (abs(dn) < 0.0001) continue;
        float t = dot(C - ro, n) / dn;
        if (t < 0.05) continue;

        float3 h = ro + t*rd;
        float  d = length(h - C) - r;

        float att    = 1.0 / (1.0 + 0.13*t*t);         // depth cue
        float3 tint  = pal(0.05 + theta*0.30);         // color = latitude on S^2

        col += tint * exp(-d*d*GLOW)     * att * 0.45; // halo
        col += tint * exp(-d*d*GLOW*6.0) * att * 1.30; // hot core
    }

    return col;                       
}



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

/*
 float2 C = input.position.xy;


 float3 ray_origin = float3(0.0, 0.0, 5.0 + cos(T));

    // --- Ray direction (unchanged) ---
float3 rd = normalize(float3(
    uv.x,
    uv.y,
    -1.0
));
    float  z   = 0.0;
    float3 col = 0.0;

    // Inner-loop constants (lacunarity 1.25, gain 0.8)
    static const float FREQ[6] = { 2.5000000, 3.1250000, 3.9062500, 4.8828125, 6.1035156, 7.6293945 };
    static const float AMP [6] = { 0.4000000, 0.3200000, 0.2560000, 0.2048000, 0.1638400, 0.1310720 };

    const float STEP_SCALE = 1.0 / 9.0;

    [loop]
    for (int i = 0; i < 75; ++i)
    {
        float3 p = ray_origin + z * rd;

        float a = T + p.y * 0.25;
        float sa = sin(a), ca = cos(a);

        float c5 = cos(a + 5.0), s5 = sin(a + 5.0);
        float c8 = cos(a + 8.0), s8 = sin(a + 8.0);

        // Two consecutive Y-axis rotations: first by (a+5), then by (a+8).
        float2 xz = p.xz;
        float2 r5 = float2(xz.x * c5 - xz.y * s5,
                           xz.x * s5 + xz.y * c5);
        float2 r58 = float2(r5.x * c8 - r5.y * s8,
                            r5.x * s8 + r5.y * c8);
        p.x = r58.x;
        p.z = r58.y;

        [unroll]
        for (int j = 0; j < 6; ++j)
        {
            float3 q = float3(p.y - 8.0 * T, p.z, p.x);
            p += cos(q * FREQ[j] + T) * AMP[j];
        }

        float d = 0.01 + abs(length(p.xz) + p.y * 0.3 - 1.0) * STEP_SCALE;

        float y  = p.y * 0.5;
        float3 emission = float3(sin(y), sin(y - 1.0), sin(y - 2.0)) + 1.1;

        col += emission / d;

        z += d;
        if (z > 12.0) break;   // far plane
    }

    col = tanh(col * 0.001);   // default ~0.001

*/
/*


   // ------------------------------------------------------------------
    // 1) Time-varying unit quaternion  q = (cos(θ/2), sin(θ/2)·n̂)
    // ------------------------------------------------------------------
    float3 axis  = normalize(float3(sin(0.5 * T),cos(0.5 * T),1.0));
    float  theta = 1.5 + 1.2 * frac(0.4 * T);      // ≈ [0.3, 2.7] rad
    Quat   q     = q_from_axis_angle(axis, theta);


    // ------------------------------------------------------------------
    // 2) Map each pixel to a 3D direction on the front hemisphere.
    //    v is the vector "seen" through that pixel.
    // ------------------------------------------------------------------
    float  r2 = dot(uv, uv);
    float3 v  = float3(uv, sqrt(1.0 - r2));   // |v| = 1

    // ------------------------------------------------------------------
    // 3) Rotate it:  rv = q v q*
    // ------------------------------------------------------------------
    float3 rv = q_rotate(q, v);

    // ------------------------------------------------------------------
    // 4) Draw.
    //    • RGB = rotated direction → the sphere appears to spin.
    //    • Dark core where rv == v → the two points where the axis
    //      pierces the sphere. Those are the *only* directions q
    //      leaves invariant.
    // ------------------------------------------------------------------
    float3 col = rv * 0.5 + 0.5;

    float moved = length(rv - v);              // 0 exactly on the axis
    col = lerp(float3(0.04, 0.04, 0.07), col,
               smoothstep(0.0, 0.22, moved));

    // Dark background outside the sphere
    float mask = step(r2, 1.0);
    col = lerp(float3(0.015, 0.02, 0.035), col, mask);
*/




 float3 col = shader_hopf(uv, T);
return float4(col, 1.0);
}
