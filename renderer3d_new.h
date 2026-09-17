#ifndef MU_GFX_RENDERER3D_H
#define MU_GFX_RENDERER3D_H
#include "vk.h"
#include "src/scene3d_shared.h"

typedef struct MeshId { mu_weak_handle handle; } MeshId;
typedef struct MaterialId { mu_weak_handle handle; } MaterialId;
typedef struct InstanceId { mu_weak_handle handle; } InstanceId;

typedef struct MeshDesc {
    ByteSpan vertices; /* SceneVertex records: float position/normal/UV. */
    ByteSpan indices;  /* uint32_t triangle indices, copied at creation. */
} MeshDesc;

typedef struct InstanceDesc {
    MeshId mesh;
    MaterialId material;
    /* Row-major affine transform; nonsingular, positive determinant. */
    SceneVector rows[3];
    SceneVector tint;
} InstanceDesc;

typedef struct SceneView {
    SceneVector clip_rows[4]; /* Vulkan zero-to-one clip depth. */
    SceneVector light;        /* xyz = direction toward sun, w = ambient [0,1]. */
} SceneView;

typedef enum SceneMode { SCENE_GPU_CULL, SCENE_CPU_DIRECT } SceneMode;
typedef struct Renderer3DDesc {
    uint32_t max_instances;
    VkFormat color_format;
    VkFormat depth_format;
} Renderer3DDesc;
typedef struct Render3DStats {
    uint32_t instances;
    uint32_t batches;
    uint32_t cpu_visible;
    uint64_t snapshot_bytes;
    uint64_t geometry_upload_bytes;
} Render3DStats;

typedef struct Renderer3DState Renderer3DState;
typedef struct Renderer3D { Renderer3DState *state; } Renderer3D;

bool renderer3d_create(VkBackend *vk, Renderer3D *renderer, const Renderer3DDesc *desc);
void renderer3d_destroy(VkBackend *vk, Renderer3D *renderer);
/* Destruction is immediate: detach instances and wait for all prepared/recorded/
   submitted uses (or explicitly defer destruction) before destroying resources.
   Scene membership does not retain resources. Renderer teardown destroys leftovers. */
MeshId renderer3d_mesh_create(VkBackend *vk, Renderer3D *renderer, const MeshDesc *desc);
void renderer3d_mesh_destroy(VkBackend *vk, Renderer3D *renderer, MeshId mesh);
MaterialId renderer3d_material_create(Renderer3D *renderer, SceneVector linear_color);
void renderer3d_material_destroy(Renderer3D *renderer, MaterialId material);
InstanceId renderer3d_instance_create(Renderer3D *renderer, const InstanceDesc *desc);
void renderer3d_instance_destroy(Renderer3D *renderer, InstanceId instance);
void renderer3d_instance_update(Renderer3D *renderer, InstanceId instance, const InstanceDesc *desc);
bool renderer3d_instance_get(Renderer3D *renderer, InstanceId instance, InstanceDesc *out);

/* Prepare on a reclaimed frame slot. No scene/resource mutations between prepare
   and record. Prepared data survives until that slot is reclaimed; neither call
   submits, presents or waits. max_instances is an explicit scene admission budget. */
void renderer3d_prepare(VkBackend *vk, Renderer3D *renderer, VkCommandBuffer cmd,
                        const SceneView *view, SceneMode mode);
void renderer3d_record(VkBackend *vk, Renderer3D *renderer, VkCommandBuffer cmd,
                       RenderTarget *color, RenderTarget *depth);
Render3DStats renderer3d_stats(const Renderer3D *renderer);
#endif
