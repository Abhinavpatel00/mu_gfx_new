/* toon.slang — a cel-shaded variant of the scene draw. Standalone on purpose:
   scene.slang is the PBR path and stays untouched, and the renderer picks
   between the two pipelines at runtime. The vertex stage is identical; only the
   fragment stage differs.

   Output is linear HDR, like the PBR path: tonemap, exposure and bloom are the
   post pass's job, so both pipelines feed the same grade. */
#include "common.slang"
#include "lighting.slang"
#include "../src/three_d/scene_shared.h"

[[vk::push_constant]] SceneDrawPush pc;

#define SCENE_SAMPLER 0
#define TOON_BANDS    3.0

static float3 quat_rotate(SceneU32 q, float3 v)
{
    int3   i  = int3((int)(q << 22) >> 22, (int)(q << 12) >> 22, (int)(q << 2) >> 22);
    float3 qv = float3(i) * (1.0f / 511.0f);
    float  qw = sqrt(max(0.0f, 1.0f - dot(qv, qv)));
    qw       = (q & (1u << 30)) != 0u ? -qw : qw;
    float3 t = 2.0f * cross(qv, v);
    return v + qw * t + cross(qv, t);
}

static float2 unpack_half2(SceneU32 xy) { return float2(f16tof32(xy & 0xFFFFu), f16tof32(xy >> 16)); }

static float3 unpack_oct(SceneU32 o)
{
    float2 e = max(float2((int)(o << 16) >> 16, (int)o >> 16) * (1.0f / 32767.0f), -1.0f);
    float3 n = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
    float  t = max(-n.z, 0.0f);
    n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);
    return normalize(n);
}

struct SceneVOut {
    float4               position : SV_Position;
    float3               normal   : TEXCOORD0;
    float2               uv       : TEXCOORD1;
    nointerpolation uint material : TEXCOORD2;
    float3               world    : TEXCOORD3;
    nointerpolation float bloom  : TEXCOORD4;
    nointerpolation uint face    : TEXCOORD5; /* slime face atlas tile; only FACE quads read it */
};

[shader("vertex")]
SceneVOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID, uint base_inst : SV_StartInstanceLocation)
{
    SceneGroupShade gs   = pc.group_shade[pc.group_base[base_inst]];
    SceneU32        slot = pc.vis[base_inst + iid];

    SceneInstance     inst = pc.instances[slot];
    ScenePackedVertex v    = gs.vertex_stream[vid];

    float3 local = float3(unpack_half2(v.position_xy), f16tof32(v.position_z & 0xFFFFu));
    float  s     = f16tof32(inst.scale);
    float3 ipos  = float3(inst.pos[0], inst.pos[1], inst.pos[2]);
    /* Same hop squash-and-stretch + gel jiggle as scene.slang:vs_main. sy==1
       and amp==0 (the defaults for every non-slime instance) is the identity. */
    float  sy      = pc.squash[slot];
    float  widen   = rsqrt(max(sy, 1e-3f));
    float3 su      = float3(widen, sy, widen);

    SceneVOut o;
    o.face = pc.face_id[slot];

    /* SDF face quad: body-locked decal, same math as scene.slang. */
    if ((pc.materials[gs.material].flags & SCENE_MAT_FACE) != 0u) {
        float3 fq = float3(local.x * 1.05f * s * widen, (local.y * 0.85f + 0.12f) * s * sy, 1.02f * s * widen);
        float3 world = ipos + quat_rotate(inst.quat, fq);
        float famp = pc.wiggle[slot];
        if (famp != 0.0f) {
            float fh  = local.y + 0.5f;
            float fph = global_data.time * 6.0f + float(slot / 2u) * 2.399963f;
            world += quat_rotate(inst.quat, unpack_oct(v.normal_oct)) * (sin(fph) * famp * fh * s);
        }
        o.position = float4(dot(pc.clip_rows[0], float4(world, 1.0f)), dot(pc.clip_rows[1], float4(world, 1.0f)),
                            dot(pc.clip_rows[2], float4(world, 1.0f)), dot(pc.clip_rows[3], float4(world, 1.0f)));
        o.normal   = quat_rotate(inst.quat, unpack_oct(v.normal_oct));
        o.uv       = unpack_half2(v.uv);
        o.material = gs.material;
        o.world    = world;
        o.bloom    = pc.bloom[slot];
        return o;
    }

    float3 world   = ipos + quat_rotate(inst.quat, local * su * s);

    float amp = pc.wiggle[slot];
    if (amp != 0.0f)
    {
        float h    = local.y * 0.5f + 0.5f;
        float ph   = global_data.time * 6.0f + float(slot / 2u) * 2.399963f;
        world += quat_rotate(inst.quat, unpack_oct(v.normal_oct)) * (sin(ph) * amp * h * s);
    }

    o.position = float4(dot(pc.clip_rows[0], float4(world, 1.0f)), dot(pc.clip_rows[1], float4(world, 1.0f)),
                        dot(pc.clip_rows[2], float4(world, 1.0f)), dot(pc.clip_rows[3], float4(world, 1.0f)));
    float3 nrm = unpack_oct(v.normal_oct);
    o.normal   = normalize(quat_rotate(inst.quat, nrm / max(su, 1e-3f)));
    o.uv       = unpack_half2(v.uv);
    o.material = gs.material;
    o.world    = world;
    o.bloom    = pc.bloom[slot];
    return o;
}

[shader("fragment")]
float4 fs_main(SceneVOut i) : SV_Target0
{
    SceneGpuMaterial m = pc.materials[i.material];

    float4 base = float4(m.base_color & 255u, (m.base_color >> 8) & 255u, (m.base_color >> 16) & 255u,
                         (m.base_color >> 24) & 255u) * (1.0f / 255.0f);
    float3 albedo = base.rgb;
    float alpha = base.a;
    if ((m.flags & SCENE_MAT_HAS_ALBEDO) != 0u)
    {
        float4 texel = textures[NonUniformResourceIndex(m.texture)].Sample(samplers[SCENE_SAMPLER], i.uv);
        albedo *= texel.rgb;
        alpha *= texel.a;
    }
    if ((m.flags & SCENE_MAT_ALPHA_TEST) != 0u && alpha < 0.5f)
        discard;

    /* SDF slime face: same atlas resolve as scene.slang, unlit. */
    if ((m.flags & SCENE_MAT_FACE) != 0u) {
        uint   tile    = i.face % (SCENE_FACE_COLS * SCENE_FACE_ROWS);
        float2 tile_xy = float2(tile % SCENE_FACE_COLS, tile / SCENE_FACE_COLS);
        float2 fuv     = (i.uv + tile_xy) * float2(1.0f / float(SCENE_FACE_COLS), 1.0f / float(SCENE_FACE_ROWS));
        float4 sdf     = textures[NonUniformResourceIndex(m.texture)].Sample(samplers[SCENE_SAMPLER], fuv);
        float  w       = max(max(fwidth(sdf.r), fwidth(sdf.a)), 1e-4f);
        float  eye     = smoothstep(0.5f - w, 0.5f + w, sdf.r);
        float  mouth   = smoothstep(0.5f - w, 0.5f + w, sdf.a);
        float  white   = smoothstep(0.5f - w, 0.5f + w, sdf.b);
        if (max(max(eye, mouth), white) < 0.02f)
            discard;
        float3 dark = float3(0.05f, 0.055f, 0.09f);
        float3 eye_col = dark * (0.25f + 0.75f * sdf.g) + float3(1.0f) * (smoothstep(0.75f, 0.95f, sdf.g) * 0.8f);
        float3 col     = float3(1.0f) * white;
        col            = lerp(col, eye_col, clamp(eye, 0.0f, 1.0f));
        col            = lerp(col, dark, clamp(mouth, 0.0f, 1.0f));
        float ring = (smoothstep(0.5f - 3.5f * w, 0.5f - w, sdf.r) - eye)
                   + (smoothstep(0.5f - 3.5f * w, 0.5f - w, sdf.a) - mouth);
        col = lerp(col, dark * 0.45f, clamp(ring, 0.0f, 1.0f));
        return float4(col, i.bloom);
    }

    float3 n = normalize(i.normal);
    float  ao = 1.0f;

    if ((m.flags & SCENE_MAT_HAS_NORMAL) != 0u) {
        float3 map     = textures[NonUniformResourceIndex(m.normal_texture)].Sample(samplers[SCENE_SAMPLER], i.uv).xyz * 2.0f - 1.0f;
        float3 dpdx    = ddx(i.world), dpdy = ddy(i.world);
        float2 duvdx   = ddx(i.uv), duvdy = ddy(i.uv);
        float3 tangent = normalize(dpdx * duvdy.y - dpdy * duvdx.y);
        float3 bitangent = normalize(-dpdx * duvdy.x + dpdy * duvdx.x);
        n = normalize(map.x * tangent + map.y * bitangent + map.z * normalize(cross(tangent, bitangent)));
    }

    if ((m.flags & SCENE_MAT_HAS_ORM) != 0u) {
        float3 orm = textures[NonUniformResourceIndex(m.orm_texture)].Sample(samplers[SCENE_SAMPLER], i.uv).rgb;
        ao = orm.r;
        m.metallic = orm.b;
        m.roughness = orm.g;
    }

    if ((m.flags & SCENE_MAT_UNLIT) != 0u)
        return float4(albedo, i.bloom);

    float3 v = normalize(pc.camera.xyz - i.world);
    float3 l = env_sun_dir();

    /* Quantized diffuse ramp. The half-band is smoothed with smoothstep so the
       edge does not alias into a hard staircase as the surface turns. */
    float  lit  = saturate(dot(n, l));
    float  f    = lit * TOON_BANDS;
    float  lo   = floor(f);
    float  ramp = (lo + smoothstep(0.40f, 0.60f, f - lo)) / TOON_BANDS;

    /* Rim wraps toward the lit side so it reads as a bounce edge rather than a
       uniform outline, and it takes the sky's color for the same reason the
       ambient does: it is the sky leaning on the silhouette. */
    float  ndv  = saturate(dot(n, v));
    float  rim  = pow(1.0f - ndv, 2.5f) * (0.55f + 0.45f * lit);

    float3 h    = normalize(l + v);
    float  spec = pow(saturate(dot(n, h)), 64.0f);
    spec        = smoothstep(0.30f, 0.55f, spec);

    float  metallic  = saturate(m.metallic);
    float3 f0        = lerp(float3(0.04f), albedo, metallic);
    float3 kd        = (1.0f - f0) * (1.0f - metallic);

    float3 sun      = env_sun_radiance();
    float3 ambient  = kd * albedo * env_ambient(n, ao);
    float3 diffuse  = kd * albedo * ramp * sun;
    float3 specular = f0 * spec * sun * ao;
    /* Sky-coloured, but scaled to an accent: at 1.4 the rim covered every
       grazing leaf in sky blue and desaturated the whole canopy. */
    float3 rim_light = env_sky_color() * rim * 0.5f;

    /* Gel slimes get a stronger, albedo-tinted fresnel rim on top of the shared
       sky rim, so they read as wet while ordinary props keep the base look. */
    if ((m.flags & SCENE_MAT_GEL) != 0u)
        rim_light += albedo * pow(1.0f - ndv, 3.0f) * 0.8f;

    /* Linear HDR: exposure, bloom and the tonemap all happen in post. Alpha
       carries the per-instance bloom strength into the bloom bright-pass. */
    return float4(diffuse + ambient + specular + rim_light, i.bloom);
}
