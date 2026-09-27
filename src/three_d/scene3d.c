/* GPU-driven 3D pass: buffers, pipelines, dispatches, draws.
 *
 * Nothing here builds a draw command on the CPU. Each frame uploads compact
 * instance records plus a candidate list; the cull kernel writes instanceCount
 * and visible[]; the draws consume them. The CPU never learns the visible
 * count. Model import lives in scene3d_asset.c and only has to fill the
 * asset's vertex/index/material buffers and mesh table. */

#include "scene3d.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One draw per global mesh slot; a model's meshes are contiguous, so the slot
   index is draw_offset + local mesh index. */
#define SCENE3D_MAX_MESH_SLOTS SCENE3D_MAX_BATCHES

#define SCENE3D_FRAME_USAGE                                                                            \
    (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |                           \
     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)

/* ===================================================================== camera
 * scene3d_camera_mode_update  -- integrate the pose from input (game
 *                                frame hook).
 * scene3d_camera_update      -- build the view-proj matrix (render
 *                                hook).  Kept as a separate, cheap call
 *                                because the render hook has the aspect
 *                                ratio but not dt or input.
 * --------------------------------------------------------------------- */

/* Spherical offset -> cartesian, yaw/pitch convention matching the
   existing orbit code (yaw=0 -> -z, pitch=0 -> horizon). */
static void cam_sphere_offset(const SceneCamera *cam, vec3 out_pos) {
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    float cy = cosf(cam->yaw),  sy = sinf(cam->yaw);
    out_pos[0] = cam->focus[0] + cp * sy * cam->third_dist;
    out_pos[1] = cam->focus[1] + sp * cam->third_dist;
    out_pos[2] = cam->focus[2] - cp * cy * cam->third_dist;
}

static void cam_look_at(SceneCamera *cam, mat4 out_view, const vec3 target) {
    vec3 dir;
    glm_vec3_sub(target, cam->position, dir);
    vec3 center;
    glm_vec3_add(cam->position, dir, center);
    vec3 world_up = {0.0f, -1.0f, 0.0f};
    glm_lookat(cam->position, center, world_up, out_view);
}

/* Fly forward from yaw/pitch (yaw=0 -> -z, pitch=0 -> horizon). */
static void cam_fly_forward(const SceneCamera *cam, vec3 out_fwd) {
    out_fwd[0] = cosf(cam->pitch) * sinf(cam->yaw);
    out_fwd[1] = sinf(cam->pitch);
    out_fwd[2] = -cosf(cam->pitch) * cosf(cam->yaw);
}

/* Orbit: mouse-drag or arrow/WASD steer yaw/pitch, scroll zooms.
   The camera orbits cam->focus at cam->third_dist. */
static void cam_orbit(SceneCamera *cam, const Input *input, float dt) {
    float turn = 1.6f * dt;
    if (mouse_down(input, MOUSE_LEFT)) {
        cam->yaw   += (float)input->mouse_dx * 0.006f;
        cam->pitch -= (float)input->mouse_dy * 0.006f;
    }
    if (key_down(input, KEY_LEFT)  || key_down(input, KEY_A)) cam->yaw   -= turn;
    if (key_down(input, KEY_RIGHT) || key_down(input, KEY_D)) cam->yaw   += turn;
    if (key_down(input, KEY_UP)    || key_down(input, KEY_W)) cam->pitch += turn;
    if (key_down(input, KEY_DOWN)  || key_down(input, KEY_S)) cam->pitch -= turn;
    if (key_pressed(input, KEY_Q)) cam->third_dist *= 1.15f;
    if (key_pressed(input, KEY_E)) cam->third_dist *= 0.87f;
    cam->third_dist *= 1.0f - (float)input->scroll_y * 0.08f;

    if (cam->pitch > 1.5f)  cam->pitch = 1.5f;
    if (cam->pitch < -1.5f) cam->pitch = -1.5f;
    if (cam->third_dist < 6.0f)  cam->third_dist = 6.0f;
    if (cam->third_dist > 500.0f) cam->third_dist = 500.0f;

    cam_sphere_offset(cam, cam->position);
}

/* Fly: mouse looks, WASD moves through world space. */
static void cam_fly(SceneCamera *cam, const Input *input, float dt) {
    float turn = 1.6f * dt;
    if (mouse_down(input, MOUSE_LEFT)) {
        cam->yaw   += (float)input->mouse_dx * 0.006f;
        cam->pitch -= (float)input->mouse_dy * 0.006f;
    }
    if (cam->pitch > 1.5f)  cam->pitch = 1.5f;
    if (cam->pitch < -1.5f) cam->pitch = -1.5f;

    /* forward / right in the XZ plane, yaw=0 -> -z. */
    vec3 fwd = {sinf(cam->yaw), 0.0f, -cosf(cam->yaw)};
    vec3 right = {cosf(cam->yaw), 0.0f, sinf(cam->yaw)};
    vec3 move = {0.0f};
    if (key_down(input, KEY_W)) { float m[3]; glm_vec3_scale(fwd, 1.0f, m); glm_vec3_add(move, m, move); }
    if (key_down(input, KEY_S)) { float m[3]; glm_vec3_scale(fwd, -1.0f, m); glm_vec3_add(move, m, move); }
    if (key_down(input, KEY_D)) { float m[3]; glm_vec3_scale(right, 1.0f, m); glm_vec3_add(move, m, move); }
    if (key_down(input, KEY_A)) { float m[3]; glm_vec3_scale(right, -1.0f, m); glm_vec3_add(move, m, move); }
    if (key_down(input, KEY_SPACE) || key_down(input, KEY_E)) move[1] += cam->speed * dt;
    if (key_down(input, KEY_LEFT_SHIFT) || key_down(input, KEY_Q)) move[1] -= cam->speed * dt;

    if (glm_vec3_norm(move) > 0.001f) {
        glm_vec3_normalize(move);
        glm_vec3_scale(move, cam->speed * dt, move);
        glm_vec3_add(cam->position, move, cam->position);
    }
}

void scene3d_camera_mode_update(SceneCamera *cam, CameraMode mode,
                                   const Input *input, float dt) {
    cam->mode = mode;
    switch (mode) {
        case CAM_ORBIT: cam_orbit(cam, input, dt); break;
        case CAM_FLY:   cam_fly(cam, input, dt);   break;
        default:        cam_orbit(cam, input, dt); break;
    }
}

void scene3d_camera_update(SceneCamera *cam, float aspect) {
    mat4 view, proj;
    if (cam->mode == CAM_FLY) {
        vec3 fwd;
        cam_fly_forward(cam, fwd);
        vec3 center;
        glm_vec3_add(cam->position, fwd, center);
        cam_look_at(cam, view, center);
    } else {
        cam_look_at(cam, view, cam->focus);
    }
    float f = 1.0f / tanf(cam->fov_y * 0.5f);
    memset(proj, 0, sizeof(proj));
    proj[0][0] = f / aspect;
    proj[1][1] = f;
    proj[2][2] = 0.0f;
    proj[2][3] = -1.0f;
    proj[3][2] = cam->near_z;

    glm_mat4_mul(proj, view, cam->view_proj);

    /* Shaders take the matrix as four dot-able rows; cglm stores columns. */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            cam->clip_rows[i][j] = cam->view_proj[j][i];
}

/* ---- Lifetime ---------------------------------------------------------- */

static Buffer scene_device_buffer(VkBackend *vk, VkDeviceSize bytes) {
    Buffer b = {0};
    create_device_buffer(vk, bytes, SCENE3D_FRAME_USAGE, &b);
    return b;
}

void scene3d_init(Scene3d *s, VkBackend *vk, const VkFormat *color_format, const VkFormat *depth_format) {
    memset(s, 0, sizeof(*s));
    s->vk = vk;

    s->instance_data  = (SceneGpuInstance *)malloc((size_t)SCENE3D_MAX_INSTANCES * sizeof(SceneGpuInstance));
    s->candidate_data = (SceneCandidate *)malloc((size_t)SCENE3D_MAX_INSTANCES * sizeof(SceneCandidate));
    s->draw_data      = (SceneGpuDraw *)malloc((size_t)SCENE3D_MAX_MESH_SLOTS * sizeof(SceneGpuDraw));
    s->skin_job_data  = (SceneSkinJob *)malloc((size_t)SCENE3D_MAX_SKIN_JOBS * sizeof(SceneSkinJob));
    if (!s->instance_data || !s->candidate_data || !s->draw_data || !s->skin_job_data) {
        fprintf(stderr, "[scene3d] CPU staging allocation failed\n");
        exit(EXIT_FAILURE);
    }

    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        SceneFrameGpu *f = &s->frame[i];
        f->instances     = scene_device_buffer(vk, (VkDeviceSize)SCENE3D_MAX_INSTANCES * sizeof(SceneGpuInstance));
        f->candidates    = scene_device_buffer(vk, (VkDeviceSize)SCENE3D_MAX_INSTANCES * sizeof(SceneCandidate));
        f->draws         = scene_device_buffer(vk, (VkDeviceSize)SCENE3D_MAX_MESH_SLOTS * sizeof(SceneGpuDraw));
        f->visible       = scene_device_buffer(vk, (VkDeviceSize)SCENE3D_MAX_INSTANCES * sizeof(uint32_t));
        f->skin_jobs     = scene_device_buffer(vk, (VkDeviceSize)SCENE3D_MAX_SKIN_JOBS * sizeof(SceneSkinJob));
    }

    {
        GraphicsPipelineConfig cfg = pipeline_config_default();
        cfg.vert_path              = "compiledshaders/scene3d.vert.spv";
        cfg.frag_path              = "compiledshaders/scene3d.frag.spv";
        cfg.color_attachment_count = 1;
        cfg.color_formats          = color_format;
        cfg.depth_format           = *depth_format;
        cfg.blends[0]              = blend_disabled();
        s->pipeline_scene          = pipeline_create_graphics(vk, &cfg);
    }

    s->pipeline_cull = pipeline_create_compute(vk, "compiledshaders/scene3d.comp.spv");
}

void scene3d_destroy(Scene3d *s, VkBackend *vk) {
    scene3d_assets_destroy(s);
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        destroy_buffer(vk, &s->frame[i].instances);
        destroy_buffer(vk, &s->frame[i].candidates);
        destroy_buffer(vk, &s->frame[i].draws);
        destroy_buffer(vk, &s->frame[i].visible);
        destroy_buffer(vk, &s->frame[i].skin_jobs);
    }
    free(s->instance_data);
    free(s->candidate_data);
    free(s->draw_data);
    free(s->skin_job_data);
    memset(s, 0, sizeof(*s));
}

/* ---- Frame build ------------------------------------------------------- */

static void scene_instance_upload(const SceneInstanceSource *inst, const float center[3], float radius,
                                  SceneGpuInstance *gi) {
    float m[4][4];
    float scale[3] = {inst->scale, inst->scale, inst->scale};
    float q[4]     = {inst->orientation[0], inst->orientation[1], inst->orientation[2], inst->orientation[3]};

    /* Inline TRS so the model transform is built once per instance, not twice. */
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;

    m[0][0] = (1.0f - 2.0f * (yy + zz)) * scale[0];
    m[0][1] = (2.0f * (xy - wz)) * scale[1];
    m[0][2] = (2.0f * (xz + wy)) * scale[2];
    m[0][3] = inst->position[0];
    m[1][0] = (2.0f * (xy + wz)) * scale[0];
    m[1][1] = (1.0f - 2.0f * (xx + zz)) * scale[1];
    m[1][2] = (2.0f * (yz - wx)) * scale[2];
    m[1][3] = inst->position[1];
    m[2][0] = (2.0f * (xz - wy)) * scale[0];
    m[2][1] = (2.0f * (yz + wx)) * scale[1];
    m[2][2] = (1.0f - 2.0f * (xx + yy)) * scale[2];
    m[2][3] = inst->position[2];

    for (int r = 0; r < 3; r++)
        memcpy(&gi->rows[r], m[r], sizeof(float) * 4);

    gi->bounds = (SceneVector){m[0][0] * center[0] + m[0][1] * center[1] + m[0][2] * center[2] + m[0][3],
                               m[1][0] * center[0] + m[1][1] * center[1] + m[1][2] * center[2] + m[1][3],
                               m[2][0] * center[0] + m[2][1] * center[1] + m[2][2] * center[2] + m[2][3],
                               radius * fabsf(inst->scale)};
}

/* ---- Global mesh slots ------------------------------------------------- */

static SceneAsset *scene_asset_for_slot(Scene3d *s, uint32_t slot, SceneMesh **out) {
    for (uint32_t mo = 0; mo < s->model_count; mo++) {
        SceneAsset *a = &s->models[mo];
        if (slot >= a->draw_offset && slot < a->draw_offset + a->mesh_count) {
            *out = &a->meshes[slot - a->draw_offset];
            return a;
        }
    }
    *out = NULL;
    return NULL;
}


/* ---- Render ------------------------------------------------------------ */

/* The cull kernel claims slots inside each mesh's visible slice with an
   atomic add, so a slice only needs a correct capacity and base — candidate
   order in the array does not matter. Count every batch in one pass, then
   prefix-sum the bases. */
static void scene_build_draws(Scene3d *s, uint32_t candidate_count) {
    uint32_t counts[SCENE3D_MAX_MESH_SLOTS] = {0};

    for (uint32_t i = 0; i < candidate_count; i++) {
        uint32_t batch = s->candidate_data[i].batch;
        if (batch < SCENE3D_MAX_MESH_SLOTS)
            counts[batch]++;
    }

    uint32_t base = 0;
    for (uint32_t slot = 0; slot < s->mesh_slot_count && slot < SCENE3D_MAX_MESH_SLOTS; slot++) {
        SceneMesh *mesh = NULL;
        scene_asset_for_slot(s, slot, &mesh);
        Scene3dBatchGroup *group = &s->batches[slot];

        group->mesh         = slot;
        group->draw         = slot;
        group->candidates   = counts[slot];
        group->visible_base = base;
        base += counts[slot];

        if (mesh) {
            s->draw_data[slot] = (SceneGpuDraw){.index_count    = mesh->index_count,
                                                .instance_count = 0,
                                                .first_index    = mesh->first_index,
                                                .vertex_offset  = (int32_t)mesh->base_vertex,
                                                .first_instance = group->visible_base,
                                                .pad            = {0, 0, 0}};
        }
    }

    s->draw_count   = s->mesh_slot_count < SCENE3D_MAX_MESH_SLOTS ? s->mesh_slot_count : SCENE3D_MAX_MESH_SLOTS;
    s->batch_count  = s->draw_count;
}

void scene3d_render(Scene3d *s, VkCommandBuffer cmd, RenderTarget *color, RenderTarget *depth,
                    const SceneInstanceSource *instances, uint32_t count, const SceneCamera *cam,
                    const float sun[4]) {
    VkBackend     *vk = s->vk;
    SceneFrameGpu *f  = &s->frame[vk->current_frame];

    s->instance_data_count = 0;
    s->candidate_count     = 0;
    s->skin_job_count      = 0;
    s->last_skinned        = 0;

    if (count > SCENE3D_MAX_INSTANCES)
        count = SCENE3D_MAX_INSTANCES;

    /* 1. CPU prep: compact records plus one candidate per (instance, mesh). */
    for (uint32_t i = 0; i < count; i++) {
        const SceneInstanceSource *src = &instances[i];
        if (!(src->flags & SCENE_INSTANCE_VISIBLE) || src->model >= s->model_count)
            continue;

        SceneAsset       *asset = &s->models[src->model];
        uint32_t          slot  = s->instance_data_count;
        SceneGpuInstance *gi    = &s->instance_data[slot];

        /* Bounds come from the model's first mesh (uniform scale assumed). */
        float center[3] = {0.0f, 0.0f, 0.0f};
        float radius    = 0.0f;
        if (asset->mesh_count) {
            memcpy(center, asset->meshes[0].center, sizeof(center));
            radius = asset->meshes[0].radius;
        }
        scene_instance_upload(src, center, radius, gi);
        gi->tint = src->tint ? src->tint : 0xFFFFFFFFu;

        for (uint32_t m = 0; m < asset->mesh_count; m++) {
            uint32_t mesh_slot = asset->draw_offset + m;
            if (mesh_slot >= SCENE3D_MAX_MESH_SLOTS || s->candidate_count >= SCENE3D_MAX_INSTANCES)
                break;
            s->candidate_data[s->candidate_count++] = (SceneCandidate){.instance = slot, .batch = mesh_slot};
        }

        s->instance_data_count++;
    }

    scene_build_draws(s, s->candidate_count);
    s->last_instances  = s->instance_data_count;
    s->last_candidates = s->candidate_count;
    s->last_draws      = s->draw_count;

    if (s->candidate_count == 0)
        return;


    /* 2. Upload the compacted tables into this frame's buffers. */
    VkDeviceSize inst_bytes = (VkDeviceSize)s->instance_data_count * sizeof(SceneGpuInstance);
    VkDeviceSize cand_bytes = (VkDeviceSize)s->candidate_count * sizeof(SceneCandidate);
    VkDeviceSize draw_bytes = (VkDeviceSize)s->draw_count * sizeof(SceneGpuDraw);
    VkDeviceSize vis_bytes  = (VkDeviceSize)s->instance_data_count * sizeof(uint32_t);

    renderer_upload_buffer_to_slice(
        vk, cmd, (BufferSlice){.buffer = f->instances.buffer, .offset = 0, .size = inst_bytes},
        (ByteSpan){s->instance_data, (uint32_t)inst_bytes});
    renderer_upload_buffer_to_slice(
        vk, cmd, (BufferSlice){.buffer = f->candidates.buffer, .offset = 0, .size = cand_bytes},
        (ByteSpan){s->candidate_data, (uint32_t)cand_bytes});
    renderer_upload_buffer_to_slice(
        vk, cmd, (BufferSlice){.buffer = f->draws.buffer, .offset = 0, .size = draw_bytes},
        (ByteSpan){s->draw_data, (uint32_t)draw_bytes});

    cmd_buffer_barrier(cmd, f->instances.buffer, 0, inst_bytes, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    cmd_buffer_barrier(cmd, f->candidates.buffer, 0, cand_bytes, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    cmd_buffer_barrier(cmd, f->draws.buffer, 0, draw_bytes, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    /* 3. Cull: one thread per candidate, each atomically claiming a slot in
          its mesh's visible slice. Skinning dispatch lands with the
          skinned-model commit; skin_job_count stays 0 until then. */
    ScenePush push = {0};
    memcpy(push.clip_rows, cam->clip_rows, sizeof(push.clip_rows));
    frustum_extract(&push.frustum, (const SceneVector *)cam->clip_rows);
    push.sun            = (SceneVector){sun[0], sun[1], sun[2], sun[3]};
    push.material       = (SceneVector){1.0f, 1.0f, 1.0f, -1.0f};
    push.sampler        = vk->default_samplers.samplers[SAMPLER_LINEAR_CLAMP];
    push.instance_count = s->candidate_count;
    push.instances      = f->instances.address;
    push.candidates     = f->candidates.address;
    push.commands       = f->draws.address;
    push.visible        = f->visible.address;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->render_pipelines.pipelines[s->pipeline_cull - 1]);
    dispatch_push(vk, cmd, BYTE_SPAN(push), (s->candidate_count + 63u) / 64u, 1, 1);

    cmd_buffer_barrier(cmd, f->draws.buffer, 0, draw_bytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    cmd_buffer_barrier(cmd, f->visible.buffer, 0, vis_bytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    /* 4. One indexed indirect draw per mesh slot, inside our own pass.
          Color loads (the first stage on the target cleared it), depth clears
          to the reverse-Z far plane. */
    GpuProfiler *frame_prof = &vk->gpuprofiler[vk->current_frame];
    uint32_t     draws      = 0;
    GPU_SCOPE(frame_prof, cmd, "Scene3D", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
        PassAttachment color_a = {.target = color, .load = LOAD_KEEP, .store = STORE_KEEP};
        PassAttachment depth_a = {.target = depth, .load = LOAD_CLEAR, .store = STORE_KEEP};
        depth_a.clear[0]       = 0.0f;
        begin_pass(vk, cmd,
                   &(PassDesc){.colors = &color_a, .color_count = 1, .depth = &depth_a, .pipeline = 0});
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          vk->render_pipelines.pipelines[s->pipeline_scene - 1]);

        for (uint32_t b = 0; b < s->batch_count; b++) {
            Scene3dBatchGroup *group = &s->batches[b];
            if (!group->candidates)
                continue;

            SceneMesh  *mesh  = NULL;
            SceneAsset *asset = scene_asset_for_slot(s, group->mesh, &mesh);
            if (!mesh)
                continue;

            push.vertex_stream = mesh->vertex_stream;
            const SceneMaterialGpu *mat = (asset && asset->materials && mesh->material < asset->material_count)
                                              ? &asset->materials[mesh->material]
                                              : NULL;
            float albedo = (mat && mat->albedo_texture != UINT32_MAX) ? (float)mat->albedo_texture : -1.0f;
            push.material = mat ? (SceneVector){mat->base_color.x, mat->base_color.y, mat->base_color.z, albedo}
                                : (SceneVector){1.0f, 1.0f, 1.0f, -1.0f};

            vkCmdBindIndexBuffer(cmd, asset->gpu.indices.buffer, 0, VK_INDEX_TYPE_UINT16);
            cmd_draw_indexed_indirect(
                vk, cmd, BYTE_SPAN(push),
                (BufferSlice){.buffer = f->draws.buffer,
                              .offset = (VkDeviceSize)group->draw * sizeof(SceneGpuDraw),
                              .size   = sizeof(SceneGpuDraw)},
                1, sizeof(SceneGpuDraw));
            draws++;
        }
        end_pass(cmd);
    }

    s->last_draws = draws;
}
