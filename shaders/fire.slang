// ============================================================
// Optimized procedural fire shader for Slang / Vulkan
//
// Main performance changes:
//
//   ORIGINAL                         OPTIMIZED
//   ---------------------------------------------------------
//   ray steps:       70             ray steps:       40
//   warp layers:      6             warp layers:      4
//   rotation: 4 cos calls           rotation: 1 sincos
//   color:    3 sin calls           color:    1 sincos
//   d /= 0.8                         freq *= 1.25
//   repeated 8*time                  precomputed time8
//
// The expensive part of this shader is:
//
//        40 rays
//          ×
//         4 warp layers
//          ×
//      vector cosine
//
//        = 160 warp evaluations / pixel
//
// Instead of:
//
//        70 × 6 = 420
//
// So the domain-warp portion alone is reduced by about 62%.
//
// Visual idea:
//
//                  RAY
//                   |
//                   v
//        +-----------------------+
//        | coarse deformation    |  frequency ~2.5
//        +-----------------------+
//                   |
//        +-----------------------+
//        | medium deformation    |  frequency ~3.125
//        +-----------------------+
//                   |
//        +-----------------------+
//        | finer deformation     |  frequency ~3.906
//        +-----------------------+
//                   |
//        +-----------------------+
//        | fine deformation      |  frequency ~4.883
//        +-----------------------+
//                   |
//                   v
//              density/SDF
//                   |
//                   v
//               next ray step
// ============================================================


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


// ============================================================
// Fullscreen triangle
//
//       (-1,+1)
//          *
//          | \
//          |  \
//          |   \
//          |    \
//          *-----*
//       (-1,-1) (+1,-1)
//
// One triangle covers the entire screen.
// ============================================================

[shader("vertex")]
VSOutput vs_main(uint id : SV_VertexID)
{
    static const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    VSOutput o;
    o.position = float4(positions[id], 0.0, 1.0);

    // Not actually needed by the optimized fragment shader,
    // but retained so the vertex interface remains familiar.
    o.uv = float2(0.0, 0.0);

    return o;
}


// ============================================================
// Fragment shader
// ============================================================

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target0
{
    const float PI = 3.14159265359;

    float2 resolution =
        float2(push.width, push.height);

    float time = push.time;

    // SV_Position.xy is already in pixel coordinates.
    //
    // Visual:
    //
    //        screen
    //    +----------------+
    //    |                |
    //    |       * C      |
    //    |                |
    //    +----------------+
    //
    // Flip Y so positive Y points upward in our procedural
    // coordinate system.
    float2 pixel = input.position.xy;
    pixel.y = resolution.y - pixel.y;


    // ========================================================
    // CAMERA RAY
    // ========================================================
    //
    // Original:
    //
    // normalize(
    //     float3(
    //         2*C.x - W,
    //         2*C.y - H,
    //         H
    //     )
    // )
    //
    // Center pixel approximately becomes:
    //
    //     (0, 0, 1)
    //
    // so the camera looks toward +Z.
    //
    // Every pixel gets a different normalized direction:
    //
    //                  +Z
    //                   ^
    //                  /
    //                 /
    //      \         /
    //       \       /
    //        \     /
    //         \   /
    //          \ /
    //           O
    //         camera
    //
    float3 rayDir = normalize(
        float3(
            2.0 * pixel.x - resolution.x,
            2.0 * pixel.y - resolution.y,
            resolution.y
        )
    );


    // ========================================================
    // RAY MARCH STATE
    // ========================================================

    float z = 0.0;

    // RGB accumulation.
    //
    // We do not need alpha during accumulation because the final
    // shader output always uses alpha = 1.
    float3 accum = 0.0;


    // Precompute values which otherwise would be repeatedly
    // reconstructed inside every ray step.
    //
    // This is a tiny optimization compared with reducing the
    // iteration count, but it costs essentially nothing.
    float time8 = time * 8.0;


    // ========================================================
    // RAY MARCH
    //
    // 40 instead of 70.
    //
    // This is the first major performance lever.
    // ========================================================

    [loop]
    for (int i = 0; i < 40; ++i)
    {
        // ----------------------------------------------------
        // Current point on ray
        //
        // p(z) = z * rayDir
        //
        //     camera
        //       O
        //        \
        //         \
        //          * p
        //
        // z is distance along the ray.
        // ----------------------------------------------------

        float3 p = z * rayDir;


        // ----------------------------------------------------
        // Animated depth translation
        //
        // Original:
        //
        //     p.z += 5 + cos(time);
        //
        // This creates:
        //
        //     base position = 5
        //     oscillation   = cos(time)
        //
        // range:
        //
        //     [4, 6]
        // ----------------------------------------------------

        p.z += 5.0 + cos(time);


        // ====================================================
        // HEIGHT-DEPENDENT XZ DEFORMATION
        // ====================================================
        //
        // Original code constructed FOUR cosine values:
        //
        //     cos(angle)
        //     cos(angle + 8)
        //     cos(angle + 5)
        //     cos(angle)
        //
        // but we only really need:
        //
        //     cos(angle)
        //     cos(angle + 5)
        //     cos(angle + 8)
        //
        // We can obtain all three from ONE sincos(angle).
        //
        // Identity:
        //
        //     cos(a+b) = cos(a)cos(b) - sin(a)sin(b)
        //
        // Therefore:
        //
        //     cos(a+5) = cos(a)cos(5) - sin(a)sin(5)
        //     cos(a+8) = cos(a)cos(8) - sin(a)sin(8)
        //
        // Instead of evaluating:
        //
        //     cos(a)
        //     cos(a+5)
        //     cos(a+8)
        //
        // we evaluate:
        //
        //     sin(a), cos(a)
        //
        // once.
        // ====================================================

        float angle =
            time +
            p.y * 0.25;


        float sinAngle;
        float cosAngle;

        sincos(angle, sinAngle, cosAngle);


        // ----------------------------------------------------
        // Precomputed constants.
        //
        // cos(5) =  0.2836621855
        // sin(5) = -0.9589242747
        //
        // cos(8) = -0.1455000338
        // sin(8) =  0.9893582468
        // ----------------------------------------------------

        float cosAngle5 =
            cosAngle * 0.2836621855
          - sinAngle * (-0.9589242747);

        float cosAngle8 =
            cosAngle * (-0.1455000338)
          - sinAngle * 0.9893582468;


        // ----------------------------------------------------
        // Original:
        //
        //     p.xz *= mat2(...)
        //
        // Explicitly expand the matrix multiplication instead
        // of constructing a matrix object.
        //
        // This is:
        //
        //     [x' z'] = [x z] M
        //
        // where M changes with height.
        // ----------------------------------------------------

        float px = p.x;
        float pz = p.z;

        p.x =
            px * cosAngle
          + pz * cosAngle5;

        p.z =
            px * cosAngle8
          + pz * cosAngle;


        // ====================================================
        // MULTI-SCALE DOMAIN WARP
        // ====================================================
        //
        // Original:
        //
        //     d = 2;
        //
        //     for 6 iterations
        //     {
        //         d /= 0.8;
        //         p += cos(...) / d;
        //     }
        //
        // Important subtlety:
        //
        // d is divided BEFORE it is used.
        //
        // Therefore the actual first frequency was:
        //
        //     2 / 0.8 = 2.5
        //
        // not 2.
        //
        // Frequencies:
        //
        //     2.5
        //     3.125
        //     3.90625
        //     4.8828125
        //
        // for our four layers.
        //
        // Instead of division every iteration:
        //
        //     d /= 0.8
        //
        // use multiplication by:
        //
        //     1.25
        //
        // because:
        //
        //     1 / 0.8 = 1.25
        //
        // At the same time the displacement amplitude is:
        //
        //     1 / frequency
        //
        // so we keep an inverse-frequency variable and multiply
        // it by 0.8 each iteration.
        //
        //
        //              frequency
        //
        //              2.50
        //                |
        //               ×1.25
        //                v
        //              3.125
        //                |
        //               ×1.25
        //                v
        //              3.906
        //                |
        //               ×1.25
        //                v
        //              4.883
        //
        //                  +
        //
        //       higher frequency
        //              ↓
        //
        //       smaller amplitude
        // ====================================================

        float frequency = 2.5;
        float inverseFrequency = 0.4;


        [loop]
        for (int j = 0; j < 4; ++j)
        {
            // ------------------------------------------------
            // Equivalent to:
            //
            // float3 shifted =
            //     float3(
            //         p.y - 8*time,
            //         p.z,
            //         p.x
            //     );
            //
            // time8 was precomputed above.
            // ------------------------------------------------

            float3 shifted =
                float3(
                    p.y - time8,
                    p.z,
                    p.x
                );


            // ------------------------------------------------
            // Procedural displacement.
            //
            //     displacement =
            //         cos(shifted * frequency + time)
            //         / frequency
            //
            // The cosine produces a smooth 3D wave.
            //
            // Every layer:
            //
            //     higher frequency
            //     lower amplitude
            //
            // which creates fractal-like detail.
            // ------------------------------------------------

            p +=
                cos(shifted * frequency + time)
                * inverseFrequency;


            // Move to the next frequency.
            frequency *= 1.25;

            // Since amplitude = 1 / frequency,
            // multiplying frequency by 1.25 means amplitude
            // gets multiplied by 0.8.
            inverseFrequency *= 0.8;
        }


        // ====================================================
        // DISTANCE / DENSITY FIELD
        // ====================================================
        //
        // Base geometry:
        //
        //     length(p.xz) + 0.3*p.y - 1
        //
        // If:
        //
        //     r = length(p.xz)
        //
        // then the zero surface is:
        //
        //     r + 0.3y - 1 = 0
        //
        //     r = 1 - 0.3y
        //
        // which resembles a cone.
        //
        // The absolute value turns this into an approximate
        // distance-to-shell quantity.
        // ====================================================

        float radial =
            sqrt(
                p.x * p.x +
                p.z * p.z
            );

        float d =
            0.01
          + abs(
                radial +
                p.y * 0.3
                - 1.0
            ) / 9.0;


        // ----------------------------------------------------
        // Advance along the ray.
        //
        // Small d:
        //     close to geometry
        //     small march
        //
        // Large d:
        //     far from geometry
        //     large march
        // ----------------------------------------------------

        z += d;


        // ====================================================
        // EMISSION / COLOR
        // ====================================================
        //
        // Original:
        //
        //     sin(a)
        //     sin(a - 1)
        //     sin(a - 2)
        //
        // where:
        //
        //     a = p.y * 0.5
        //
        // Instead of THREE independent sine evaluations,
        // calculate sin(a), cos(a) ONCE.
        //
        // Identity:
        //
        //     sin(a-b)
        //       =
        //     sin(a)cos(b) - cos(a)sin(b)
        //
        // Therefore:
        //
        //     sin(a-1) = sin(a)cos(1) - cos(a)sin(1)
        //
        //     sin(a-2) = sin(a)cos(2) - cos(a)sin(2)
        // ====================================================

        float colorAngle = p.y * 0.5;

        float sinColor;
        float cosColor;

        sincos(
            colorAngle,
            sinColor,
            cosColor
        );


        // ----------------------------------------------------
        // RGB phase-shifted sine.
        //
        // sin(a)
        // sin(a-1)
        // sin(a-2)
        // ----------------------------------------------------

        float3 wave;

        wave.x =
            sinColor;

        wave.y =
            sinColor * 0.5403023059
          - cosColor * 0.8414709848;

        wave.z =
            sinColor * (-0.4161468365)
          - cosColor * 0.9092974268;


        // ----------------------------------------------------
        // Inverse-distance weighting.
        //
        // Small d -> strong emission
        // Large d -> weak emission
        //
        //             1
        // density ~ -----
        //              d
        // ----------------------------------------------------

        accum +=
            (wave + 1.1) / d;


        // ----------------------------------------------------
        // Optional early-out.
        //
        // Once the ray is sufficiently far away from the
        // procedural volume, continuing to march is mostly
        // wasted work.
        //
        // Remove this if your particular scene needs a larger
        // depth range.
        // ----------------------------------------------------

        if (z > 20.0)
            break;
    }


    // ========================================================
    // TONE MAPPING
    // ========================================================
    //
    // tanh compresses a huge accumulated range into a usable
    // display range.
    //
    // Instead of:
    //
    //     hard clamp
    //
    // we get:
    //
    //     0 ---- smooth ----> 1
    //
    // ========================================================

    float3 fire =
        tanh(accum / 442.0);

    return float4(fire, 1.0);
}
