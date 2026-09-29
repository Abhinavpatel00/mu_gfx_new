#ifndef MU_SCENE_INTERNAL_H
#define MU_SCENE_INTERNAL_H

#include "scene.h"

#include "../../external/cgltf/cgltf.h"

void scene_hiz_destroy(Scene *s);
void scene_hiz_build(Scene *s, VkCommandBuffer cmd, RenderTarget *src, uint32_t src_index, uint32_t level);
void scene_assets_init(VkBackend *vk);
void scene_assets_destroy(VkBackend *vk);
void scene_instances_init(uint32_t capacity);
void scene_instances_destroy(void);
VkDeviceAddress scene_slice_addr(const Scene *s, BufferSlice slice);
uint32_t scene_skin_register_from_gltf(VkBackend *vk, VkCommandBuffer cmd, cgltf_data *data,
                                       cgltf_skin *skin, const float mesh_world[4][4]);
void scene_skin_envelope(uint32_t skin_id, float *out_root, float *out_reach);
uint64_t scene_skin_palette_base(uint32_t skin_id, VkBackend *vk);
uint32_t scene_skin_joint_count(uint32_t skin_id);
uint32_t scene_slot_skin_set(uint32_t set_id);
uint32_t scene_skin_palette_slices(BufferSlice *out, uint32_t cap);
uint32_t scene_skin_stream_slices(BufferSlice *out, uint32_t cap);
uint32_t scene_vertex_stream_slices(BufferSlice *out, uint32_t cap);
void scene_skins_destroy(VkBackend *vk);
bool scene_mesh_slot_add(Scene *s, const MeshSlotDesc *desc, uint32_t *out_slot);
SceneGpuMesh *scene_mesh_table(void);
uint32_t scene_mesh_count(void);
uint32_t scene_mesh_set_count(void);
BufferSlice scene_index_pool(void);

#define MAX_MESHES_PER_SET 16

#endif
