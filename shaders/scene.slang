/* scene.slang — T5: the vertex and fragment stages of the GPU-driven draw.
   Indexed indirect draw + vertex pull; no mesh shaders.
   The cull kernel lives in cull.slang because the two stages want different
   push constants and one module cannot declare two. */
#include "common.slang"
#include "lighting.slang"
#include "../src/three_d/scene_shared.h"

[[vk::push_constant]] SceneDrawPush pc;

/* textures[] and samplers[] come from common.slang: the material already holds
   the bindless descriptor slot, so there is no per-draw binding work. Sampler 0 is SAMPLER_LINEAR_WRAP. The material could name its own, but every
   albedo in the format wants the same one, and a per-material sampler choice is
   a second uniform to fetch for no difference today. */
#define SCENE_SAMPLER 0

/* ---- quaternion (10-10-10-2) ---- */
static float3 quat_rotate(SceneU32 q, float3 v)
{
    int3   i  = int3((int)(q << 22) >> 22, (int)(q << 12) >> 22, (int)(q << 2) >> 22);
    float3 qv = float3(i) * (1.0f / 511.0f);
    float  qw = sqrt(max(0.0f, 1.0f - dot(qv, qv)));
    qw       = (q & (1u << 30)) != 0u ? -qw : qw; /* sign bit is data-dependent */
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
    return normalize(n);}

struct SceneVOut {
    float4               position : SV_Position;
    float3               normal   : TEXCOORD0;
    float2               uv       : TEXCOORD1;
    nointerpolation uint material : TEXCOORD2;
    float3               world    : TEXCOORD3; /* the eye vector needs the world point, not the clip one */
    nointerpolation float bloom  : TEXCOORD4; /* per-instance bloom strength, 0 = excluded */
    nointerpolation uint face    : TEXCOORD5; /* slime face atlas tile; only FACE quads read it */
};

[shader("vertex")]
SceneVOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID, uint base_inst : SV_StartInstanceLocation)
{
    /* Identity split, and which half each comes from is the whole point:
          - the group is uniform across the draw, so it comes from one row
            addressed by the draw's own instance base (group_base[base_instance])
            and read once, then broadcast by the hardware;
          - that one row carries the rung's vertex stream and its material, so
            mesh, stream and material are a single dependent load rather than the
            chain group_base -> group_mesh -> shade_meshes -> vertex_stream;
          - the instance datum is one consolidated row per survivor, written by
            cs_scatter once per frame: vs_rows[base_inst + iid] is the single
            dependent per-lane load, replacing the old vis -> slot -> seven
            random per-vertex table loads. */
    SceneGroupShade gs  = pc.group_shade[pc.group_base[base_inst]];
    SceneVsRow      row = pc.vs_rows[base_inst + iid];

    ScenePackedVertex v = gs.vertex_stream[vid];

    float3 local = float3(unpack_half2(v.position_xy), f16tof32(v.position_z & 0xFFFFu));
    float  s     = f16tof32(row.scale_sq & 0xFFFFu);
    float3 ipos  = float3(row.pos[0], row.pos[1], row.pos[2]);
    /* Hop squash-and-stretch, volume-preserving and branch-free. sy scales the
       local height about the instance centre; widen = 1/sqrt(sy) keeps the
       volume constant for small changes. sy==1 and widen==1 is the identity,
       so every non-slime instance (the default) is bit-identical to the rigid
       path. */
    float  sy      = f16tof32(row.scale_sq >> 16u);
    float  sz      = f16tof32(row.st_wg & 0xFFFFu);
    float  widen   = rsqrt(max(sy, 1e-3f));
    /* Forward stretch along local +Z (the facing the yaw already points at),
       traded against x so volume holds: widen/rsqrt(sz) * widen*sqrt(sz) leaves
       the horizontal area at widen^2 under any sz. sz==1 is the identity. */
    float  ax      = widen * rsqrt(max(sz, 1e-3f));
    float  az      = widen * sqrt(max(sz, 1e-3f));
    float3 su      = float3(ax, sy, az);

    SceneVOut o;

    /* Bloom strength and the face tile are packed into one word; the jiggle
       amplitude rides the other word. amp==0 multiplies to a zero offset, so
       every non-slime instance (the default) is bit-identical to the rigid
       path with no branch. */
    float amp = f16tof32(row.st_wg >> 16u);
    o.bloom   = f16tof32(row.bloom_face & 0xFFFFu);

    o.face = row.bloom_face >> 16u;

    /* SDF face quad: rigidly attached to the body's front (+Z in instance
       space) so ears, jiggle and face share one transform — a billboard would
       slide against the yawed body. The quad inherits squash/widen and the
       gel jiggle below via the shared slot-pair phase, and sits 2% proud of
       the unit body radius so the GREATER depth test keeps it. From behind
       you see plain body, like SR's decal faces. Group-uniform branch. */
    if ((pc.materials[gs.material].flags & SCENE_MAT_FACE) != 0u) {
        float3 fq = float3(local.x * 1.05f * s * ax, (local.y * 0.85f + 0.12f) * s * sy, 1.02f * s * az);
        float3 world = ipos + quat_rotate(row.quat, fq);
        if (amp != 0.0f) {
            float h  = local.y + 0.5f;
            float ph = global_data.time * 6.0f + float(row.pair) * 2.399963f;
            world += quat_rotate(row.quat, unpack_oct(v.normal_oct)) * (sin(ph) * amp * h);
        }
        o.position = float4(dot(pc.clip_rows[0], float4(world, 1.0f)), dot(pc.clip_rows[1], float4(world, 1.0f)),
                            dot(pc.clip_rows[2], float4(world, 1.0f)), dot(pc.clip_rows[3], float4(world, 1.0f)));
        o.normal   = quat_rotate(row.quat, unpack_oct(v.normal_oct)); /* unused: face path is unlit */
        o.uv       = unpack_half2(v.uv);
        o.material = gs.material;
        o.world    = world;
        return o;
    }

    float3 squishy = local * su;
    float3 world   = ipos + quat_rotate(row.quat, squishy * s);

    /* Gel jiggle: displace along the local normal by a per-instance amplitude,
       phase-shifted per slot so a crowd of slimes does not wobble in lockstep.
       The height factor keeps the base planted and wobbles the crown most, which
       is what reads as gel rather than a rigid translate. */
    if (amp != 0.0f) {
        float h  = local.y * 0.5f + 0.5f;
        /* Phase from the instance *pair* index, not the raw slot: a slime is
           two slots (body, face quad) sharing one transform, and they must
           jiggle in lockstep or the face slides off the body. Different pairs
           still land on unrelated phases. */
        float ph = global_data.time * 6.0f + float(row.pair) * 2.399963f;
        world += quat_rotate(row.quat, unpack_oct(v.normal_oct)) * (sin(ph) * amp * h);
    }

    if (pc.counts[5] != 0u && iid == 0u) /* DEBUG: one line per draw, MU_SHADER_DEBUG */
        printf("[VS] base=%u iid=%u pair=%u pos=(%.4f, %.4f, %.4f)\n", base_inst, iid, row.pair, world.x, world.y,
               world.z);

    o.position = float4(dot(pc.clip_rows[0], float4(world, 1.0f)), dot(pc.clip_rows[1], float4(world, 1.0f)),
                        dot(pc.clip_rows[2], float4(world, 1.0f)), dot(pc.clip_rows[3], float4(world, 1.0f)));
    /* Inverse squash for the normal: the surface was scaled by (widen, sy,
       widen), so the geometric normal divides by the same factors. Same
       identity argument as the position path. */
    float3 nrm = unpack_oct(v.normal_oct);
    o.normal   = normalize(quat_rotate(row.quat, nrm / max(su, 1e-3f)));
    o.uv       = unpack_half2(v.uv);
    o.material = gs.material;
    o.world    = world;
    return o;
}

/* Cook-Torrance GGX against one directional light. Metalness and roughness come
   from the material row, not from a constant, so a model that varies them looks
   like itself rather than like every other model. */
/* pow() maps to the log2/exp2 path on Intel Gen; the FS fresnels use small
   integer exponents that cost 1-3 FMAs as repeated squares instead. */
static float pow2(float x) { return x * x; }
static float pow3(float x) { return x * x * x; }
static float pow5(float x) { float x2 = x * x; return x2 * x2 * x; }

static float3 fresnel_schlick(float3 f0, float vdh)
{
    return f0 + (1.0f - f0) * pow5(1.0f - vdh);
}

static float distribution_ggx(float ndh, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = ndh * ndh * (a2 - 1.0f) + 1.0f;
    return a2 / max(3.14159265f * d * d, 1e-7f);
}

static float geometry_smith(float ndv, float ndl, float roughness)
{
    float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
    float gv = ndv / (ndv * (1.0f - k) + k);
    float gl = ndl / (ndl * (1.0f - k) + k);
    return gv * gl;
}

[shader("fragment")]
float4 fs_main(SceneVOut i) : SV_Target0
{
    SceneGpuMaterial m = pc.materials[i.material];

    /* glTF's baseColorFactor is a linear multiplier, and the base color texture
       is uploaded as an sRGB VkFormat so the sampler decodes it. Neither needs a
       gamma conversion here. */
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
#ifndef SCENE_NO_DISCARD
    if ((m.flags & SCENE_MAT_ALPHA_TEST) != 0u && alpha < 0.5f)
        discard;
#endif

    /* SDF slime face: one atlas tile per expression (R = eyes, A = mouth,
       B = eye whites, G = eye highlights, 0.5 = shape edge). Edge width comes
       from screen-space derivatives so faces stay crisp at any distance — the
       whole point of SDF faces. Unlit: expressions must read in shadow.
       SCENE_NO_DISCARD (early-Z route) skips this branch entirely: FACE
       materials are routed to the late-Z (alpha) pipeline, because the
       discard inside would force the whole early route into late-Z. */
#ifndef SCENE_NO_DISCARD
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
        float3 dark = float3(0.05f, 0.055f, 0.09f); /* the old geometry navy */
        /* G is a sheen ramp across the eye, not a glint dot: shade the pupil
           by it and add a hot core where it peaks. Mouths stay flat dark. */
        float3 eye_col = dark * (0.25f + 0.75f * sdf.g) + float3(1.0f) * (smoothstep(0.75f, 0.95f, sdf.g) * 0.8f);
        float3 col     = float3(1.0f) * white;          /* hunter-style eye whites */
        col            = lerp(col, eye_col, clamp(eye, 0.0f, 1.0f));
        col            = lerp(col, dark, clamp(mouth, 0.0f, 1.0f));
        /* Outline ring just outside the eye/mouth edge, like SR's mouth
           outlines: one band, no extra textures. */
        float ring = (smoothstep(0.5f - 3.5f * w, 0.5f - w, sdf.r) - eye)
                   + (smoothstep(0.5f - 3.5f * w, 0.5f - w, sdf.a) - mouth);
        col = lerp(col, dark * 0.45f, clamp(ring, 0.0f, 1.0f));
        return float4(col, i.bloom);
    }
#endif /* SCENE_NO_DISCARD */

    if ((m.flags & SCENE_MAT_UNLIT) != 0u)
        return float4(albedo, i.bloom);

    float3 n = normalize(i.normal);
    float  ao = 1.0f;

    /* Screen-space tangent frame from the position and UV gradients. The packed
       vertex row is 16 bytes and carries no tangent, so this is the only frame
       available; it is exact on a flat triangle and only drifts where the UV
       unwrap bends sharply across a triangle. */
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
    float3 v = normalize(pc.camera.xyz - i.world);
    float3 l = env_sun_dir();

    float ndl = max(dot(n, l), 0.0f);
    float ndv = max(dot(n, v), 1e-4f);
    float3 h  = normalize(l + v);
    float ndh = max(dot(n, h), 0.0f);
    float vdh = max(dot(v, h), 0.0f);

    float  metallic  = clamp(m.metallic, 0.0f, 1.0f);
    float  roughness = clamp(m.roughness, 0.045f, 1.0f);
    float3 f0       = lerp(float3(0.04f), albedo, metallic);
    float3 f        = fresnel_schlick(f0, vdh);
    float  d        = distribution_ggx(ndh, roughness);
    float  g        = geometry_smith(ndv, ndl, roughness);
    float3 spec     = f * (d * g / max(4.0f * ndv * ndl, 1e-6f));
    float3 kd       = (1.0f - f) * (1.0f - metallic);

    /* The direction is toward the light, so there is nothing to negate here;
       flipping it lights the far side of every surface. */
    float3 diffuse = kd * albedo * (1.0f / 3.14159265f);
    float3 direct  = (diffuse + spec) * ndl * env_sun_radiance();

    /* Ambient as a split-sum environment, done analytically so there is no
       prefiltered cubemap and no BRDF LUT: the diffuse half is the hemispheric
       irradiance, the specular half is the analytic EnvBRDF fit times an
       approximate reflection color, occluded toward creases. This is what keeps
       metal from being black (its kd is ~0) and rough dielectrics from getting a
       free full-intensity specular. */
    float2 ab      = env_brdf_approx(roughness, ndv);
    float3 fss_ess = f0 * ab.x + ab.y;                       /* single-scatter specular */
    float  ess     = ab.x + ab.y;
    float3 f_avg   = f0 + (1.0f - f0) / 21.0f;               /* average Fresnel */
    /* Energy lost to multiple bounces inside a rough microsurface comes back:
       without this, roughness visibly darkens metal. */
    float3 f_ms    = fss_ess * f_avg / max(1.0f - f_avg * (1.0f - ess), 1e-4f);
    float  spec_ao = env_specular_occlusion(ao, ndv, roughness);

    float3 env_diff = env_diffuse_irradiance(n) * ao;
    float3 env_spec = env_reflection_color(reflect(-v, n), roughness);
    float3 ambient  = (1.0f - fss_ess) * (1.0f - metallic) * albedo * env_diff
                    + (fss_ess + f_ms) * env_spec * spec_ao;

    /* Gel rim: a fresnel edge tinted by the slime's own albedo, so the silhouette
       glows like wet gel. Gated on the material flag so ordinary props are
       untouched. */
    if ((m.flags & SCENE_MAT_GEL) != 0u) {
        float rim = pow3(1.0f - saturate(ndv));
        direct += albedo * rim * 0.8f;
    }

    /* Linear HDR. Tonemapping and the output gamma live in the post pass now,
       so exposure, bloom and color grading all see the same linear values
       instead of grading an already-encoded image. Alpha carries the
       per-instance bloom strength into the bloom bright-pass. */
    return float4(direct + ambient, i.bloom);
}
