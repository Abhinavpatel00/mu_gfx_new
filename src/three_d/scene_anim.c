#include "scene.h"
#include "scene_internal.h"

#include "../../external/cgltf/cgltf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENE_MAX_SKINS 16
#define SCENE_MAX_CLIPS 32

typedef struct AnimChannel {
    char     joint_name[64];
    uint8_t  path;   /* 0 translation, 1 rotation, 2 scale */
    uint8_t  interp; /* 0 step, 1 linear, 2 cubic */
    uint32_t key_count;
    float   *times;
    float   *values; /* key_count * (3 | 4 | 3*stride) floats */
} AnimChannel;

typedef struct AnimClip {
    bool          used;
    char          name[64];
    float         duration;
    uint32_t      channel_count;
    AnimChannel  *channels;
} AnimClip;

typedef struct SkinJoint {
    char     name[64];
    int32_t  parent;
    float    bind_t[3];
    float    bind_r[4];
    float    bind_s[3];
    float    ibm[16]; /* row-major */
} SkinJoint;

typedef struct SkinReg {
    bool         used;
    uint32_t     joint_count;
    SkinJoint   *joints;
    float        root_pos[3];
    float        max_joint_dist;
    float        mesh_world[4][4];
    BufferSlice  palette_slice;
    float       *palette_cpu; /* 2 * SCENE_SKIN_PHASES * joints * 16 floats */
    uint32_t     clip_walk;
    uint32_t     clip_idle;
    bool         has_clips;
} SkinReg;

static SkinReg  g_skins[SCENE_MAX_SKINS];
static AnimClip g_clips[SCENE_MAX_CLIPS];

static void skin_bind_palette(const SkinReg *reg, float *dst);

static void anim_quat_normalize(float q[4]) {
    float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 1e-8f) {
        q[0] /= len;
        q[1] /= len;
        q[2] /= len;
        q[3] /= len;
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

static void anim_quat_slerp(const float a[4], const float b[4], float t, float out[4]) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float b1[4];
    if (dot < 0.0f) {
        dot = -dot;
        b1[0] = -b[0];
        b1[1] = -b[1];
        b1[2] = -b[2];
        b1[3] = -b[3];
    } else {
        memcpy(b1, b, sizeof(b1));
    }
    float s0, s1;
    if (dot > 0.9995f) {
        s0 = 1.0f - t;
        s1 = t;
    } else {
        float theta = acosf(dot);
        float inv = 1.0f / sinf(theta);
        s0 = sinf((1.0f - t) * theta) * inv;
        s1 = sinf(t * theta) * inv;
    }
    out[0] = s0 * a[0] + s1 * b1[0];
    out[1] = s0 * a[1] + s1 * b1[1];
    out[2] = s0 * a[2] + s1 * b1[2];
    out[3] = s0 * a[3] + s1 * b1[3];
    anim_quat_normalize(out);
}

static void anim_trs_to_mat(const float t[3], const float r[4], const float s[3], float out[4][4]) {
    float qx = r[0], qy = r[1], qz = r[2], qw = r[3];
    float xx = qx * qx, yy = qy * qy, zz = qz * qz;
    float xy = qx * qy, xz = qx * qz, yz = qy * qz;
    float wx = qw * qx, wy = qw * qy, wz = qw * qz;
    memset(out, 0, sizeof(float) * 16);
    out[0][0] = (1.0f - 2.0f * (yy + zz)) * s[0];
    out[0][1] = (2.0f * (xy - wz)) * s[1];
    out[0][2] = (2.0f * (xz + wy)) * s[2];
    out[1][0] = (2.0f * (xy + wz)) * s[0];
    out[1][1] = (1.0f - 2.0f * (xx + zz)) * s[1];
    out[1][2] = (2.0f * (yz - wx)) * s[2];
    out[2][0] = (2.0f * (xz - wy)) * s[0];
    out[2][1] = (2.0f * (yz + wx)) * s[1];
    out[2][2] = (1.0f - 2.0f * (xx + yy)) * s[2];
    out[0][3] = t[0];
    out[1][3] = t[1];
    out[2][3] = t[2];
    out[3][3] = 1.0f;
}

static void anim_mat_mul(const float a[4][4], const float b[4][4], float out[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, t, sizeof(t));
}

static uint32_t anim_key_span(const float *times, uint32_t count, float t) {
    if (t <= times[0])
        return 0;
    if (t >= times[count - 1])
        return count - 2;
    uint32_t lo = 0, hi = count - 1;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (times[mid] <= t)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

static void anim_sample_channel(const AnimChannel *ch, float t, float *out_vec, uint32_t *out_dim) {
    uint32_t dim = ch->path == 1 ? 4 : 3;
    uint32_t stride = ch->interp == 2 ? dim * 3 : dim;
    *out_dim = dim;
    if (ch->key_count == 1 || t <= ch->times[0]) {
        uint32_t base = ch->interp == 2 ? dim : 0;
        memcpy(out_vec, &ch->values[base], dim * sizeof(float));
        return;
    }
    if (t >= ch->times[ch->key_count - 1]) {
        uint32_t base = (ch->key_count - 1) * stride + (ch->interp == 2 ? dim : 0);
        memcpy(out_vec, &ch->values[base], dim * sizeof(float));
        return;
    }
    if (ch->interp == 0) {
        uint32_t k = anim_key_span(ch->times, ch->key_count, t);
        memcpy(out_vec, &ch->values[k * stride], dim * sizeof(float));
        return;
    }
    uint32_t k = anim_key_span(ch->times, ch->key_count, t);
    float t0 = ch->times[k], t1 = ch->times[k + 1];
    float u = (t - t0) / (t1 - t0 > 1e-8f ? t1 - t0 : 1e-8f);
    if (ch->interp == 1) {
        const float *a = &ch->values[k * stride];
        const float *b = &ch->values[(k + 1) * stride];
        if (dim == 4) {
            anim_quat_slerp(a, b, u, out_vec);
        } else {
            out_vec[0] = a[0] + (b[0] - a[0]) * u;
            out_vec[1] = a[1] + (b[1] - a[1]) * u;
            out_vec[2] = a[2] + (b[2] - a[2]) * u;
        }
        return;
    }
    float dt = t1 - t0;
    const float *p0 = &ch->values[k * stride + dim];
    const float *m0 = &ch->values[k * stride + dim * 2];
    const float *p1 = &ch->values[(k + 1) * stride + dim];
    const float *m1 = &ch->values[(k + 1) * stride];
    float u2 = u * u, u3 = u2 * u;
    float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    float h10 = u3 - 2.0f * u2 + u;
    float h01 = -2.0f * u3 + 3.0f * u2;
    float h11 = u3 - u2;
    for (uint32_t i = 0; i < dim; i++)
        out_vec[i] = h00 * p0[i] + h10 * dt * m0[i] + h01 * p1[i] + h11 * dt * m1[i];
    if (dim == 4)
        anim_quat_normalize(out_vec);
}

uint32_t scene_skin_register_from_gltf(VkBackend *vk, VkCommandBuffer cmd, cgltf_data *data,
                                       cgltf_skin *skin, const float mesh_world[4][4]) {
    uint32_t jcount = (uint32_t)skin->joints_count;
    if (jcount == 0 || jcount > 128 || !skin->inverse_bind_matrices)
        return UINT32_MAX;
    uint32_t id = UINT32_MAX;
    for (uint32_t i = 0; i < SCENE_MAX_SKINS; i++) {
        if (!g_skins[i].used) {
            id = i;
            break;
        }
    }
    if (id == UINT32_MAX)
        return UINT32_MAX;

    SkinReg *reg = &g_skins[id];
    memset(reg, 0, sizeof(*reg));
    reg->joints = (SkinJoint *)calloc(jcount, sizeof(SkinJoint));
    if (!reg->joints)
        return UINT32_MAX;
    reg->joint_count = jcount;
    memcpy(reg->mesh_world, mesh_world, sizeof(reg->mesh_world));

    for (uint32_t j = 0; j < jcount; j++) {
        cgltf_node *node = skin->joints[j];
        SkinJoint *sj = &reg->joints[j];
        snprintf(sj->name, sizeof(sj->name), "%s", node->name ? node->name : "");
        sj->parent = -1;
        if (node->parent) {
            for (uint32_t k = 0; k < jcount; k++) {
                if (skin->joints[k] == node->parent) {
                    sj->parent = (int32_t)k;
                    break;
                }
            }
        }
        if (node->has_matrix) {
            sj->bind_t[0] = node->matrix[12];
            sj->bind_t[1] = node->matrix[13];
            sj->bind_t[2] = node->matrix[14];
            sj->bind_r[0] = sj->bind_r[1] = sj->bind_r[2] = 0.0f;
            sj->bind_r[3] = 1.0f;
            sj->bind_s[0] = sj->bind_s[1] = sj->bind_s[2] = 1.0f;
        } else {
            memcpy(sj->bind_t, node->translation, sizeof(sj->bind_t));
            memcpy(sj->bind_r, node->rotation, sizeof(sj->bind_r));
            memcpy(sj->bind_s, node->scale, sizeof(sj->bind_s));
        }
        float ibm[16];
        cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibm, 16);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                sj->ibm[r * 4 + c] = ibm[c * 4 + r];
    }
    (void)data;

    float jw[4][4], sm[4][4], ibm_m[4][4];
    reg->max_joint_dist = 0.0f;
    for (uint32_t j = 0; j < jcount; j++) {
        SkinJoint *sj = &reg->joints[j];
        float local[4][4];
        anim_trs_to_mat(sj->bind_t, sj->bind_r, sj->bind_s, local);
        if (sj->parent >= 0) {
            /* parent world recomputed on demand; joints may precede parents so resolve recursively */
            float pw[4][4];
            int32_t p = sj->parent;
            float chain[4][4];
            memcpy(chain, local, sizeof(chain));
            while (p >= 0) {
                SkinJoint *sp = &reg->joints[p];
                float pl[4][4];
                anim_trs_to_mat(sp->bind_t, sp->bind_r, sp->bind_s, pl);
                float next[4][4];
                anim_mat_mul(pl, chain, next);
                memcpy(chain, next, sizeof(chain));
                p = sp->parent;
            }
            memcpy(pw, chain, sizeof(pw));
            memcpy(jw, pw, sizeof(jw));
        } else {
            memcpy(jw, local, sizeof(jw));
            reg->root_pos[0] = jw[0][3];
            reg->root_pos[1] = jw[1][3];
            reg->root_pos[2] = jw[2][3];
        }
        float dx = jw[0][3] - reg->root_pos[0];
        float dy = jw[1][3] - reg->root_pos[1];
        float dz = jw[2][3] - reg->root_pos[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > reg->max_joint_dist)
            reg->max_joint_dist = d;
        memcpy(ibm_m, reg->joints[j].ibm, sizeof(ibm_m));
        float ibm_mm[4][4];
        memcpy(ibm_mm, ibm_m, sizeof(ibm_mm));
        anim_mat_mul(jw, ibm_mm, sm);
        (void)sm;
    }

    VkDeviceSize pal_bytes =
        (VkDeviceSize)(2 * SCENE_SKIN_PHASES * jcount) * 16 * sizeof(float);
    reg->palette_slice = buffer_pool_alloc(&vk->gpu_pool, pal_bytes, 16);
    reg->palette_cpu = (float *)malloc((size_t)pal_bytes);
    if (!reg->palette_slice.buffer || !reg->palette_cpu) {
        if (reg->palette_slice.buffer)
            buffer_pool_free(reg->palette_slice);
        free(reg->joints);
        memset(reg, 0, sizeof(*reg));
        return UINT32_MAX;
    }
    for (uint32_t b = 0; b < 2 * SCENE_SKIN_PHASES; b++)
        skin_bind_palette(reg, &reg->palette_cpu[(size_t)b * jcount * 16]);
    ByteSpan span = {.data = reg->palette_cpu, .size = (uint32_t)pal_bytes};
    renderer_upload_buffer_to_slice(vk, cmd, reg->palette_slice, span);
    cmd_buffer_barrier(cmd, reg->palette_slice.buffer, reg->palette_slice.offset, pal_bytes,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    reg->clip_walk = UINT32_MAX;
    reg->clip_idle = UINT32_MAX;
    reg->used = true;
    return id;
}

void scene_skin_envelope(uint32_t skin_id, float *out_root, float *out_reach) {
    if (skin_id >= SCENE_MAX_SKINS || !g_skins[skin_id].used) {
        out_root[0] = out_root[1] = out_root[2] = 0.0f;
        *out_reach = 0.0f;
        return;
    }
    memcpy(out_root, g_skins[skin_id].root_pos, 3 * sizeof(float));
    *out_reach = g_skins[skin_id].max_joint_dist;
}

uint64_t scene_skin_palette_base(uint32_t skin_id, VkBackend *vk) {
    if (skin_id >= SCENE_MAX_SKINS || !g_skins[skin_id].used)
        return 0;
    return vk->gpu_base_addr + g_skins[skin_id].palette_slice.offset;
}

uint32_t scene_skin_joint_count(uint32_t skin_id) {
    if (skin_id >= SCENE_MAX_SKINS || !g_skins[skin_id].used)
        return 0;
    return g_skins[skin_id].joint_count;
}

bool scene_anim_load(Scene *s, const char *path) {
    (void)s;
    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success || !data) {
        log_error("[scene] anim parse failed: %s", path);
        return false;
    }
    /* Channel times/values live in the binary buffer; without this the
       accessors read through a NULL buffer and leave malloc garbage. */
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        log_error("[scene] anim buffer load failed: %s", path);
        cgltf_free(data);
        return false;
    }
    for (cgltf_size ai = 0; ai < data->animations_count; ai++) {
        cgltf_animation *anim = &data->animations[ai];
        uint32_t id = UINT32_MAX;
        for (uint32_t i = 0; i < SCENE_MAX_CLIPS; i++) {
            if (!g_clips[i].used) {
                id = i;
                break;
            }
        }
        if (id == UINT32_MAX)
            break;
        AnimClip *clip = &g_clips[id];
        memset(clip, 0, sizeof(*clip));
        snprintf(clip->name, sizeof(clip->name), "%s", anim->name ? anim->name : "");
        clip->channels = (AnimChannel *)calloc(anim->channels_count, sizeof(AnimChannel));
        if (!clip->channels)
            break;
        float duration = 0.0f;
        for (cgltf_size ci = 0; ci < anim->channels_count; ci++) {
            cgltf_animation_channel *ch = &anim->channels[ci];
            cgltf_animation_sampler *smp = ch->sampler;
            AnimChannel *dst = &clip->channels[clip->channel_count];
            snprintf(dst->joint_name, sizeof(dst->joint_name), "%s",
                     ch->target_node && ch->target_node->name ? ch->target_node->name : "");
            if (ch->target_path == cgltf_animation_path_type_translation)
                dst->path = 0;
            else if (ch->target_path == cgltf_animation_path_type_rotation)
                dst->path = 1;
            else if (ch->target_path == cgltf_animation_path_type_scale)
                dst->path = 2;
            else
                continue;
            if (smp->interpolation == cgltf_interpolation_type_step)
                dst->interp = 0;
            else if (smp->interpolation == cgltf_interpolation_type_cubic_spline)
                dst->interp = 2;
            else
                dst->interp = 1;
            uint32_t keys = (uint32_t)smp->input->count;
            uint32_t dim = dst->path == 1 ? 4 : 3;
            uint32_t stride = dst->interp == 2 ? dim * 3 : dim;
            dst->times = (float *)malloc((size_t)keys * sizeof(float));
            dst->values = (float *)malloc((size_t)keys * stride * sizeof(float));
            if (!dst->times || !dst->values) {
                free(dst->times);
                free(dst->values);
                continue;
            }
            for (uint32_t k = 0; k < keys; k++) {
                cgltf_accessor_read_float(smp->input, k, &dst->times[k], 1);
                cgltf_accessor_read_float(smp->output, k, &dst->values[(size_t)k * stride], stride);
                if (dst->times[k] > duration)
                    duration = dst->times[k];
            }
            dst->key_count = keys;
            clip->channel_count++;
        }
        clip->duration = duration;
        clip->used = true;
        if (getenv("PETS_SKINDBG")) {
            fprintf(stderr, "[anim] clip[%u] '%s' dur=%.3f channels=%u\n", id, clip->name,
                    (double)clip->duration, clip->channel_count);
            for (uint32_t c = 0; c < clip->channel_count && c < 8; c++) {
                AnimChannel *ch = &clip->channels[c];
                fprintf(stderr, "[anim]   ch%u '%s' path=%u interp=%u keys=%u t0=%.4f tN=%.4f v0=(%.5g %.5g %.5g %.5g)\n",
                        c, ch->joint_name, ch->path, ch->interp, ch->key_count,
                        ch->key_count ? (double)ch->times[0] : -1.0,
                        ch->key_count ? (double)ch->times[ch->key_count - 1] : -1.0,
                        ch->key_count && ch->values ? (double)ch->values[0] : -1.0,
                        ch->key_count && ch->values ? (double)ch->values[1] : -1.0,
                        ch->key_count && ch->values ? (double)ch->values[2] : -1.0,
                        ch->key_count && ch->values ? (double)ch->values[3] : -1.0);
            }
        }
    }
    cgltf_free(data);
    return true;
}

uint32_t scene_anim_find(const char *name) {
    for (uint32_t i = 0; i < SCENE_MAX_CLIPS; i++) {
        if (g_clips[i].used && strcmp(g_clips[i].name, name) == 0)
            return i;
    }
    return UINT32_MAX;
}

void scene_skin_play_set(Scene *s, uint32_t set_id, uint32_t walk_clip, uint32_t idle_clip) {
    (void)s;
    uint32_t skin = scene_slot_skin_set(set_id);
    if (skin >= SCENE_MAX_SKINS || !g_skins[skin].used)
        return;
    if (walk_clip < SCENE_MAX_CLIPS && g_clips[walk_clip].used)
        g_skins[skin].clip_walk = walk_clip;
    if (idle_clip < SCENE_MAX_CLIPS && g_clips[idle_clip].used)
        g_skins[skin].clip_idle = idle_clip;
    g_skins[skin].has_clips =
        g_skins[skin].clip_walk != UINT32_MAX || g_skins[skin].clip_idle != UINT32_MAX;
}

static int32_t skin_resolve_joint(const SkinReg *reg, const char *name) {
    for (uint32_t j = 0; j < reg->joint_count; j++) {
        if (strcmp(reg->joints[j].name, name) == 0)
            return (int32_t)j;
    }
    return -1;
}

static void skin_joint_world(const SkinReg *reg, float local_mats[][4][4], int32_t j, float out[4][4]) {
    int32_t p = reg->joints[j].parent;
    if (p < 0) {
        memcpy(out, local_mats[j], sizeof(float) * 16);
    } else {
        float pw[4][4];
        skin_joint_world(reg, local_mats, p, pw);
        anim_mat_mul(pw, local_mats[j], out);
    }
}

static void skin_bind_palette(const SkinReg *reg, float *dst) {
    float local_mats[128][4][4];
    for (uint32_t j = 0; j < reg->joint_count; j++) {
        SkinJoint *sj = &reg->joints[j];
        anim_trs_to_mat(sj->bind_t, sj->bind_r, sj->bind_s, local_mats[j]);
    }
    for (uint32_t j = 0; j < reg->joint_count; j++) {
        float jw[4][4], sm[4][4], ibm[4][4], mw_sm[4][4];
        skin_joint_world(reg, local_mats, (int32_t)j, jw);
        memcpy(ibm, reg->joints[j].ibm, sizeof(ibm));
        anim_mat_mul(jw, ibm, sm);
        anim_mat_mul(reg->mesh_world, sm, mw_sm);
        memcpy(&dst[j * 16], mw_sm, sizeof(mw_sm));
    }
}

static void scene_skin_sample_pose(SkinReg *reg, uint32_t clip_id, float time, float *out_palette) {
    AnimClip *clip = &g_clips[clip_id];
    float t = clip->duration > 0.0f ? fmodf(time, clip->duration) : 0.0f;

    float local_mats[128][4][4];
    for (uint32_t j = 0; j < reg->joint_count; j++) {
        anim_trs_to_mat(reg->joints[j].bind_t, reg->joints[j].bind_r, reg->joints[j].bind_s,
                        local_mats[j]);
    }
    for (uint32_t c = 0; c < clip->channel_count; c++) {
        AnimChannel *ch = &clip->channels[c];
        int32_t j = skin_resolve_joint(reg, ch->joint_name);
        if (j < 0)
            continue;
        uint32_t dim = 0;
        float v[4] = {0, 0, 0, 1};
        anim_sample_channel(ch, t, v, &dim);
        SkinJoint *sj = &reg->joints[j];
        float tt[3], rr[4], ss[3];
        memcpy(tt, sj->bind_t, sizeof(tt));
        memcpy(rr, sj->bind_r, sizeof(rr));
        memcpy(ss, sj->bind_s, sizeof(ss));
        if (ch->path == 0) {
            if (sj->parent < 0) {
                memcpy(tt, sj->bind_t, sizeof(tt));
            } else {
                memcpy(tt, v, sizeof(tt));
            }
        } else if (ch->path == 1) {
            memcpy(rr, v, sizeof(rr));
        } else {
            memcpy(ss, v, sizeof(ss));
        }
        anim_trs_to_mat(tt, rr, ss, local_mats[j]);
    }
    for (uint32_t j = 0; j < reg->joint_count; j++) {
        float jw[4][4], sm[4][4], ibm[4][4], mw_sm[4][4];
        skin_joint_world(reg, local_mats, (int32_t)j, jw);
        memcpy(ibm, reg->joints[j].ibm, sizeof(ibm));
        anim_mat_mul(jw, ibm, sm);
        anim_mat_mul(reg->mesh_world, sm, mw_sm);
        memcpy(&out_palette[j * 16], mw_sm, sizeof(mw_sm));
    }
}

void scene_skin_update(Scene *s, VkCommandBuffer cmd, float time) {
    for (uint32_t i = 0; i < SCENE_MAX_SKINS; i++) {
        SkinReg *reg = &g_skins[i];
        if (!reg->used || !reg->has_clips)
            continue;
        float walk_dur = reg->clip_walk != UINT32_MAX ? g_clips[reg->clip_walk].duration : 1.0f;
        float idle_dur = reg->clip_idle != UINT32_MAX ? g_clips[reg->clip_idle].duration : 1.0f;
        if (walk_dur <= 0.0f)
            walk_dur = 1.0f;
        if (idle_dur <= 0.0f)
            idle_dur = 1.0f;
        for (uint32_t b = 0; b < 2 * SCENE_SKIN_PHASES; b++) {
            uint32_t anim = b / SCENE_SKIN_PHASES;
            uint32_t phase = b % SCENE_SKIN_PHASES;
            uint32_t clip = anim == 0 ? reg->clip_walk : reg->clip_idle;
            float dur = anim == 0 ? walk_dur : idle_dur;
            float *dst = &reg->palette_cpu[(size_t)b * reg->joint_count * 16];
            if (clip != UINT32_MAX) {
                float pt = time + (float)phase * dur / (float)SCENE_SKIN_PHASES;
                scene_skin_sample_pose(reg, clip, pt, dst);
            } else {
                skin_bind_palette(reg, dst);
            }
        }
        ByteSpan span = {.data = reg->palette_cpu,
                         .size = (uint32_t)(2 * SCENE_SKIN_PHASES * reg->joint_count * 16 *
                                            sizeof(float))};
        static uint32_t skindump_tick = 0;
        if ((skindump_tick++ % 120) == 0) {
            const float *p0 = &reg->palette_cpu[0];
            uint32_t     bad_count = 0;
            /* row-major: translation at out[0][3],out[1][3],out[2][3] = 3,7,11 */
            fprintf(stderr, "[skin] t=%.2f skin=%u joints=%u j0_t=(%.3f,%.3f,%.3f) j1_t=(%.3f,%.3f,%.3f) j1_r=(%.4f,%.4f,%.4f,%.4f)",
                    time, i, reg->joint_count, p0[3], p0[7], p0[11], p0[16 + 3], p0[16 + 7], p0[16 + 11],
                    p0[16 + 0], p0[16 + 1], p0[16 + 2], p0[16 + 5]);
            float max_dev = 0.0f;
            for (uint32_t j = 0; j < reg->joint_count; j++) {
                const float *m = &p0[j * 16];
                for (int k = 0; k < 16; k++) {
                    float want = (k == 0 || k == 5 || k == 10 || k == 15) ? 1.0f : 0.0f;
                    float d = m[k] - want;
                    if (d < 0)
                        d = -d;
                    if (d > max_dev)
                        max_dev = d;
                }
            }
            fprintf(stderr, " dev=%.5f", (double)max_dev);
            for (uint32_t j = 0; j < reg->joint_count; j++) {
                for (uint32_t k = 0; k < 16; k++) {
                    float v = p0[j * 16 + k];
                    if (!(v > -1e10f && v < 1e10f)) {
                        if (bad_count < 4)
                            fprintf(stderr, " BAD j%u[%u]=%g", j, k, (double)v);
                        bad_count++;
                    }
                }
            }
            if (bad_count > 4)
                fprintf(stderr, " (+%u more)", bad_count - 4);
            if (skindump_tick == 1) {
                static float bind_full[128 * 16];
                skin_bind_palette(reg, bind_full);
                const float *bind0 = bind_full;
                fprintf(stderr, "[skin] mesh_world={%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | "
                                "%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g}\n",
                        (double)reg->mesh_world[0][0], (double)reg->mesh_world[0][1], (double)reg->mesh_world[0][2],
                        (double)reg->mesh_world[0][3], (double)reg->mesh_world[1][0], (double)reg->mesh_world[1][1],
                        (double)reg->mesh_world[1][2], (double)reg->mesh_world[1][3], (double)reg->mesh_world[2][0],
                        (double)reg->mesh_world[2][1], (double)reg->mesh_world[2][2], (double)reg->mesh_world[2][3],
                        (double)reg->mesh_world[3][0], (double)reg->mesh_world[3][1], (double)reg->mesh_world[3][2],
                        (double)reg->mesh_world[3][3]);
                for (uint32_t j = 0; j < (reg->joint_count > 2 ? 2u : reg->joint_count); j++) {
                    fprintf(stderr, "[skin] j%u bind_t=(%.6g %.6g %.6g) bind_r=(%.6g %.6g %.6g %.6g) "
                                    "bind_s=(%.6g %.6g %.6g) parent=%d\n",
                            j, (double)reg->joints[j].bind_t[0], (double)reg->joints[j].bind_t[1],
                            (double)reg->joints[j].bind_t[2], (double)reg->joints[j].bind_r[0],
                            (double)reg->joints[j].bind_r[1], (double)reg->joints[j].bind_r[2],
                            (double)reg->joints[j].bind_r[3], (double)reg->joints[j].bind_s[0],
                            (double)reg->joints[j].bind_s[1], (double)reg->joints[j].bind_s[2],
                            reg->joints[j].parent);
                    fprintf(stderr, "[skin] j%u walk m={%.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g "
                                    "%.5g %.5g | %.5g %.5g %.5g %.5g}\n",
                            j, (double)p0[j * 16 + 0], (double)p0[j * 16 + 1], (double)p0[j * 16 + 2],
                            (double)p0[j * 16 + 3], (double)p0[j * 16 + 4], (double)p0[j * 16 + 5],
                            (double)p0[j * 16 + 6], (double)p0[j * 16 + 7], (double)p0[j * 16 + 8],
                            (double)p0[j * 16 + 9], (double)p0[j * 16 + 10], (double)p0[j * 16 + 11],
                            (double)p0[j * 16 + 12], (double)p0[j * 16 + 13], (double)p0[j * 16 + 14],
                            (double)p0[j * 16 + 15]);
                    fprintf(stderr, "[skin] j%u bind m={%.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g "
                                    "%.5g %.5g | %.5g %.5g %.5g %.5g}\n",
                            j, (double)bind0[j * 16 + 0], (double)bind0[j * 16 + 1], (double)bind0[j * 16 + 2],
                            (double)bind0[j * 16 + 3], (double)bind0[j * 16 + 4], (double)bind0[j * 16 + 5],
                            (double)bind0[j * 16 + 6], (double)bind0[j * 16 + 7], (double)bind0[j * 16 + 8],
                            (double)bind0[j * 16 + 9], (double)bind0[j * 16 + 10], (double)bind0[j * 16 + 11],
                            (double)bind0[j * 16 + 12], (double)bind0[j * 16 + 13], (double)bind0[j * 16 + 14],
                            (double)bind0[j * 16 + 15]);
                }
            }
            fprintf(stderr, "\n");
        }
        renderer_upload_buffer_to_slice(s->vk, cmd, reg->palette_slice, span);
        cmd_buffer_barrier(cmd, reg->palette_slice.buffer, reg->palette_slice.offset,
                           reg->palette_slice.size, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
}

uint32_t scene_skin_palette_slices(BufferSlice *out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SCENE_MAX_SKINS && n < cap; i++) {
        if (g_skins[i].used && g_skins[i].palette_slice.buffer)
            out[n++] = g_skins[i].palette_slice;
    }
    return n;
}

void scene_skins_destroy(VkBackend *vk) {    (void)vk;
    for (uint32_t i = 0; i < SCENE_MAX_SKINS; i++) {
        buffer_pool_free(g_skins[i].palette_slice);
        free(g_skins[i].joints);
        free(g_skins[i].palette_cpu);
    }
    memset(g_skins, 0, sizeof(g_skins));
    for (uint32_t i = 0; i < SCENE_MAX_CLIPS; i++) {
        for (uint32_t c = 0; c < g_clips[i].channel_count; c++) {
            free(g_clips[i].channels[c].times);
            free(g_clips[i].channels[c].values);
        }
        free(g_clips[i].channels);
    }
    memset(g_clips, 0, sizeof(g_clips));
}
