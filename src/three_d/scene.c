#include "scene.h"
#include "scene_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool scene_span_alloc(Scene *s, VkDeviceSize bytes, BufferSlice *out);

Scene *scene_create(VkBackend *vk, const VkFormat *color_format, const VkFormat *depth_format, const SceneDesc *desc) {
    Scene *s = (Scene *)calloc(1, sizeof(Scene));
    if (!s)
        return NULL;

    s->vk = vk;

    uint32_t max_instances = desc->max_instances ? desc->max_instances : SCENE_MAX_INSTANCES;
    uint32_t max_mesh_slots = desc->max_mesh_slots ? desc->max_mesh_slots : SCENE_MAX_MESH_SLOTS;
    uint32_t max_materials = desc->max_material ? desc->max_material : SCENE_MAX_MATERIALS;
    uint32_t max_views = desc->max_views ? desc->max_views : SCENE_MAX_VIEWS;

    if (!scene_span_alloc(s, (VkDeviceSize)max_instances * sizeof(SceneGpuInstance), &s->instance_slice))
        goto fail;
    if (!scene_span_alloc(s, (VkDeviceSize)max_mesh_slots * sizeof(SceneGpuMesh), &s->mesh_slice))
        goto fail;
    if (!scene_span_alloc(s, (VkDeviceSize)SCENE_MAX_MESH_SETS * sizeof(SceneMeshSet), &s->mesh_set_slice))
        goto fail;
    if (!scene_span_alloc(s, (VkDeviceSize)max_mesh_slots * SCENE_MAX_LOD_LEVELS * sizeof(SceneLodRow),
                          &s->lod_slice))
        goto fail;
    if (!scene_span_alloc(s, (VkDeviceSize)SCENE_MAX_MESH_SLOTS * sizeof(SceneSkinRecord), &s->skin_slice))
        goto fail;
    if (!scene_span_alloc(s, (VkDeviceSize)max_materials * sizeof(SceneGpuMaterial), &s->material_slice))
        goto fail;
    if (!scene_span_alloc(s, (VkDeviceSize)8 * sizeof(uint32_t), &s->counter_slice))
        goto fail;

    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        if (!create_buffer(vk, 128, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_ONLY,
                           &s->readback[i]))
            goto fail;
        memset(s->readback[i].mapping, 0, 128);
    }

    for (uint32_t i = 0; i < max_views; i++) {
        if (!scene_span_alloc(s, (VkDeviceSize)SCENE_MAX_COMMANDS * sizeof(SceneGpuDraw), &s->command_slices[i]))
            goto fail;
        if (!scene_span_alloc(s, (VkDeviceSize)SCENE_MAX_COMMANDS * sizeof(uint32_t), &s->visible_slices[i]))
            goto fail;
        if (!scene_span_alloc(s, (VkDeviceSize)sizeof(uint32_t), &s->count_slices[i]))
            goto fail;
        if (!scene_span_alloc(s, (VkDeviceSize)sizeof(SceneGpuView), &s->view_slices[i]))
            goto fail;
    }

    {
        GraphicsPipelineConfig cfg = pipeline_config_default();
        cfg.vert_path = "compiledshaders/scene.vert.spv";
        cfg.frag_path = "compiledshaders/scene.frag.spv";
        cfg.color_attachment_count = 1;
        cfg.color_formats = color_format;
        cfg.depth_format = *depth_format;
        cfg.blends[0] = blend_disabled();
        s->pipeline_scene = pipeline_create_graphics(vk, &cfg);
    }
    s->pipeline_cull = pipeline_create_compute(vk, "compiledshaders/scene.cs_cull.comp.spv");
    s->pipeline_hiz = pipeline_create_compute(vk, "compiledshaders/hiz.cs_hiz.comp.spv");

    s->occlusion_mode = OCCLUSION_HIZ_PREV_FRAME;
    s->lod_target = 1.0f;
    s->sampler = vk->default_samplers.samplers[SAMPLER_LINEAR_CLAMP];

    scene_assets_init(vk);
    scene_instances_init(max_instances);

    return s;

fail:
    scene_destroy(s);
    return NULL;
}

static void cam_orbit(SceneCamera *cam, const Input *input, float dt) {
    float turn = 1.6f * dt;
    if (mouse_down(input, MOUSE_LEFT)) {
        cam->yaw   += (float)input->mouse_dx * 0.006f;
        cam->pitch -= (float)input->mouse_dy * 0.006f;
    }
    if (key_down(input, KEY_LEFT) || key_down(input, KEY_A)) cam->yaw -= turn;
    if (key_down(input, KEY_RIGHT) || key_down(input, KEY_D)) cam->yaw += turn;
    if (key_down(input, KEY_UP) || key_down(input, KEY_W)) cam->pitch += turn;
    if (key_down(input, KEY_DOWN) || key_down(input, KEY_S)) cam->pitch -= turn;
    if (key_pressed(input, KEY_Q)) cam->third_dist *= 1.15f;
    if (key_pressed(input, KEY_E)) cam->third_dist *= 0.87f;
    cam->third_dist *= 1.0f - (float)input->scroll_y * 0.08f;
    if (cam->pitch > 1.5f) cam->pitch = 1.5f;
    if (cam->pitch < -1.5f) cam->pitch = -1.5f;
    if (cam->third_dist < 6.0f) cam->third_dist = 6.0f;
    if (cam->third_dist > 500.0f) cam->third_dist = 500.0f;

    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    float cy = cosf(cam->yaw), sy = sinf(cam->yaw);
    cam->position[0] = cam->focus[0] + cp * sy * cam->third_dist;
    cam->position[1] = cam->focus[1] + sp * cam->third_dist;
    cam->position[2] = cam->focus[2] - cp * cy * cam->third_dist;
}

static void cam_fly(SceneCamera *cam, const Input *input, float dt) {
    float turn = 1.6f * dt;
    if (mouse_down(input, MOUSE_LEFT)) {
        cam->yaw   += (float)input->mouse_dx * 0.006f;
        cam->pitch -= (float)input->mouse_dy * 0.006f;
    }
    if (cam->pitch > 1.5f) cam->pitch = 1.5f;
    if (cam->pitch < -1.5f) cam->pitch = -1.5f;

    vec3 fwd = {sinf(cam->yaw), 0.0f, -cosf(cam->yaw)};
    vec3 right = {cosf(cam->yaw), 0.0f, sinf(cam->yaw)};
    vec3 move = {0.0f};
    if (key_down(input, KEY_W)) glm_vec3_add(move, fwd, move);
    if (key_down(input, KEY_S)) glm_vec3_sub(move, fwd, move);
    if (key_down(input, KEY_D)) glm_vec3_add(move, right, move);
    if (key_down(input, KEY_A)) glm_vec3_sub(move, right, move);
    if (key_down(input, KEY_SPACE) || key_down(input, KEY_E)) move[1] += 1.0f;
    if (key_down(input, KEY_LEFT_SHIFT) || key_down(input, KEY_Q)) move[1] -= 1.0f;

    if (glm_vec3_norm(move) > 0.001f) {
        glm_vec3_normalize(move);
        glm_vec3_scale(move, cam->speed * dt, move);
        glm_vec3_add(cam->position, move, cam->position);
    }
}

void scene_camera_mode_update(SceneCamera *cam, CameraMode mode, const Input *input, float dt) {
    cam->mode = mode;
    if (mode == CAM_FLY)
        cam_fly(cam, input, dt);
    else
        cam_orbit(cam, input, dt);
}

void scene_camera_update(SceneCamera *cam, float aspect) {
    mat4 view, proj;
    if (cam->mode == CAM_FLY) {
        vec3 fwd = {cosf(cam->pitch) * sinf(cam->yaw), sinf(cam->pitch), -cosf(cam->pitch) * cosf(cam->yaw)};
        vec3 center;
        glm_vec3_add(cam->position, fwd, center);
        vec3 dir;
        glm_vec3_sub(center, cam->position, dir);
        vec3 mid;
        glm_vec3_add(cam->position, dir, mid);
        vec3 world_up = {0.0f, -1.0f, 0.0f};
        glm_lookat(cam->position, mid, world_up, view);
    } else {
        vec3 dir;
        glm_vec3_sub(cam->focus, cam->position, dir);
        vec3 mid;
        glm_vec3_add(cam->position, dir, mid);
        vec3 world_up = {0.0f, -1.0f, 0.0f};
        glm_lookat(cam->position, mid, world_up, view);
    }
    float f = 1.0f / tanf(cam->fov_y * 0.5f);
    memset(proj, 0, sizeof(proj));
    proj[0][0] = f / aspect;
    proj[1][1] = f;
    proj[2][2] = 0.0f;
    proj[2][3] = -1.0f;
    proj[3][2] = cam->near_z;

    glm_mat4_mul(proj, view, cam->view_proj);

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            cam->clip_rows[i][j] = cam->view_proj[j][i];
}

void scene_destroy(Scene *scene) {
    if (!scene)
        return;

    scene_hiz_destroy(scene);
    scene_assets_destroy(scene->vk);
    scene_instances_destroy();
    scene_skins_destroy(scene->vk);

    buffer_pool_free(scene->instance_slice);
    buffer_pool_free(scene->mesh_slice);
    buffer_pool_free(scene->mesh_set_slice);
    buffer_pool_free(scene->lod_slice);
    buffer_pool_free(scene->skin_slice);
    buffer_pool_free(scene->material_slice);
    buffer_pool_free(scene->counter_slice);
    forEach(i, MAX_FRAMES_IN_FLIGHT) destroy_buffer(scene->vk, &scene->readback[i]);

    for (uint32_t i = 0; i < SCENE_MAX_VIEWS; i++) {
        buffer_pool_free(scene->view_slices[i]);
        buffer_pool_free(scene->command_slices[i]);
        buffer_pool_free(scene->visible_slices[i]);
        buffer_pool_free(scene->count_slices[i]);
    }

    free(scene);
}

static bool scene_span_alloc(Scene *s, VkDeviceSize bytes, BufferSlice *out) {
    *out = buffer_pool_alloc(&s->vk->gpu_pool, bytes, 16);
    return out->buffer != NULL;
}

VkDeviceAddress scene_slice_addr(const Scene *s, BufferSlice slice) {
    return s->vk->gpu_base_addr + slice.offset;
}
