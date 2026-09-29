#ifndef MU_SCENE_H
#define MU_SCENE_H

#include "../../vk.h"
#include "scene_shared.h"
#include "../../renderer.h"
#include "../../external/mu/mu.h"

#define SCENE_MAX_INSTANCES 65536u
#define SCENE_MAX_MESH_SETS 256u
#define SCENE_MAX_VIEWS     8u
#define SCENE_MAX_MATERIALS 4096u
#define SCENE_MAX_MESH_SLOTS 1024u
#define SCENE_MAX_LOD_LEVELS 8u
#define SCENE_HIZ_MAX_LEVELS 16u
#define SCENE_MAX_COMMANDS  65536u

typedef enum OcclusionMode {
    OCCLUSION_OFF = 0,
    OCCLUSION_HIZ_PREV_FRAME,
} OcclusionMode;

typedef struct Scene {
    VkBackend *vk;

    BufferSlice instance_slice;
    BufferSlice mesh_slice;
    BufferSlice mesh_set_slice;
    BufferSlice lod_slice;
    BufferSlice skin_slice;
    BufferSlice material_slice;
    BufferSlice counter_slice;
    BufferSlice view_slices[SCENE_MAX_VIEWS];
    BufferSlice command_slices[SCENE_MAX_VIEWS];
    BufferSlice visible_slices[SCENE_MAX_VIEWS];
    BufferSlice count_slices[SCENE_MAX_VIEWS];
    Buffer readback[MAX_FRAMES_IN_FLIGHT];

    SceneGpuView views[SCENE_MAX_VIEWS];
    uint32_t    view_count;

    uint32_t instance_count;
    uint32_t instance_total;
    uint32_t last_submitted;
    uint32_t sampler;
    bool     instance_upload_pending;

    RenderTarget hiz[SCENE_HIZ_MAX_LEVELS];
    uint32_t     hiz_levels;
    uint32_t     hiz_warmup;
    uint32_t     hiz_src_w;
    uint32_t     hiz_src_h;

    OcclusionMode occlusion_mode;
    uint32_t      hiz_disable_frames;

    float lod_target;

    PipelineID pipeline_scene;
    PipelineID pipeline_cull;
    PipelineID pipeline_hiz;
} Scene;

typedef struct SceneDesc {
    uint32_t max_instances;
    uint32_t max_mesh_slots;
    uint32_t max_material;
    uint32_t max_views;
} SceneDesc;

typedef struct InstanceUpdate {
    uint32_t    slot;
    SceneGpuInstance data;
} InstanceUpdate;

typedef struct MeshSlotDesc {
    uint64_t vertex_stream;
    uint64_t index_stream;
    uint32_t index_count;
    uint32_t base_vertex;
    uint32_t material;
    float    center[3];
    float    radius;
    uint32_t lod_count;
    const SceneLodRow *lods;
} MeshSlotDesc;

typedef struct MeshSetDesc {
    const uint32_t *mesh_slots;
    uint32_t        mesh_count;
} MeshSetDesc;

typedef struct MaterialDesc {
    uint32_t base_color;
    uint32_t albedo_texture;
    uint32_t flags;
} MaterialDesc;

typedef struct SceneViewDesc {
    float    rows[4][4];
    float    sun[4];
    float    viewport_w;
    float    viewport_h;
    uint32_t caps;
} SceneViewDesc;

typedef struct SceneCounters {
    uint32_t submitted;
    uint32_t culled_frustum;
    uint32_t culled_hiz;
    uint32_t drawn;
    uint32_t dropped_commands;
    uint32_t commands;
    uint32_t lod[4];
} SceneCounters;

typedef enum CameraMode {
    CAM_ORBIT,
    CAM_FLY,
} CameraMode;

typedef struct SceneCamera {
    float position[3];
    float yaw;
    float pitch;
    float fov_y;
    float near_z;
    float far_z;
    mat4  view_proj;
    float clip_rows[4][4];
    CameraMode mode;
    float      speed;
    float      third_dist;
    float      focus[3];
} SceneCamera;

void scene_camera_update(SceneCamera *cam, float aspect);
void scene_camera_mode_update(SceneCamera *cam, CameraMode mode, const Input *input, float dt);

static inline uint32_t scene_pack_quat(float x, float y, float z, float w) {
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f) x = 1.0f;
    if (y < -1.0f) y = -1.0f;
    if (y > 1.0f) y = 1.0f;
    if (z < -1.0f) z = -1.0f;
    if (z > 1.0f) z = 1.0f;
    if (w < -1.0f) w = -1.0f;
    if (w > 1.0f) w = 1.0f;
    uint32_t qx = (uint32_t)lroundf((x + 1.0f) * 511.0f);
    uint32_t qy = (uint32_t)lroundf((y + 1.0f) * 511.0f);
    uint32_t qz = (uint32_t)lroundf((z + 1.0f) * 511.0f);
    uint32_t qw = (uint32_t)lroundf((w + 1.0f) * 1.5f);
    if (qx > 1023u) qx = 1023u;
    if (qy > 1023u) qy = 1023u;
    if (qz > 1023u) qz = 1023u;
    if (qw > 3u) qw = 3u;
    return qx | (qy << 10) | (qz << 20) | (qw << 30);
}

Scene *scene_create(VkBackend *vk, const VkFormat *color_format, const VkFormat *depth_format, const SceneDesc *desc);
void   scene_destroy(Scene *scene);

uint32_t scene_mesh_set_add(Scene *s, const MeshSetDesc *desc);
uint32_t scene_material_add(Scene *s, const MaterialDesc *desc);
uint64_t scene_index_stream(const Scene *s);
uint32_t scene_load_model(Scene *s, VkCommandBuffer cmd, const char *path);

bool scene_instance_reserve(Scene *s, uint32_t count, uint32_t *out_first);
bool scene_upload_instances(Scene *s, VkCommandBuffer cmd, ByteSpan updates);
void scene_instance_free(Scene *s, uint32_t first, uint32_t count);

void scene_frame_begin(Scene *s, VkCommandBuffer cmd);
void scene_view_set(Scene *s, uint32_t view, const SceneViewDesc *desc);
void scene_cull(Scene *s, VkCommandBuffer cmd, uint32_t view);
void scene_draw(Scene *s, VkCommandBuffer cmd, RenderTarget *color, RenderTarget *depth, uint32_t view);
bool scene_counters_read(Scene *s, SceneCounters *out);
void scene_debug_dump(Scene *s);

void scene_set_occlusion(Scene *s, OcclusionMode mode);
void scene_disable_occlusion(Scene *s, uint32_t frames);
void scene_set_lod_target(Scene *s, float target);

bool scene_anim_load(Scene *s, const char *path);
uint32_t scene_anim_find(const char *name);
void scene_skin_play_set(Scene *s, uint32_t set_id, uint32_t walk_clip, uint32_t idle_clip);
void scene_skin_update(Scene *s, VkCommandBuffer cmd, float time);

#endif
