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
    SceneGroupShade gs  = pc.group_shade[pc.group_base[base_inst]];
    SceneVsRow      row = pc.vs_rows[base_inst + iid];

    ScenePackedVertex v = gs.vertex_stream[vid];

    float3 local = float3(unpack_half2(v.position_xy), f16tof32(v.position_z & 0xFFFFu));
    float  s     = f16tof32(row.scale_sq & 0xFFFFu);
    float3 ipos  = float3(row.pos[0], row.pos[1], row.pos[2]);
    /* Same hop squash-and-stretch + gel jiggle as scene.slang:vs_main. sy==1
       and amp==0 (the defaults for every non-slime instance) is the identity. */
    float  sy    = f16tof32(row.scale_sq >> 16u);
    float  widen = rsqrt(max(sy, 1e-3f));
    float3 su    = float3(widen, sy, widen);

    SceneVOut o;
    o.face  = row.bloom_face >> 16u;
    float amp = f16tof32(row.st_wg >> 16u);
    o.bloom   = f16tof32(row.bloom_face & 0xFFFFu);

    /* SDF face quad: body-locked decal, same math as scene.slang. */
    if ((pc.materials[gs.material].flags & SCENE_MAT_FACE) != 0u) {
        float3 fq = float3(local.x * 1.05f * s * widen, (local.y * 0.85f + 0.12f) * s * sy, 1.02f * s * widen);
        float3 world = ipos + quat_rotate(row.quat, fq);
        if (amp != 0.0f) {
            float fh  = local.y + 0.5f;
            float fph = global_data.time * 6.0f + float(row.pair) * 2.399963f;
            world += quat_rotate(row.quat, unpack_oct(v.normal_oct)) * (sin(fph) * amp * fh * s);
        }
        o.position = float4(dot(pc.clip_rows[0], float4(world, 1.0f)), dot(pc.clip_rows[1], float4(world, 1.0f)),
                            dot(pc.clip_rows[2], float4(world, 1.0f)), dot(pc.clip_rows[3], float4(world, 1.0f)));
        o.normal   = quat_rotate(row.quat, unpack_oct(v.normal_oct));
        o.uv       = unpack_half2(v.uv);
        o.material = gs.material;
        o.world    = world;
        return o;
    }

    float3 world   = ipos + quat_rotate(row.quat, local * su * s);

    if (amp != 0.0f)
    {
        float h    = local.y * 0.5f + 0.5f;
        float ph   = global_data.time * 6.0f + float(row.pair) * 2.399963f;
        world += quat_rotate(row.quat, unpack_oct(v.normal_oct)) * (sin(ph) * amp * h * s);
    }

    o.position = float4(dot(pc.clip_rows[0], float4(world, 1.0f)), dot(pc.clip_rows[1], float4(world, 1.0f)),
                        dot(pc.clip_rows[2], float4(world, 1.0f)), dot(pc.clip_rows[3], float4(world, 1.0f)));
    float3 nrm = unpack_oct(v.normal_oct);
    o.normal   = normalize(quat_rotate(row.quat, nrm / max(su, 1e-3f)));
    o.uv       = unpack_half2(v.uv);
    o.material = gs.material;
    o.world    = world;
    return o;
}

/* Integer/2.5-exponent helpers. pow() maps to the Medium log2/exp2 path on
   Intel Gen (LUT + two transcendentals, ~30 cycles); repeated squares are
   1-6 cheaper FMAs and the FS runs once per pixel, so the style/gel fresnels
   and spec dots take the multiplier chain. Bit-approx differences vs pow()
   are within cosmetic noise for a stylized look. */
static float pow2(float x) { return x * x; }
static float pow3(float x) { return x * x * x; }
static float pow4(float x) { float x2 = x * x; return x2 * x2; }
static float pow5(float x) { float x2 = x * x; return x2 * x2 * x; }
static float pow24(float x) { float x2 = x * x; float x4 = x2 * x2; float x8 = x4 * x4; return x8 * x8 * x8; }
static float pow32(float x) { float x2 = x * x; float x4 = x2 * x2; float x8 = x4 * x4; float x16 = x8 * x8; return x16 * x16; }
static float pow48(float x) { float x2 = x * x; float x4 = x2 * x2; float x8 = x4 * x4; float x16 = x8 * x8; return x16 * x16 * x16; }
static float pow2_5(float x) { float x2 = x * x; return x2 * sqrt(x); }

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

    uint   style = global_data.toon_style;
    float3 v     = normalize(pc.camera.xyz - i.world);
    float3 l     = env_sun_dir();
    float  ndl   = saturate(dot(n, l));
    float  ndv   = saturate(dot(n, v));
    float3 h     = normalize(l + v);
    float  ndh   = saturate(dot(n, h));

    float  metallic  = saturate(m.metallic);
    float  roughness = clamp(m.roughness, 0.045f, 1.0f);
    float3 f0        = lerp(float3(0.04f), albedo, metallic);
    float3 kd        = (1.0f - f0) * (1.0f - metallic);
    float3 sun       = env_sun_radiance();
    float3 ambient   = kd * albedo * env_diffuse_irradiance(n) * ao;

    /* Three switchable cel looks (GlobalData.toon_style, set from the UI). Each
       picks its own band count and edge width, its shadow tint, its specular
       character and its rim; the gel tint, sun and ambient are shared. A uniform
       branch, so no divergence. */
    float3 diffuse;
    float3 specular;
    float3 rim;
    float  outline = 0.0f;

    if (style == 0u) {
        /* Anime cel: two hard tonal steps, a cool shadow, a crisp specular dot
           and a dark silhouette outline. */
        float  f    = ndl * 2.0f;
        float  ramp = (floor(f) + smoothstep(0.47f, 0.53f, f - floor(f))) * 0.5f;
        float3 shadow = float3(0.58f, 0.66f, 0.86f);
        diffuse   = kd * albedo * ramp * lerp(float3(1.0f), shadow, 1.0f - ramp);
        float  sp = pow48(ndh);
        specular  = f0 * smoothstep(0.35f, 0.42f, sp) * (1.0f - roughness * 0.7f);
        rim       = albedo * pow4(1.0f - ndv) * 0.18f * (0.4f + 0.6f * ramp);
        outline   = pow5(1.0f - ndv) * 0.45f;
    } else if (style == 1u) {
        /* Soft stylized: four gently smoothed steps, a neutral-warm shadow, a
           modest soft specular and a breath of sky rim. */
        float  f    = ndl * 4.0f;
        float  ramp = (floor(f) + smoothstep(0.25f, 0.75f, f - floor(f))) * 0.25f;
        float3 shadow = float3(1.0f, 0.97f, 0.93f);
        diffuse   = kd * albedo * ramp * lerp(float3(1.0f), shadow, 1.0f - ramp);
        float  sp = pow32(ndh);
        specular  = f0 * smoothstep(0.28f, 0.55f, sp) * 0.6f;
        rim       = env_sky_color() * pow2_5(1.0f - ndv) * (0.4f + 0.4f * ramp) * 0.12f;
    } else {
        /* Painterly: two wide soft steps, a warm shadow, a strong sky-to-ground
           ambient gradient and almost no specular — an illustrated look. */
        float  f    = ndl * 2.0f;
        float  ramp = (floor(f) + smoothstep(0.12f, 0.88f, f - floor(f))) * 0.5f;
        float3 shadow = float3(0.86f, 0.76f, 0.60f);
        diffuse   = kd * albedo * ramp * lerp(float3(1.0f), shadow, 1.0f - ramp);
        specular  = f0 * pow24(ndh) * 0.25f;
        float3 warm = lerp(env_sky_color(), env_sun_radiance(), 0.5f);
        rim       = warm * pow2(1.0f - ndv) * (0.5f + 0.5f * ramp) * 0.16f;
    }

    /* Gel slimes keep their wet albedo-tinted fresnel on top of any style. */
    if ((m.flags & SCENE_MAT_GEL) != 0u)
        rim += albedo * pow3(1.0f - ndv) * 0.25f;

     /* The band is an irradiance multiplier, so keep the direct term on the same
         Lambert 1/pi HDR scale as the PBR path instead of adding white light. */
    float3 col = diffuse * LIGHT_INV_PI * sun + ambient + specular * sun * ao + rim;

    /* The anime ink darkens the silhouette rather than adding light, so folds
       and overlapping props read as drawn. Other styles leave it at zero. */
    col *= 1.0f - outline;

    /* Linear HDR: exposure, bloom and the tonemap all happen in post. Alpha
       carries the per-instance bloom strength into the bloom bright-pass. */
    return float4(col, i.bloom);
}
