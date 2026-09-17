#ifndef MU_GFX_RENDERER3D_H
#define MU_GFX_RENDERER3D_H
#include "vk.h"
#include "src/scene3d_shared.h"
#include "external/mu/mu/mu_bulk_storage.h"

typedef struct MeshId { mu_weak_handle handle; } MeshId;
typedef struct MaterialId { mu_weak_handle handle; } MaterialId;
typedef struct InstanceId { mu_weak_handle handle; } InstanceId;

typedef struct SceneVertex {
    float position[3];
    float normal[3];
    float uv[2];
} SceneVertex;

typedef struct MeshDesc {
    ByteSpan vertices; // SceneVertex records, mesh-local finite half-float positions/UVs.
    ByteSpan indices;  // uint32_t triangle-list indices; copied and narrowed when eligible.
} MeshDesc;

typedef struct InstanceDesc {
    MeshId mesh;
    MaterialId material;
    SceneVector rows[3]; // Row-major nonsingular affine transform, positive determinant; shear supported.
    SceneVector tint;    // Linear RGBA [0,1], published as UNORM8.
} InstanceDesc;

typedef struct SceneView {
    SceneVector clip_rows[4]; // Vulkan zero-to-one depth, non-reversed Z.
    SceneVector sun;          // xyz direction toward sun (nonzero), w ambient [0,1].
} SceneView;

typedef struct Renderer3DDesc {
    uint32_t max_instances;
    uint32_t max_materials;
    VkFormat color_format;
    VkFormat depth_format;
} Renderer3DDesc;

typedef struct Render3DStats {
    uint32_t instances;
    uint32_t batches;
    uint64_t scene_upload_bytes;
    uint64_t geometry_upload_bytes;
    uint64_t geometry_live_bytes;
} Render3DStats;

typedef struct Renderer3DState Renderer3DState;
typedef struct Renderer3D { Renderer3DState *state; } Renderer3D;

bool renderer3d_create(VkBackend *vk, Renderer3D *scene, const Renderer3DDesc *desc);
void renderer3d_destroy(VkBackend *vk, Renderer3D *scene);

// Mesh publication uses the backend GPU pool (INDEX_BUFFER + SHADER_DEVICE_ADDRESS +
// TRANSFER_DST usage required). Returns zero for invalid content or exhausted pool.
MeshId renderer3d_mesh_create(VkBackend *vk, Renderer3D *scene, const MeshDesc *desc);
void renderer3d_mesh_destroy(VkBackend *vk, Renderer3D *scene, MeshId mesh);
MaterialId renderer3d_material_create(Renderer3D *scene, SceneVector linear_color);
void renderer3d_material_destroy(Renderer3D *scene, MaterialId material);
void renderer3d_material_update(Renderer3D *scene, MaterialId material, SceneVector linear_color);
InstanceId renderer3d_instance_create(Renderer3D *scene, const InstanceDesc *desc);
void renderer3d_instance_destroy(Renderer3D *scene, InstanceId instance);
void renderer3d_instance_update(Renderer3D *scene, InstanceId instance, const InstanceDesc *desc);
bool renderer3d_instance_get(const Renderer3D *scene, InstanceId instance, InstanceDesc *out);

// Single-threaded scene mutation before prepare. Material/instance budgets are
// admission limits (create returns zero when full). Stale mutation IDs assert.
// Updates and instance removal leave submitted frame replicas intact. Mesh destruction
// is immediate: detach instances, then wait for ALL recorded/submitted uses before
// freeing its geometry range. No mutations between prepare and record. Submit prepared
// uploads on the graphics queue before preparing another frame; do not discard them.
// Prepare exactly once on a reclaimed frame slot, before record; neither call waits
// or submits. At shutdown wait_idle, drain deletion queues, then destroy the renderer.
void renderer3d_prepare(VkBackend *vk, Renderer3D *scene, VkCommandBuffer cmd, const SceneView *view);
void renderer3d_record(VkBackend *vk, Renderer3D *scene, VkCommandBuffer cmd,
                       RenderTarget *color, RenderTarget *depth);
Render3DStats renderer3d_stats(const Renderer3D *scene);
#endif
