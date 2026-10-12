#include "common.slang"

/* Screen-space ambient occlusion, half resolution, single dispatch.
   Normal-oriented hemisphere sampling with a rotated Poisson disk and a
   per-sample radius jitter, which spreads the estimates over the hemisphere
   volume instead of pinning them to one shell. Normals are reconstructed from
   screen-space depth derivatives, so the scene needs no normal target.

   No temporal accumulation: the disk rotation and the 16 samples are enough
   that the residual noise is high-frequency and the depth-aware upsample in
   postprocess.slang averages it away at the full-res edge it lands on. That
   keeps AO free of history, disocclusion tests and the smearing that comes
   with them. */

struct SsaoPush {
    uint  depth_texture_id;
    uint  output_image_id;
    uint  sampler_id;
    uint  width;   // AO target size (half res)
    uint  height;
    uint  sample_count; // <= 16, the disk length
    uint  frame;
    float proj_00;
    float proj_11;
    float radius;   // world-space occlusion radius
    float bias;     // view-space depth the occluder must beat to count
    float intensity; // shaping exponent on the raw AO
    float near_z;
};
[[vk::push_constant]] SsaoPush push;

static const float Pi2 = 6.2831853071795865;

/* Poisson disk, radius 1. Rotating it per pixel is what turns the fixed
   pattern into noise the upsample can average, rather than banding. */
static const float2 Poisson[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870),
    float2(0.34495938, 0.29387760),   float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845),  float2(0.97484398, 0.75648379),  float2(0.44323325, -0.97511554),
    float2(0.53742981, -0.47373420),  float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507),  float2(-0.81409955, 0.91437590),  float2(0.19984126, 0.78641367),
    float2(0.14383161, -0.14100790)};

uint IGN(int2 p) {
    int x = p.x * 1664525 + 1013904223;
    int y = p.y * 1664525 + 1013904223;
    return uint(x ^ y);
}

// [0,1] from the high bits of a per-pixel, per-frame hash.
float Rnd(int2 p, uint f) {
    uint h = IGN(p);
    h += f * 0x9E3779B9u;
    h ^= h >> 15u;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12u;
    return float(h & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

// Reverse-Z reconstruction: depth stores near/dist, so view z = -dist.
float3 ViewPos(float2 ndc, float depth, float p00, float p11, float nearZ) {
    float dist = nearZ / max(depth, 1e-4);
    return float3(ndc.x / p00, ndc.y / p11, -1.0) * dist;
}

// Project a view-space point back to a [0,1] depth-buffer coordinate.
float2 ProjectUv(float3 p, float p00, float p11) {
    return float2(p.x * p00 / max(-p.z, 1e-4), p.y * p11 / max(-p.z, 1e-4)) * 0.5 + 0.5;
}

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.width || id.y >= push.height)
        return;
    int2 pix = int2(id.xy);

    Texture2D           depth_tex = textures[push.depth_texture_id];
    SamplerState        smp       = samplers[push.sampler_id];
    RWTexture2D<float4> out_img   = storage_images[push.output_image_id];

    float2 texel = 1.0 / float2(push.width, push.height);
    float2 uv    = (float2(pix) + 0.5) * texel;

    float z = depth_tex.SampleLevel(smp, uv, 0.0).r;
    // reverse-Z clear is 0.0: sky has no surface to occlude.
    if (z <= 1e-4) {
        out_img[pix] = float4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    float2 ndc = uv * 2.0 - 1.0;
    float3 pos = ViewPos(ndc, z, push.proj_00, push.proj_11, push.near_z);

    /* Normal from depth derivatives, keeping the neighbour on the same surface:
       across a silhouette the far one would tilt the normal into the gap. */
    float  zr = depth_tex.SampleLevel(smp, uv + float2(texel.x, 0.0), 0.0).r;
    float  zl = depth_tex.SampleLevel(smp, uv - float2(texel.x, 0.0), 0.0).r;
    float  zu = depth_tex.SampleLevel(smp, uv + float2(0.0, texel.y), 0.0).r;
    float  zd = depth_tex.SampleLevel(smp, uv - float2(0.0, texel.y), 0.0).r;
    float3 pr = ViewPos(float2(ndc.x + 2.0 * texel.x, ndc.y), max(zr, 1e-4), push.proj_00, push.proj_11, push.near_z);
    float3 pl = ViewPos(float2(ndc.x - 2.0 * texel.x, ndc.y), max(zl, 1e-4), push.proj_00, push.proj_11, push.near_z);
    float3 pu = ViewPos(float2(ndc.x, ndc.y + 2.0 * texel.y), max(zu, 1e-4), push.proj_00, push.proj_11, push.near_z);
    float3 pd = ViewPos(float2(ndc.x, ndc.y - 2.0 * texel.y), max(zd, 1e-4), push.proj_00, push.proj_11, push.near_z);
    float3 dx = (abs(pr.z - pos.z) < abs(pl.z - pos.z)) ? (pr - pos) : (pos - pl);
    float3 dy = (abs(pu.z - pos.z) < abs(pd.z - pos.z)) ? (pu - pos) : (pos - pd);
    float3 N  = normalize(cross(dx, dy));
    if (dot(N, pos) > 0.0)
        N = -N; // face the camera

    /* Tangent frame around N. The 0.999 branch only picks a non-parallel seed;
       abs() rather than a comparison keeps the two branches the same width. */
    float3 T = normalize(cross(abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0), N));
    float3 B = cross(N, T);

    /* Keep the sample pattern stable. A new rotation every frame makes half-
       resolution AO shimmer, while a per-pixel white-noise rotation remains
       visible as grain after the low-resolution target is upsampled. */
    float  ang = 0.0;
    float2 rot = float2(cos(ang), sin(ang));

    float occlusion = 0.0;
    for (uint i = 0u; i < push.sample_count; ++i) {
        float2 d = Poisson[i];
        d        = float2(d.x * rot.x - d.y * rot.y, d.x * rot.y + d.y * rot.x);

        /* Lift the disk point onto the hemisphere and jitter its length so the
           samples fill the volume rather than one shell. */
        float  zc     = sqrt(max(0.0, 1.0 - dot(d, d)));
        float  jitter = 1.0;
        float3 dir    = T * d.x + B * d.y + N * zc;
        float3 sPos   = pos + dir * (push.radius * jitter);

        float2 suv = ProjectUv(sPos, push.proj_00, push.proj_11);
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0)
            continue;
        float sz = depth_tex.SampleLevel(smp, suv, 0.0).r;
        if (sz <= 1e-4)
            continue; // the sample fell on sky

        float3 real = ViewPos(suv * 2.0 - 1.0, sz, push.proj_00, push.proj_11, push.near_z);
        /* Occluded when the surface under the sample sits in front of the sample
           point (less negative z is closer). The range check drops far occluders
           so a distant wall does not halo a nearby floor. */
        float  range = smoothstep(0.0, 1.0, push.radius / max(abs(real.z - pos.z), 1e-4));
        occlusion += (real.z >= sPos.z + push.bias ? 1.0 : 0.0) * range;
    }

    float ao = 1.0 - occlusion / float(push.sample_count);
    ao       = pow(saturate(ao), push.intensity);
    out_img[pix] = float4(ao, ao, ao, 1.0);
}
