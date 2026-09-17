#ifndef MU_GFX_RENDERER3D_H
#define MU_GFX_RENDERER3D_H
#include "vk.h"

typedef struct Renderer3D {
    Buffer geometry;
    Buffer commands[MAX_FRAMES_IN_FLIGHT];
    VkPipeline cull_pipeline;
    VkPipeline draw_pipeline;
} Renderer3D;

bool renderer3d_create(VkBackend *vk, Renderer3D *scene, VkFormat color, VkFormat depth);
void renderer3d_destroy(VkBackend *vk, Renderer3D *scene);
// Record only after the frame slot's previous submission has completed.
void renderer3d_record(VkBackend *vk, Renderer3D *scene, VkCommandBuffer cmd, RenderTarget *color,
                       RenderTarget *depth);
#endif
