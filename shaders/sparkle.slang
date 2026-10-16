/* sparkle.slang — the transparent pass that runs after the opaque scene draw.
   Sparkles are procedural: no particle buffer, no CPU emission. Instance iid
   picks a slime (iid % slime_count), reads that slime's body row for its
   position, and derives an orbit, a twinkle phase and a hue from a hash of iid.
   The seed is the memory — the same iid always yields the same sparkle, so the
   crowd twinkles deterministically without a single CPU write.

   Drawn as additive camera-facing quads, depth-tested against the opaque depth
   but not writing it, so sparkles sit behind trunks correctly and never occlude
   each other in the depth buffer. */
#include "common.slang"
#include "../src/three_d/scene_shared.h"

[[vk::push_constant]] SceneSparklePush pc;

static uint hash_u32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static float hash_f(uint x, uint salt) { return float(hash_u32(x ^ salt) >> 8) * (1.0f / 16777216.0f); }

struct SparkleVOut {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float3 color : TEXCOORD1;
    float  alpha : TEXCOORD2;
};

[shader("vertex")]
SparkleVOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    /* Two triangles, camera-facing, centred on origin in the billboard plane.
       Corner per vertex: 0,3 = BL, 1 = BR, 2,4 = TR, 5 = TL. */
    float2 corner;
    if (vid == 0u || vid == 3u)
        corner = float2(-1.0f, -1.0f);
    else if (vid == 1u)
        corner = float2(1.0f, -1.0f);
    else if (vid == 2u || vid == 4u)
        corner = float2(1.0f, 1.0f);
    else
        corner = float2(-1.0f, 1.0f);

    uint  slime_count = max(pc.counts[1], 1u);
    uint  per         = max(pc.counts[2], 1u);
    /* Cluster the sparkles of one slime onto consecutive instance ids so the
       four invocations share a single body-row read (locality), and the hash
       salt still differs per sparkle via the low bits of iid. */
    uint  j           = min(iid / per, slime_count - 1u);
    SceneInstance body = pc.instances[j * 2u];
    float scale = f16tof32(body.scale);
    float3 base = float3(body.pos[0], body.pos[1], body.pos[2]);

    float t  = global_data.time;
    float p1 = hash_f(iid, 0x9E3779B9u) * 6.2831853f;
    float p2 = hash_f(iid, 0x85EBCA6Bu) * 6.2831853f;
    float p3 = hash_f(iid, 0xC2B2AE35u);

    /* Slow orbit above the body, bobbing on a second phase so a cluster does
       not pulse in unison. */
    float ang   = t * (0.7f + p3 * 0.9f) + p1;
    float orbit = scale * (0.18f + 0.22f * p3);
    float rise  = scale * (0.55f + 0.35f * (0.5f + 0.5f * sin(t * 1.3f + p2)));
    float3 center = base + float3(cos(ang) * orbit, rise, sin(ang) * orbit);

    /* Camera basis from the clip rows: row 0 ∝ right, row 1 ∝ -up (the
       projection scales them, but normalizing recovers unit axes). */
    float3 right = normalize(pc.clip_rows[0].xyz);
    float3 up    = normalize(-pc.clip_rows[1].xyz);

    float twinkle = 0.5f + 0.5f * sin(t * (5.0f + p3 * 4.0f) + p1);
    float size    = scale * 0.13f * (0.45f + 0.55f * twinkle);

    float3 world = center + (right * corner.x + up * corner.y) * size;

    SparkleVOut o;
    o.pos = float4(dot(pc.clip_rows[0], float4(world, 1.0f)), dot(pc.clip_rows[1], float4(world, 1.0f)),
                   dot(pc.clip_rows[2], float4(world, 1.0f)), dot(pc.clip_rows[3], float4(world, 1.0f)));
    o.uv = corner * 0.5f + 0.5f;

    /* Warm gold to cool cyan, so a crowd reads as a scatter of tiny lights
       rather than one colour. */
    float hue = hash_f(iid, 0x27D4EB2Fu);
    o.color   = lerp(float3(1.0f, 0.85f, 0.35f), float3(0.55f, 0.9f, 1.0f), hue);
    o.alpha   = (0.35f + 0.65f * twinkle) * (0.6f + 0.4f * hue);
    return o;
}

[shader("fragment")]
float4 fs_main(SparkleVOut i) : SV_Target0
{
    /* Soft round falloff with a hot core; premultiplied by alpha for the
       additive (ONE, ONE) blend. */
    float2 d = i.uv * 2.0f - 1.0f;
    float r2 = dot(d, d);
    if (r2 > 1.0f)
        discard;
    float glow = pow(saturate(1.0f - r2), 1.6f);
    float core = pow(saturate(1.0f - r2), 6.0f);
    float a    = i.alpha * (glow * 0.8f + core * 0.9f);
    return float4(i.color * a * 1.6f, a);
}
