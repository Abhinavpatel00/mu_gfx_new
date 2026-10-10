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
    uint32_t max_instances; /* = dynamic_capacity + static_capacity */
    uint32_t dynamic_capacity;
    uint32_t static_capacity;
    uint32_t max_meshes;
    uint32_t max_lod_rows;
    uint32_t max_materials;
    uint32_t max_survivors; /* candidates-style upper bound on visible entries */
} SceneDesc;

/* One rung of a mesh's LOD ladder. The ladder is stored as a flat index range
   per rung rather than a per-vertex lod field, so picking a rung is arithmetic
   on the draw, not a branch per vertex. */
typedef struct SceneLodDesc {
    uint32_t first_vertex; /* into the mesh's own vertex slice */
    uint32_t first_index;  /* into the mesh's own index slice */
    uint32_t index_count;
    float    error;        /* screen-space threshold; ascending over the ladder */
} SceneLodDesc;

typedef struct SceneMeshDesc {
    const struct ScenePackedVertex *vertices; /* mesh-local packed vertices, all rungs back to back */
    uint32_t                        vertex_count;
    const uint16_t                 *indices; /* rung-local, all rungs back to back */
    uint32_t                        index_count;
    const SceneLodDesc             *lods;     /* lod_count entries; NULL means one rung spanning everything */
    uint32_t                        lod_count;
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

/* The backend the scene was created on. Asset loading needs it to upload the
   images a cooked blob references. */
VkBackend *scene_vk(const Scene *s);
void   scene_destroy(Scene *s);

/* Stage a mesh on the CPU. Call scene_upload_scene() before the first frame. */
uint32_t scene_mesh_add(Scene *s, const SceneMeshDesc *desc);

/* Stage a material and return its index, which is what a mesh's .material
   field takes. Returns UINT32_MAX past max_materials. The row is copied whole:
   the cooker already wrote the exact layout the fragment shader reads. */
uint32_t scene_material_add(Scene *s, const struct SceneGpuMaterial *material);

/* One gpu_inst[] table, split by a scene constant rather than a per-row flag:
   slots below dynamic_capacity are rewritten every frame, slots from there up
   are uploaded once. The split is a placement decision made at create time; no
   shader or CPU loop ever asks which region a slot is in.

   Both return a slot in the same space, so callers treat them identically. */
uint32_t scene_instance_create(Scene *s, const SceneInstanceDesc *desc);        /* dynamic */
uint32_t scene_instance_create_static(Scene *s, const SceneInstanceDesc *desc); /* static  */

void scene_instance_set(Scene *s, uint32_t slot, const SceneInstanceDesc *desc);

/* Destroys an instance by removing its candidate rows and retiring its slot.
   O(1) per row; nothing scans for dead instances and no death flag exists.
   Holes are reclaimed by scene_compact_slots() at load boundaries. */
void scene_instance_destroy(Scene *s, uint32_t slot);
bool scene_instance_alive(const Scene *s, uint32_t slot);

/* Reclaims retired slots. O(instances); call at load time, never per event. */
void scene_compact_slots(Scene *s);

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

/* Rungs every mesh of the scene carries: 1 until a LOD ladder arrives. */
uint32_t scene_lod_count(const Scene *s);

/* Switch the scene draw between the PBR pipeline and the cel-shaded variant.
   Both exist from the first frame; this is a pointer choice, not a rebuild.
   The initial value comes from the MU_TOON environment variable. */
void scene_set_toon(Scene *s, bool toon);
bool scene_toon(const Scene *s);

/* Pack a mesh-local vertex into the 16-byte GPU format. */
void scene_pack_vertex(struct ScenePackedVertex *out, const float pos[3], const float normal[3], const float uv[2]);

/* ---- cooked asset loading ------------------------------------------------
   A .mua registers its meshes into the scene; it does not create instances.
   Node transforms are baked into the vertices at cook time, so a model is a
   group of meshes that all sit in one space: place the group by creating one
   instance per mesh at the same transform. first_mesh is the id of the first
   one, and the ids are contiguous from there. */
typedef struct SceneAssetLoad {
    uint32_t first_mesh;
    uint32_t mesh_count;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t lod_count; /* rungs every mesh of this model carries */
    float    center[3];
    float    radius;
} SceneAssetLoad;

/* Header-only look at a blob: what registering it would cost. The caller needs
   this before scene_create, because mesh and LOD capacities are fixed there and
   cannot grow afterwards. Touches the header, the section table and the mesh
   rows — never the vertex or index payload. */
typedef struct SceneAssetProbe {
    uint32_t mesh_count;
    uint32_t material_count;
    uint32_t texture_count;
    uint32_t lod_rows;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t max_lod_count; /* deepest ladder any one mesh carries */
} SceneAssetProbe;

bool scene_asset_probe(const char *path, SceneAssetProbe *out, char *err, uint32_t err_cap);

/* Staging only: it fills the scene's CPU tables and must be followed by
   scene_upload_scene before the first frame. On failure nothing is added. */
bool scene_asset_load(Scene *s, const char *path, SceneAssetLoad *out, char *err, uint32_t err_cap);

#endif /* MU_GFX_SCENE_H */
