/* scene.h — GPU-driven, data-oriented 3D scene (public API).
   See docs/gpu-driven-dod-redesign.md. The game owns CPU truth; the renderer
   owns GPU visibility, LOD, compaction and command generation. */
#ifndef MU_GFX_SCENE_H
#define MU_GFX_SCENE_H

#include "scene_shared.h"
#include "../../vk.h"

#define SCENE_MAX_VIEWS 4

typedef struct Scene Scene;

typedef struct SceneDesc {
    uint32_t max_instances;
    uint32_t max_meshes;
    uint32_t max_lod_rows;
    uint32_t max_materials;
    uint32_t max_survivors; /* candidates-style upper bound on visible entries */
} SceneDesc;

typedef struct SceneMeshDesc {
    const struct ScenePackedVertex *vertices; /* mesh-local packed vertices */
    uint32_t                        vertex_count;
    const uint16_t                 *indices;
    uint32_t                        index_count;
    float                           local_center[3];
    float                           local_radius;
    uint32_t                        material;
} SceneMeshDesc;

typedef struct SceneInstanceDesc {
    float    pos[3];
    float    quat[4]; /* xyzw; zero -> identity */
    float    scale;   /* uniform; 0 -> 1 */
    uint32_t mesh;
    float    lod_bias;
} SceneInstanceDesc;

typedef struct SceneViewDesc {
    float clip_rows[4][4]; /* row-major view-projection rows */
    float camera_pos[3];
    float lod_target; /* screen-space error target; unused while lod_count == 1 */
    float near_z;
    float far_z;
} SceneViewDesc;

typedef struct SceneCounters {
    uint32_t submitted;
    uint32_t culled_frustum;
    uint32_t culled_hiz;
    uint32_t drawn;
    uint32_t lod[4];
    uint32_t draws;
    uint32_t dropped;
} SceneCounters;

Scene *scene_create(VkBackend *vk, const SceneDesc *desc);
void   scene_destroy(Scene *s);

/* Stage a mesh on the CPU. Call scene_upload_scene() before the first frame. */
uint32_t scene_mesh_add(Scene *s, const SceneMeshDesc *desc);

/* Instance slots are dense; create returns the new slot. */
uint32_t scene_instance_create(Scene *s, const SceneInstanceDesc *desc);
void     scene_instance_set(Scene *s, uint32_t slot, const SceneInstanceDesc *desc);

extern const SceneInstanceDesc kSceneInstanceIdentity; /* zero-filled */

/* Upload all staged assets, materials, group templates and instances into the
   gpu pool. Called once with a frame command buffer. */
bool scene_upload_scene(Scene *s, VkCommandBuffer cmd);

void scene_view_set(Scene *s, uint32_t view, const SceneViewDesc *view_desc);
void scene_set_sun(Scene *s, const float dir[3], float ambient);
void scene_set_clear(Scene *s, const float rgba[4]);

/* Record one frame: dirty upload, cull, compact, draw. Opens and closes its own
   graphics pass into color/depth. */
void scene_frame(Scene *s, VkCommandBuffer cmd, RenderTarget *color, RenderTarget *depth);

/* GPU truth, lag-2; false until the first sample is available. */
bool scene_counters(const Scene *s, SceneCounters *out);

/* Pack a mesh-local vertex into the 16-byte GPU format. */
void scene_pack_vertex(struct ScenePackedVertex *out, const float pos[3], const float normal[3], const float uv[2]);

#endif /* MU_GFX_SCENE_H */
