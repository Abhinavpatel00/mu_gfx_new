/* rig.c — procedural articulation.
 *
 * Every KayKit character is cooked as separate rigid parts (Body, Head,
 * ArmLeft, ArmRight, LegLeft, LegRight plus a hat or cape). The renderer draws
 * instances, not skeletons, so the articulation happens here: a joint angle set
 * in, one instance transform per part out.
 *
 * The scene instance transform is T(pos) * R(versor) * S(scale) applied to a mesh
 * whose vertices are already in model space, so a part rotating about a pivot p
 * wants pos = p - R*p, not pos = p. Getting that wrong is the whole bug; it is
 * written once, in crypt_rig_solve, and every pose goes through it.
 *
 * The joint table is per model, not per part type: parts are matched to joints
 * by name so re-cooking a source (which can reorder primitives) cannot break the
 * binding, and a model that lacks a part simply never gets that instance. */

#include "crypt.h"

#include <math.h>
#include <string.h>

const vec3 crypt_joint_pivot[J_COUNT] = {
    [J_BODY]  = {0.00f, 0.00f, 0.00f},
    [J_HEAD]  = {0.00f, 1.44f, 0.00f},
    [J_ARM_L] = {0.50f, 1.40f, 0.00f},
    [J_ARM_R] = {-0.50f, 1.40f, 0.00f},
    [J_LEG_L] = {0.17f, 0.56f, 0.00f},
    [J_LEG_R] = {-0.17f, 0.56f, 0.00f},
};

RiggedModel crypt_character[CHARACTER_COUNT];

/* name suffix -> joint. A part that matches nothing here is not animated; it is
   a prop welded to the body (quiver) or headgear that rides the skull (helm,
   hat, mask) and inherits that joint's rotation with no angle of its own. */
typedef struct RigPart {
    const char *suffix;
    Joint        joint;
    bool         rotate;
} RigPart;

static const RigPart kRig[] = {
    {"_Body", J_BODY, false},     {"_Cape", J_BODY, false},     {"_Quiver", J_BODY, false},
    {"_Head", J_HEAD, true},      {"_Helmet", J_HEAD, false},   {"_HelmetVisor", J_HEAD, false},
    {"_Hat", J_HEAD, false},      {"_Mask", J_HEAD, false},      {"_ArmLeft", J_ARM_L, true},
    {"_ArmRight", J_ARM_R, true}, {"_LegLeft", J_LEG_L, true},   {"_LegRight", J_LEG_R, true},
};
#define RIG_PART_COUNT ((uint32_t)(sizeof(kRig) / sizeof(kRig[0])))

/* Names come out of the blob rather than an index: nothing in a cooked .mua
   promises a primitive order, so a hard-coded mesh id is a latent break every
   time the source is re-exported. */
bool crypt_rig_build(Game *g, uint32_t character) {
    char            names[MAX_ASSET_MESHES * 64];
    char            err[256];
    SceneAssetProbe probe;
    uint32_t        count;

    if (!scene_asset_probe(crypt_character_path[character], &probe, err, sizeof(err)))
        return false;
    count = scene_asset_mesh_names(crypt_character_path[character], names, sizeof(names), err, sizeof(err));
    if (count == 0 || count > MAX_ASSET_MESHES)
        return false;

    SceneAssetLoad load;
    if (!scene_asset_load(g->scene, crypt_character_path[character], &load, err, sizeof(err)))
        return false;

    RiggedModel *m = &crypt_character[character];
    m->first_mesh = load.first_mesh;
    m->mesh_count = load.mesh_count;
    m->part_count = 0;

    const char *cursor = names;
    for (uint32_t i = 0; i < count; ++i, cursor += strlen(cursor) + 1u) {
        for (uint32_t r = 0; r < RIG_PART_COUNT; ++r) {
            size_t suffix_len = strlen(kRig[r].suffix);
            size_t name_len    = strlen(cursor);
            if (name_len < suffix_len || strcmp(cursor + name_len - suffix_len, kRig[r].suffix) != 0)
                continue;
            uint32_t p = m->part_count++;
            m->parts[p].mesh   = load.first_mesh + i;
            m->parts[p].joint  = kRig[r].joint;
            m->parts[p].rotate = kRig[r].rotate ? 1u : 0u;
            break;
        }
    }
    return m->part_count >= 4u; /* body, head and two limbs at minimum */
}

/* root transform = character position and facing. Each part's world transform is
   root * joint, so the joint angles are authored in the character's own frame
   and the facing is applied once, outside the loop. */
void crypt_rig_solve(const RiggedModel *m, vec3 root, versor root_rot, versor rot[], vec3 *out_pos,
                     versor *out_quat) {
    vec3 rp[J_COUNT];
    versor rq[J_COUNT];
    glm_quat_rotatev(root_rot, crypt_joint_pivot[J_BODY], rp[J_BODY]);
    for (uint32_t j = 1; j < J_COUNT; ++j) {
        glm_quat_rotatev(root_rot, crypt_joint_pivot[j], rp[j]);
        glm_quat_mul(rq[j], root_rot, rot[j]);
    }
    glm_quat_copy(root_rot, rq[J_BODY]);

    for (uint32_t p = 0; p < m->part_count; ++p) {
        uint32_t j = m->parts[p].joint;
        if (!m->parts[p].rotate) {
            out_pos[p][0] = root[0] + rp[j][0];
            out_pos[p][1] = root[1] + rp[j][1];
            out_pos[p][2] = root[2] + rp[j][2];
            glm_quat_copy(rq[j], out_quat[p]);
            continue;
        }
        /* Rotate about the pivot: world = root + Rp - R(Rp). */
        vec3 swing;
        glm_quat_rotatev(rq[j], crypt_joint_pivot[j], swing);
        out_pos[p][0] = root[0] + rp[j][0] - swing[0];
        out_pos[p][1] = root[1] + rp[j][1] - swing[1];
        out_pos[p][2] = root[2] + rp[j][2] - swing[2];
        glm_quat_copy(rq[j], out_quat[p]);
    }
}

/* ---- poses -------------------------------------------------------------
 * Locomotion is two sines: legs and arms in antiphase off one phase, the body
 * counter-rotating at twice it. Everything is a function of (phase, speed), so
 * the pose is reproducible and blendable — no keyframes, no clip memory, and
 * nothing to stream.
 *
 * Each joint is three Euler angles composed in one fixed order, so a pose is
 * readable as numbers and the composition cost is a known constant rather than
 * a chain whose order matters at every call site. Limbs are authored hanging
 * down -Y, so roll about Z swings them forward and pitch about X twists them. */

static void euler(versor q, float yaw, float pitch, float roll) {
    vec3   axis[3] = {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    float  angle[3] = {yaw, pitch, roll};
    versor qa[3];
    for (int i = 0; i < 3; ++i)
        glm_quatv(qa[i], angle[i], axis[i]);
    glm_quat_mul(q, qa[0], qa[1]);
    glm_quat_mul(q, q, qa[2]);
}

void crypt_pose_idle(versor rot[], float anim, float speed01) {
    float s = sinf(anim), c = cosf(anim);
    float amp = 0.35f + 0.65f * speed01;

    euler(rot[J_LEG_L], 0.0f, s * 0.30f * amp, s * 0.85f * amp);
    euler(rot[J_LEG_R], 0.0f, -s * 0.30f * amp, -s * 0.85f * amp);
    euler(rot[J_ARM_L], 0.0f, -s * 0.22f * amp, -s * 0.55f * amp);
    euler(rot[J_ARM_R], 0.0f, s * 0.22f * amp, s * 0.55f * amp);
    euler(rot[J_BODY], s * 0.08f * amp, c * 0.07f * amp, 0.0f);
    euler(rot[J_HEAD], -s * 0.09f * amp, 0.0f, 0.0f);
}

/* A swing is one arc: wind up, chop through, recover. t is 0..1 across the
   whole motion, and the arc is deliberately front-loaded so the hit lands
   where the player is looking rather than after the animation finishes. */
void crypt_pose_swing(versor rot[], float t) {
    const float kWindupEnd = 0.28f, kImpact = 0.46f;

    float angle;
    if (t < kWindupEnd)
        angle = -1.15f * (t / kWindupEnd);
    else if (t < kImpact)
        angle = -1.15f + 2.55f * ((t - kWindupEnd) / (kImpact - kWindupEnd));
    else
        angle = 1.40f * (1.0f - (t - kImpact) / (1.0f - kImpact));

    /* Lead arm sweeps hardest, trailing arm follows at 60%. */
    euler(rot[J_ARM_R], 0.0f, 0.0f, angle);
    euler(rot[J_ARM_L], 0.0f, 0.0f, angle * 0.6f);
    euler(rot[J_LEG_L], 0.0f, 0.0f, -0.25f + angle * 0.12f);
    euler(rot[J_LEG_R], 0.0f, 0.0f, 0.25f - angle * 0.10f);
    euler(rot[J_BODY], -angle * 0.30f, 0.12f * (1.0f - t), 0.0f);
    euler(rot[J_HEAD], angle * 0.12f, 0.0f, 0.0f);
}