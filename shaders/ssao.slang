#include "common.slang"

/* Unidirectional partial-slice GT-VBAO + temporal accumulation.
   Ground-truth Visibility Bitmask AO (https://arxiv.org/abs/2301.11376), the
   variant that needs no bidirectional ray marching
   (https://www.shadertoy.com/view/Xc3yzs). Perspective-correct, cosine-weighted
   hemisphere. Each slice is a 32-bit occupancy mask; AO is 1 - occluded bits/32.

   Marches one direction per slice and uses a partial-slice remap (w0) so the
   single march still covers the hemisphere. Runs 1 sample per frame and blends
   into an RGBA16F history buffer (.r = accumulated AO, .g = sample count,
   .b = the view distance the texel saw when it was written), reprojecting
   through last frame's view-proj. Normals are reconstructed from screen-space
   depth derivatives, so no gbuffer normal target is needed. */

struct SsaoPush {
    uint  depth_texture_id;
    uint  output_image_id;
    uint  sampler_id;
    uint  width;   // AO target size (half res)
    uint  height;
    uint  sample_count;
    // 1 = compute AO, 0 = reset history (write neutral AO=1, count=0)
    uint  mode;
    float proj_00;
    float proj_11;
    float radius;
    float thickness;
    float bias;
    float intensity;
    float near_z;
    float far_z;
    // depth texels per AO texel: the march counts steps in depth-buffer pixels
    float depth_scale;
    uint  history_texture_id;
};
[[vk::push_constant]] SsaoPush push;

static const float Pi    = 3.1415926535897932;
static const float Pi2   = 6.2831853071795865;
static const float RcpPi = 0.3183098861837907;
static const float Pi05  = 1.5707963267948966;

static const float Raymarching_Width   = 512.0;
static const float Raymarching_Samples = 32.0;
static const uint  Slices              = 32u;

static const float MaxHistory = 16.0;      // frames before the blend stops trusting history
static const float MotionAlphaScale = 0.15; // blend floor per AO texel of reprojection drift
static const float ClampMargin = 0.1;      // slack the neighbourhood clamp allows around its range

uint CountBits(uint v) {
    v = v - ((v >> 1u) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2u) & 0x33333333u);
    return ((v + (v >> 4u) & 0xF0F0F0Fu) * 0x1010101u) >> 24u;
}

// improved GTAOFastAcos: minimizes max |approx(cos x) - x| for one extra mad.
float ACosPoly(float x) {
    return 1.5707963267948966 + (-0.20491203466059038 + 0.04832927023878897 * x) * x;
}

float ACos(float x) {
    float ax = clamp(abs(x), 0.0, 1.0);
    float u  = ACosPoly(ax) * sqrt(1.0 - ax);
    return x >= 0.0 ? u : Pi - u;
}

float ASin01_Approx(float x) { return Pi05 - ACosPoly(x) * sqrt(1.0 - x); }

// ArcTan(dir) / Pi, dir normalized. Max abs error ~4.5e-5.
float ArcTan11(float2 dir) {
    float x = abs(dir.x);
    float y = dir.y;
    float u = 2.0 + x * (1.27324 + x * (-0.189431 + (0.08204 - 0.0242564 * x) * x));
    float f = y / u;
    if (dir.x < 0.0)
        f = (dir.y < 0.0 ? -1.0 : 1.0) - f;
    return f;
}

float2 cmul(float2 c0, float2 c1) {
    return float2(c0.x * c1.x - c0.y * c1.y, c0.y * c1.x + c0.x * c1.y);
}

// importance-sample the partial-slice angle from one random number.
float SamplePartialSlice(float x, float sin_thVN) {
    if (x == 0.0 || abs(x) >= 1.0)
        return x;
    bool   sgn = x < 0.0;
    x          = abs(x);
    float s    = sin_thVN;
    float o    = s - s * s;
    float slp0 = 1.0 / (1.0 + (Pi - 1.0) * (s - o * 0.30546));
    float slp1 = 1.0 / (1.0 - (1.0 - exp2(-20.0)) * (s + o * (0.5 + (0.785 - 0.5) * s)));
    float k    = 0.1 + (0.25 - 0.1) * s;
    float a    = 1.0 - (Pi - 2.0) / (Pi - 1.0);
    float b    = 1.0 / (Pi - 1.0);
    float d0   = a - slp0 * b;
    float d1   = 1.0 - slp1;
    float f0   = d0 * (Pi * x - ASin01_Approx(x));
    float f1   = d1 * (x - 1.0);
    float kk   = k * k;
    float h0   = sqrt(f0 * f0 + kk) - k;
    float h1   = sqrt(f1 * f1 + kk) - k;
    float hh   = (h0 * h1) / (h0 + h1);
    float y    = x - sqrt(hh * (hh + 2.0 * k));
    return sgn ? -y : y;
}

// vvsN: view-vec-space normal | rnd01: [0,1] -> in-plane slice direction.
float2 SamplePartialSliceDir(float3 vvsN, float rnd01) {
    float  ang0 = rnd01 * Pi2;
    float2 dir0 = float2(cos(ang0), sin(ang0));
    float  l    = length(vvsN.xy);
    if (l == 0.0)
        return dir0;
    float2 n = vvsN.xy / l;
    dir0     = cmul(dir0, n * float2(1.0, -1.0)); // align n with x-axis
    float x  = ArcTan11(dir0);
    float ang = SamplePartialSlice(x, l) * Pi;
    float2 dir = float2(cos(ang), sin(ang));
    return cmul(dir, n); // align x-axis back with n
}

// Quaternion that rotates (0,0,-1) -> to (reference convention).
float4 QuatZNegTo(float3 to) {
    to   = normalize(to);
    float3 xyz = float3(to.y, -to.x, 0.0); // cross((0,0,-1), to)
    float  s   = -to.z;                     // dot((0,0,-1), to)
    float  u   = rsqrt(max(0.0, s * 0.5 + 0.5));
    s     = 1.0 / u;
    xyz  *= u * 0.5;
    return float4(xyz, s);
}

float3 TransformQ(float3 v, float4 q) {
    float3 k = cross(q.xyz, v);
    return v + 2.0 * float3(dot(float3(q.wy, -q.z), k.xzy), dot(float3(q.wz, -q.x), k.yxz),
                            dot(float3(q.wx, -q.y), k.zyx));
}

float3 Transform_Qz0(float3 v, float4 q) {
    float k = v.y * q.x - v.x * q.y;
    float g = 2.0 * (v.z * q.w + k);
    return float3(v.xy + q.yx * float2(g, -g), v.z + 2.0 * (q.w * k - v.z * dot(q.xy, q.xy)));
}

float3 Transform_Vz0Qz0(float2 v, float4 q) {
    float o = q.x * v.y;
    float c = q.y * v.x;
    float3 b = float3(o - c, -o + c, o - c);
    return float3(v, 0.0) + 2.0 * (b * q.yxw);
}

// Reverse-Z reconstruction: depth buffer stores near/dist, so view z = -dist.
float3 ViewPos(float2 ndc, float depth, float p00, float p11, float nearZ) {
    float dist = nearZ / max(depth, 1e-4);
    return float3(ndc.x / p00, ndc.y / p11, -1.0) * dist;
}

uint IGN(int2 p) {
    int x = p.x * 1664525 + 1013904223;
    int y = p.y * 1664525 + 1013904223;
    return uint(x ^ y);
}

// [0,1] from the high bits of a per-pixel, per-frame hash.
float Rnd(int2 p, uint f, uint salt) {
    uint h = IGN(p + int2(int(salt), int(salt << 1)));
    h += f * 0x9E3779B9u;
    h ^= h >> 15u;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12u;
    return float(h & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

float SliceRelCDF_Cos(float x, float angN, float cosN, bool isPhiLargerThanAngN) {
    if (x <= 0.0 || x >= 1.0)
        return x;
    float phi = x * Pi - Pi05;
    float n0  = isPhiLargerThanAngN ? 3.0 : 1.0;
    float n1  = isPhiLargerThanAngN ? -1.0 : 1.0;
    float n2  = isPhiLargerThanAngN ? 4.0 : 0.0;
    float t0  = n0 * cosN + n1 * cos(angN - 2.0 * phi) + (n2 * angN + (n1 * 2.0) * phi + Pi) * sin(angN);
    float t1  = 4.0 * (cosN + angN * sin(angN));
    return t0 / t1;
}

// One unidirectional partial-slice. pos/N/V are view space; p00/p11 the
// projection diagonal; frame advances the per-pixel noise. Returns AO in [0,1].
float uniGTVBAO(int2 pix, float3 pos, float3 N, float3 V, float p00, float p11, uint frame) {
    /* Step in depth-buffer pixels, not AO pixels: the depth target is larger
       than this one, so a step short of a depth texel walks straight past
       geometry the ray should have hit. uv below is unchanged by the scale. */
    float2 res         = float2(push.width, push.height) * push.depth_scale;
    float2 rayStartPix = (float2(pix) + 0.5) * push.depth_scale;
    float  ao          = 0.0;
    for (uint i = 0u; i < push.sample_count; ++i) {
        uint f = frame * push.sample_count + i;
        float rs = Rnd(pix, f, 1u);
        float rz = Rnd(pix, f, 2u);
        float rw = Rnd(pix, f, 3u);

        // partial-slice direction: sample around the view vector (view-vec
        // space), lift into view space, project a near point for a screen dir.
        float4 Q_toV   = QuatZNegTo(V);
        float4 Q_fromV = float4(-Q_toV.xyz, Q_toV.w);
        float3 Nvvs    = Transform_Qz0(N, Q_fromV);
        float2 dir     = SamplePartialSliceDir(Nvvs, rs);
        float3 smplDirVS = Transform_Vz0Qz0(dir, Q_toV);

        float3 rayEndVS = pos + smplDirVS * (push.near_z * 0.5);
        /* Projection uses w = -view.z (right-handed camera looking down -Z).
           Dividing by view.z mirrors the direction across the screen center. */
        float2 rayEndNdc = float2(rayEndVS.x * p00 / -rayEndVS.z, rayEndVS.y * p11 / -rayEndVS.z);
        float2 rayEndPix = (rayEndNdc * 0.5 + 0.5) * res;
        float2 dxy = rayEndPix - rayStartPix;
        dxy /= max(length(dxy), 1e-6);

        // construct the slice
        float3 sliceN = cross(V, smplDirVS);
        float3 projN = N - sliceN * dot(N, sliceN);
        float projNSqrLen = dot(projN, projN);
        if (projNSqrLen == 0.0)
            return i == 0u ? 1.0 : ao / float(i); // degenerate slice: keep what the earlier samples found
        float projNRcpLen = rsqrt(projNSqrLen);
        float cosN = dot(projN, V) * projNRcpLen;
        float3 T = cross(sliceN, projN);
        float sgn = dot(V, T) < 0.0 ? -1.0 : 1.0;
        float angN = sgn * ACos(cosN);
        float angOff = angN * RcpPi + 0.5;

        // fraction of the slice we don't march; remap so the single march
        // still covers the full used arc.
        float w0 = clamp((sin(angN) / (cosN + angN * sin(angN))) * (Pi / 4.0) + 0.5, 0.0, 1.0);
        float w0_remap_mul = 1.0 / (1.0 - w0);
        float w0_remap_add = -w0 * w0_remap_mul;

        /* The slice only has to cross the sampling radius on screen. Marching a
           fixed 512 pixels instead spends most of the 32 steps past the radius,
           where the sample is rejected for being too far anyway. A world length
           L at view distance d covers L * p11 * height/2 / d pixels. */
        float width = clamp(2.0 * push.radius * abs(p11) * (0.5 * res.y) / max(-pos.z, 1e-4), 16.0,
                            Raymarching_Width);
        uint  occBits = 0u;
        float s = pow(width, 1.0 / Raymarching_Samples);
        float t = pow(s, rz);
        for (float step = 0.0; step < Raymarching_Samples; ++step) {
            float2 samplePix = rayStartPix + dxy * t;
            t *= s;
            if (samplePix.x < 0.0 || samplePix.x >= res.x || samplePix.y < 0.0 || samplePix.y >= res.y)
                break;
            float2 uv = samplePix / res;
            float depth = textures[push.depth_texture_id].SampleLevel(samplers[push.sampler_id], uv, 0.0).r;
            /* Reverse-Z clear depth is zero. Sky has no finite view position
               and must not contribute a false horizon. */
            if (depth <= 1e-4)
                continue;
            float3 sampleVS = ViewPos(uv * 2.0 - 1.0, depth, p00, p11, push.near_z);
            float3 deltaFront = sampleVS - pos;
            float sampleDistance = length(deltaFront);
            if (sampleDistance <= push.bias || sampleDistance > push.radius)
                continue;
            // slab back face pushed radially away from the camera (perspective)
            float3 deltaBack = deltaFront + normalize(sampleVS) * push.thickness;

            float2 horCos = float2(dot(normalize(deltaFront), V), dot(normalize(deltaBack), V));
            float2 horAng = float2(ACos(horCos.x), ACos(horCos.y));
            float2 hor01 = clamp(horAng * RcpPi + angOff, 0.0, 1.0);
            hor01.x = SliceRelCDF_Cos(hor01.x, angN, cosN, true);
            hor01.y = SliceRelCDF_Cos(hor01.y, angN, cosN, true);
            hor01 = hor01 * w0_remap_mul + w0_remap_add;
            hor01 = clamp(hor01 + rw * (1.0 / 32.0), 0.0, 1.0);

            uint oxFFFFFFFFu = 0xFFFFFFFFu;
            uint2 horInt = uint2(floor(hor01 * float(Slices)));
            uint mX = horInt.x < Slices ? oxFFFFFFFFu << horInt.x : 0u;
            uint mY = horInt.y != 0u ? oxFFFFFFFFu >> (Slices - horInt.y) : 0u;
            occBits |= mX & mY;
        }
        float occ0 = float(CountBits(occBits)) * (1.0 / float(Slices));
        ao += 1.0 - occ0;
    }
    return ao / float(push.sample_count);
}

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.width || id.y >= push.height)
        return;
    int2 pix = int2(id.xy);

    Texture2D   depth_tex = textures[push.depth_texture_id];
    SamplerState smp = samplers[push.sampler_id];
    float2 texel = 1.0 / float2(push.width, push.height);
    float2 uv = (float2(pix) + 0.5) * texel;

    RWTexture2D<float4> out_img = storage_images[push.output_image_id];

    /* Reset mode: no depth read, no history blend. Every pixel becomes a
       neutral (AO=1, count=0) texel so the first real frame after a toggle
       starts accumulation from scratch instead of blending stale history. */
    if (push.mode == 0u) {
        out_img[pix] = float4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    float z = depth_tex.SampleLevel(smp, uv, 0.0).r;
    // reverse-Z: cleared sky is 0.0. Write AO=1, count=0 so it never blends.
    if (z <= 1e-4) {
        out_img[pix] = float4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    float2 ndc = uv * 2.0 - 1.0;
    float3 pos = ViewPos(ndc, z, push.proj_00, push.proj_11, push.near_z);

    // reconstruct a normal from depth derivatives (central differences)
    float zr = depth_tex.SampleLevel(smp, uv + float2(texel.x, 0.0), 0.0).r;
    float zl = depth_tex.SampleLevel(smp, uv - float2(texel.x, 0.0), 0.0).r;
    float zu = depth_tex.SampleLevel(smp, uv + float2(0.0, texel.y), 0.0).r;
    float zd = depth_tex.SampleLevel(smp, uv - float2(0.0, texel.y), 0.0).r;
    float3 pr = ViewPos(float2(ndc.x + 2.0 * texel.x, ndc.y), max(zr, 1e-4), push.proj_00, push.proj_11, push.near_z);
    float3 pl = ViewPos(float2(ndc.x - 2.0 * texel.x, ndc.y), max(zl, 1e-4), push.proj_00, push.proj_11, push.near_z);
    float3 pu = ViewPos(float2(ndc.x, ndc.y + 2.0 * texel.y), max(zu, 1e-4), push.proj_00, push.proj_11, push.near_z);
    float3 pd = ViewPos(float2(ndc.x, ndc.y - 2.0 * texel.y), max(zd, 1e-4), push.proj_00, push.proj_11, push.near_z);
    float3 dx = (abs(pr.z - pos.z) < abs(pl.z - pos.z)) ? (pr - pos) : (pos - pl);
    float3 dy = (abs(pu.z - pos.z) < abs(pd.z - pos.z)) ? (pu - pos) : (pos - pd);
    float3 N = normalize(cross(dx, dy));
    if (dot(N, pos) > 0.0)
        N = -N; // face the camera

    float3 V = -normalize(pos);
    uint frame = global_data.frame_count;
    float ao = uniGTVBAO(pix, pos, N, V, push.proj_00, push.proj_11, frame);
    ao = clamp(pow(saturate(ao), push.intensity), 0.0, 1.0);

    /* Temporal accumulation. Reprojection aligns last frame's history, the
       stored view distance decides whether the texel we land on held the same
       surface, a neighbourhood clamp bounds the survivors, and the blend
       factor rises with reprojection drift so motion does not drag a trail. */
    float4 world    = mul(global_data.inv_view, float4(pos, 1.0));
    float4 prevClip = mul(global_data.prev_viewproj, world);
    float2 prevUv   = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    // w_clip = -view.z for this projection, so the clip w is last frame's distance
    float prevDist = -prevClip.w;

    float accum = ao, outCount = 1.0;
    if (prevClip.w > 0.0 && prevUv.x >= 0.0 && prevUv.x <= 1.0 && prevUv.y >= 0.0 && prevUv.y <= 1.0) {
        float4 h = textures[push.history_texture_id].SampleLevel(smp, prevUv, 0.0);

        /* Every texel stores the distance of the surface it saw. If that differs
           from our distance under last frame's camera by more than the AO radius
           can explain, that texel was a different surface — or empty — and its
           AO belongs to something else. This is what stops a disoccluded pixel
           from inheriting the wall that used to cover it. */
        float tol = min(max(0.08 * prevDist, 0.03), push.radius * 0.5);
        float facing = saturate(dot(N, V));
        if (abs(h.b - prevDist) > tol || abs(h.a - facing) > 0.2)
            h.g = 0.0;

        if (h.g > 0.0) {
            /* Depth-aware neighbourhood clamp: only taps that also sat on our
               surface may widen the range, so a neighbour that still holds
               another surface's AO cannot let a stale value through. */
            float  lo = h.r, hi = h.r;
            float2 ht = 1.0 / float2(push.width, push.height);
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x) {
                    float2 quv = prevUv + float2(x, y) * ht;
                    if (quv.x < 0.0 || quv.x > 1.0 || quv.y < 0.0 || quv.y > 1.0)
                        continue;
                    float4 n = textures[push.history_texture_id].SampleLevel(smp, quv, 0.0);
                    if (n.g <= 0.0 || abs(n.b - prevDist) > tol || abs(n.a - facing) > 0.2)
                        continue;
                    lo = min(lo, n.r);
                    hi = max(hi, n.r);
                }

            float drift  = length((prevUv - uv) * float2(push.width, push.height));
            float alpha  = max(1.0 / min(h.g + 1.0, MaxHistory), saturate(drift * MotionAlphaScale));
            accum        = lerp(clamp(h.r, lo - ClampMargin, hi + ClampMargin), ao, alpha);
            outCount     = 1.0 / alpha; // the weight the accumulated value now carries
        }
    }
    out_img[pix] = float4(accum, outCount, -pos.z, 1.0);
    out_img[pix] = float4(accum, outCount, -pos.z, saturate(dot(N, V)));
}



