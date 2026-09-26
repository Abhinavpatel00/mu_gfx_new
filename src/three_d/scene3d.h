#ifndef MU_SCENE3D_H
#define MU_SCENE3D_H

/* GPU-driven 3D renderer.
 *
 * Frame flow (see docs/three-d-port.md):
 *   game writes SceneInstanceSource TRS -> CPU compacts one SceneGpuInstance
 *   per visible source plus one candidate per (instance, mesh) -> cull dispatch
 *   writes instanceCount + visible[] -> one indexed indirect draw per mesh
 *   slot inside our own color+depth pass.
 *
 * The CPU never builds indirect commands and never knows the visible count.
 * Scene ownership stays with the game (main.c); this module owns assets,
 * frame buffers, pipelines, and the pass. */

#include "../../vk.h"
#include "../scene3d_shared.h"

#define SCENE3D_MAX_INSTANCES   65536u
#define SCENE3D_MAX_SKIN_JOBS   256u
#define SCENE3D_MAX_BATCHES     64u
#define SCENE3D_MAX_MODELS      64u

/* One GPU-visible instance: 3 affine rows, world sphere, tint. 80 bytes;
   matches SceneInstance in scene3d_shared.h. */
typedef struct SceneGpuInstance {
    SceneVector rows[3]; /* affine model matrix; 4th row is implicit */
    SceneVector bounds;  /* xyz world center, w radius */
    uint32_t    tint;
    uint32_t    pad[3];
} SceneGpuInstance;

/* 32 bytes; matches SceneDraw in scene3d_shared.h. The first 20 bytes are
   VkDrawIndexedIndirectCommand, so one stride-32 draw reads directly. */
typedef struct SceneGpuDraw {
    uint32_t index_count;
    uint32_t instance_count; /* GPU-written */
    uint32_t first_index;
    int32_t  vertex_offset;
    uint32_t first_instance; /* index into the visible slot array */
    uint32_t pad[3];
} SceneGpuDraw;

/* 56 bytes, all 8-byte aligned fields first. */
typedef struct SceneSkinJob {
    uint64_t skin_input_addr;
    uint64_t palette_addr;
    uint64_t skinned_out_addr;
    uint32_t vertex_offset;
    uint32_t vertex_count;
    uint32_t palette_offset;
    uint32_t joint_count;
} SceneSkinJob;

/* Persistent per-material GPU record: 112 bytes, matches SceneGpuMaterial in
   scene3d_shared.h. */
typedef SceneGpuMaterial SceneMaterialGpu;

typedef struct SceneMesh {
    uint32_t first_index; /* into the model's combined index buffer */
    uint32_t index_count;
    uint32_t base_vertex; /* added to the index; LOD0 only */
    uint32_t material;
    float    center[3]; /* local space; CPU transforms to world space */
    float    radius;
    uint64_t vertex_stream; /* device address: static or skinned stream */
} SceneMesh;

/* ---- Per-model asset: persistent device buffers, created once ---- */

typedef struct SceneModelGpu {
    Buffer static_vertices;  /* ScenePackedVertex[] over all meshes */
    Buffer skin_input;       /* SceneSkinPackedVertex[] */
    Buffer skinned_vertices; /* ScenePackedVertex[], skinning output */
    Buffer indices;          /* uint16[] combined */
    Buffer materials;        /* SceneGpuMaterial[] */
    Buffer palette;          /* mat4[], inverse bind * node global */
} SceneModelGpu;

typedef struct SceneAsset {
    SceneModelGpu gpu;
    uint32_t       material_base; /* first global material slot */

    SceneMesh     *meshes;
    uint32_t       mesh_count;
    uint32_t       draw_offset; /* first global mesh slot of this model */

    SceneMaterialGpu *materials;
    uint32_t       material_count;

    uint32_t       static_vertex_count;
    uint32_t       skin_vertex_count;
    uint32_t       index_count;

    /* Node-side animation truth (cubepets/blocky are node-animated). */
    SceneNodeData *nodes;
    uint32_t       node_count;

    SceneSkeletonJoint *joints;
    uint32_t            joint_count;
    SceneSkeletonSkin  *skins;
    uint32_t            skin_count;

    SceneClip    *clips;
    uint32_t      clip_count;
    SceneSampler *samplers;
    uint32_t      sampler_count;
    SceneChannel *channels;
    uint32_t      channel_count;

    float    *palette_mats; /* CPU staging, uploaded every frame */
    uint32_t  palette_count;

    uint32_t  flags; /* SCENE_ASSET_* */
    uint32_t  ref_count;
} SceneAsset;

#define SCENE_ASSET_SKINNED  1u
#define SCENE_ASSET_ANIMATED 2u

/* ---- Per-instance world truth, owned by the game ---- */

typedef struct SceneInstanceSource {
    uint32_t model;          /* index into Scene3d.models; UINT32_MAX = free */
    uint32_t flags;
    float    position[3];
    float    scale;
    float    orientation[4]; /* quaternion xyzw */
    uint32_t clip;           /* clip index, UINT32_MAX = bind pose */
    float    time;
    uint32_t tint;
} SceneInstanceSource;

#define SCENE_INSTANCE_VISIBLE 1u
#define SCENE_INSTANCE_SKINNED 2u

/* ---- Frame state, rebuilt every frame into slot-owned GPU buffers ---- */

typedef struct Scene3dBatchGroup {
    uint32_t mesh;         /* global mesh slot this group draws */
    uint32_t draw;         /* index into SceneGpuDraw[]; one draw per mesh slot */
    uint32_t candidates;   /* candidates submitted for this mesh */
    uint32_t visible_base; /* base into the visible slot array */
} Scene3dBatchGroup;

typedef struct SceneFrameGpu {
    Buffer   instances;  /* SceneGpuInstance[] */
    Buffer   candidates; /* SceneCandidate[] */
    Buffer   draws;      /* SceneGpuDraw[] */
    Buffer   visible;    /* uint[] visible instance slots, GPU-written */
    Buffer   skin_jobs;  /* SceneSkinJob[] */
} SceneFrameGpu;

typedef struct Scene3d {
    VkBackend *vk;

    SceneAsset models[SCENE3D_MAX_MODELS];
    uint32_t   model_count;

    /* Global mesh slots: a model's draws are contiguous, so the slot index is
       draw_offset + local mesh index. One slot per mesh. */
    uint32_t mesh_slot_count;

    /* CPU mirror of what gets uploaded this frame. */
    SceneGpuInstance *instance_data;
    SceneCandidate   *candidate_data;
    SceneGpuDraw     *draw_data;
    SceneSkinJob     *skin_job_data;
    uint32_t          instance_data_count;
    uint32_t          candidate_count;
    uint32_t          draw_count;
    uint32_t          skin_job_count;

    Scene3dBatchGroup batches[SCENE3D_MAX_BATCHES];
    uint32_t          batch_count;

    /* Frame buffers: one region per frame-in-flight, allocated once. */
    SceneFrameGpu frame[MAX_FRAMES_IN_FLIGHT];

    /* Skinning output and palette uploads are carved from the GPU pool each
       frame once the skinning kernel lands. */

    PipelineID pipeline_scene;
    PipelineID pipeline_cull;

    /* HUD counters. */
    uint32_t last_instances;
    uint32_t last_candidates;
    uint32_t last_draws;
    uint32_t last_skinned;
} Scene3d;

/* ---- Camera (world space, y-up, reverse-Z) ---- */

typedef struct SceneCamera {
    float position[3];
    float yaw;
    float pitch;
    float fov_y;
    float near_z;
    float far_z;
    /* cglm mat4: 16-byte aligned because glm_mat4_mul uses aligned SSE stores. */
    mat4  view_proj;
    float clip_rows[4][4];
} SceneCamera;

void scene3d_camera_update(SceneCamera *cam, float aspect);

/* ---- Lifetime ---- */

void scene3d_init(Scene3d *s, VkBackend *vk, const VkFormat *color_format, const VkFormat *depth_format);
void scene3d_destroy(Scene3d *s, VkBackend *vk);

/* ---- Assets ---- */

/* Cached by path. Returns UINT32_MAX on failure. */
uint32_t scene3d_load_model(Scene3d *s, const char *path);

/* Frees every asset's device buffers and CPU tables. */
void scene3d_assets_destroy(Scene3d *s);

/* Number of instances one model can spawn before the frame caps are hit. */
uint32_t scene3d_instance_capacity(const Scene3d *s);

/* ---- Per-frame ---- */

/* Uploads instances and candidates, records the cull dispatch, then opens the
   color+depth pass and records one indexed indirect draw per mesh slot.
   Call after the sprite pass (which clears color), before post-processing. */
void scene3d_render(Scene3d *s, VkCommandBuffer cmd, RenderTarget *color, RenderTarget *depth,
                    const SceneInstanceSource *instances, uint32_t count, const SceneCamera *cam,
                    const float sun[4]);

#endif
