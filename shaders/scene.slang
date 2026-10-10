/* scene.slang — T5: the vertex and fragment stages of the GPU-driven draw.
   Indexed indirect draw + vertex pull; no mesh shaders.
   The cull kernel lives in cull.slang because the two stages want different
   push constants and one module cannot declare two. */
#include "common.slang"
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
    return normalize(n);
}

struct SceneVOut {
    float4               position : SV_Position;
    float3               normal   : TEXCOORD0;
    float2               uv       : TEXCOORD1;
    nointerpolation uint material : TEXCOORD2;
    float3               world    : TEXCOORD3; /* the eye vector needs the world point, not the clip one */
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
         - the instance slot is the only genuinely per-lane datum, so it is the
           only thing read per-lane, out of vis[] which holds a plain slot.
       vis[] therefore needs no packed key and no decode. */
    SceneGroupShade gs   = pc.group_shade[pc.group_base[base_inst]];
    SceneU32        slot = pc.vis[base_inst + iid];

    /* iid is Slang SV_InstanceID == InstanceIndex - BaseInstance: draw-local.
       vis base is base_inst == firstInstance. */
    SceneInstance     inst = pc.instances[slot];
    ScenePackedVertex v    = gs.vertex_stream[vid];

    float3 local = float3(unpack_half2(v.position_xy), f16tof32(v.position_z & 0xFFFFu));
    float  s     = f16tof32(inst.scale);
    float3 ipos  = float3(inst.pos[0], inst.pos[1], inst.pos[2]);
    float3 world = ipos + quat_rotate(inst.quat, local * s);

    if (pc.counts[5] != 0u && iid == 0u) /* DEBUG: one line per draw */
        printf("[VS] base=%u iid=%u slot=%u pos=(%.4f, %.4f, %.4f)\n", base_inst, iid, slot, world.x, world.y, world.z);

    SceneVOut o;
    o.position = float4(dot(pc.clip_rows[0], float4(world, 1.0f)), dot(pc.clip_rows[1], float4(world, 1.0f)),
                        dot(pc.clip_rows[2], float4(world, 1.0f)), dot(pc.clip_rows[3], float4(world, 1.0f)));
    o.normal   = normalize(quat_rotate(inst.quat, unpack_oct(v.normal_oct)));
    o.uv       = unpack_half2(v.uv);
    o.material = gs.material;
    o.world    = world;
    return o;
}

/* Cook-Torrance GGX against one directional light. Metalness and roughness come
   from the material row, not from a constant, so a model that varies them looks
   like itself rather than like every other model. */
static float3 fresnel_schlick(float3 f0, float vdh)
{
    return f0 + (1.0f - f0) * pow(1.0f - vdh, 5.0f);
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
    float3 albedo = float3(m.base_color & 255u, (m.base_color >> 8) & 255u, (m.base_color >> 16) & 255u) *
                    (1.0f / 255.0f);
    if ((m.flags & SCENE_MAT_HAS_ALBEDO) != 0u)
        albedo *= textures[NonUniformResourceIndex(m.texture)].Sample(samplers[SCENE_SAMPLER], i.uv).rgb;

    if ((m.flags & SCENE_MAT_UNLIT) != 0u)
        return float4(albedo, 1.0f);

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
    float3 l = pc.sun.xyz;                                   /* already normalized on the CPU */

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

    /* scene_set_sun takes the direction toward the light, so there is nothing
       to negate here; flipping it lights the far side of every surface. */
    float3 diffuse = kd * albedo * (1.0f / 3.14159265f);
    float3 direct  = (diffuse + spec) * ndl;

    /* No image-based lighting yet, so the ambient term stands in for the whole
       environment: kd*albedo is the diffuse irradiance and f0*ambient is a flat
       radiance reflection. Without that specular half every metal is black,
       because kd -> 0 and there is no other light for it to reflect. */
    float3 ambient = (kd * albedo + f0) * pc.sun.w * ao;

    /* Linear HDR. Tonemapping and the output gamma live in the post pass now,
       so exposure, bloom and color grading all see the same linear values
       instead of grading an already-encoded image. */
    return float4(direct + ambient, 1.0f);
}
