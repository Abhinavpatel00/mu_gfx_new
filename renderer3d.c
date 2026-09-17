#include "renderer3d.h"
#include "src/scene3d_shared.h"
#include <stddef.h>

#define SCENE_INSTANCE_COUNT 3

typedef struct SceneGeometry {
    SceneVector vertices[8];
    uint32_t indices[36];
    SceneInstance instances[SCENE_INSTANCE_COUNT];
} SceneGeometry;

_Static_assert(sizeof(SceneDraw) == sizeof(VkDrawIndexedIndirectCommand), "indirect stride");
_Static_assert(offsetof(SceneDraw, first_instance) == offsetof(VkDrawIndexedIndirectCommand, firstInstance),
               "indirect layout");
_Static_assert(sizeof(SceneInstance) == 32 && sizeof(ScenePush) == 96, "CPU/Slang layout");
_Static_assert(offsetof(ScenePush, clip_rows) == 32, "push matrix offset");

bool renderer3d_create(VkBackend *vk, Renderer3D *scene, VkFormat color, VkFormat depth) {
    *scene = (Renderer3D){0};
    if (!vk->info.feature_chain.core.features.multiDrawIndirect ||
        !vk->info.feature_chain.core.features.drawIndirectFirstInstance ||
        !vk->info.feature_chain.v12.bufferDeviceAddress || !vk->info.feature_chain.v11.shaderDrawParameters ||
        vk->info.properties.limits.maxDrawIndirectCount < SCENE_INSTANCE_COUNT) {
        log_error("[3d] indexed multi-draw, indirect first instance, and device "
                  "addresses are required");
        return false;
    }
    const SceneGeometry geometry = {
        .vertices = {{-1, -1, -1, 0},
                     {1, -1, -1, 0},
                     {1, 1, -1, 0},
                     {-1, 1, -1, 0},
                     {-1, -1, 1, 0},
                     {1, -1, 1, 0},
                     {1, 1, 1, 0},
                     {-1, 1, 1, 0}},
        .indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                    3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5},
        .instances =
            {
                {.center_scale = {-1.25f, 0, 0, 0.7f}, .color = {0.85f, 0.3f, 0.15f, 1}},
                {.center_scale = {1.25f, 0, 0, 0.7f}, .color = {0.15f, 0.6f, 0.85f, 1}},
                {.center_scale = {100, 0, 0, 0.7f}, .color = {0.2f, 1, 0.2f, 1}},
            },
    };
    // Immutable data is published once; no per-object uploads in the frame loop.
    if (!create_buffer(vk, sizeof(geometry),
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, &scene->geometry))
        goto fail;
    memcpy(scene->geometry.mapping, &geometry, sizeof(geometry));
    vmaFlushAllocation(vk->devc.vmaallocator, scene->geometry.allocation, 0, VK_WHOLE_SIZE);
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        if (!create_buffer(vk, SCENE_INSTANCE_COUNT * sizeof(SceneDraw),
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, &scene->commands[i]))
            goto fail;
    }
    scene->cull_pipeline = create_compute_pipeline(vk, "compiledshaders/scene3d.comp.spv");
    if (!scene->cull_pipeline)
        goto fail;
    GraphicsPipelineConfig config = pipeline_config_default();
    config.vert_path = "compiledshaders/scene3d.vert.spv";
    config.frag_path = "compiledshaders/scene3d.frag.spv";
    config.color_attachment_count = 1;
    config.color_formats = &color;
    config.depth_format = depth;
    config.depth_compare_op = VK_COMPARE_OP_LESS;
    config.blends[0] = blend_disabled();
    scene->draw_pipeline = create_graphics_pipeline(vk, &config);
    if (!scene->draw_pipeline)
        goto fail;
    return true;
fail:
    log_error("[3d] scene initialization failed");
    renderer3d_destroy(vk, scene);
    return false;
}

void renderer3d_destroy(VkBackend *vk, Renderer3D *scene) {
    vkDestroyPipeline(vk->devc.device, scene->draw_pipeline, NULL);
    vkDestroyPipeline(vk->devc.device, scene->cull_pipeline, NULL);
    forEach(i, MAX_FRAMES_IN_FLIGHT) destroy_buffer(vk, &scene->commands[i]);
    destroy_buffer(vk, &scene->geometry);
    *scene = (Renderer3D){0};
}

static ScenePush scene_push(Renderer3D *scene, uint32_t slot, uint32_t width, uint32_t height) {
    mat4 view, projection, clip;
    glm_lookat((vec3){4, 3, 7}, (vec3){0, 0, 0}, (vec3){0, 1, 0}, view);
    // Right-handed, zero-to-one Vulkan depth; viewport already flips Y.
    glm_mat4_zero(projection);
    projection[0][0] = 1.7320508f * (float)height / (float)width;
    projection[1][1] = 1.7320508f;
    projection[2][2] = 100.0f / (0.1f - 100.0f);
    projection[2][3] = -1;
    projection[3][2] = 0.1f * projection[2][2];
    glm_mat4_mul(projection, view, clip);
    ScenePush push = {
        .vertices = scene->geometry.address,
        .instances = scene->geometry.address + offsetof(SceneGeometry, instances),
        .commands = scene->commands[slot].address,
        .instance_count = SCENE_INSTANCE_COUNT,
    };
    forEach(i, 4) push.clip_rows[i] = (SceneVector){clip[0][i], clip[1][i], clip[2][i], clip[3][i]};
    return push;
}

static void scene_draw(VkBackend *vk, Renderer3D *scene, VkCommandBuffer cmd, RenderTarget *color,
                       RenderTarget *depth, const ScenePush *push) {
    PassAttachment attachment = {.target = color, .load = LOAD_CLEAR, .clear = {0.02f, 0.025f, 0.03f, 1}};
    PassAttachment z = {.target = depth, .load = LOAD_CLEAR, .clear = {1}};
    begin_pass(vk, cmd, &(PassDesc){.colors = &attachment, .color_count = 1, .depth = &z});
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->draw_pipeline);
    push_constants(vk, cmd, (ByteSpan){.data = push, .size = sizeof(*push)});
    vkCmdBindIndexBuffer(cmd, scene->geometry.buffer, offsetof(SceneGeometry, indices), VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(cmd, scene->commands[vk->current_frame].buffer, 0, push->instance_count,
                             sizeof(SceneDraw));
    end_pass(cmd);
}

void renderer3d_record(VkBackend *vk, Renderer3D *scene, VkCommandBuffer cmd, RenderTarget *color,
                       RenderTarget *depth) {
    ScenePush push = scene_push(scene, vk->current_frame, color->width, color->height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scene->cull_pipeline);
    dispatch_push(vk, cmd, BYTE_SPAN(push), 1, 1, 1);
    VkMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
    };
    vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                   .memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &barrier});
    scene_draw(vk, scene, cmd, color, depth, &push);
}
