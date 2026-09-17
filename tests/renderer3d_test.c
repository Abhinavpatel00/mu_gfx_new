#include "../src/platform.h"
// Exercise the CPU reference and GPU path without adding test switches to the
// public API.
#include "../renderer3d.c"

static void submit_and_wait(VkBackend *vk, VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                         .commandBuffer = cmd};
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &command};
    VK_CHECK(vkQueueSubmit2(vk->devc.graphics_queue, 1, &submit, VK_NULL_HANDLE));
    wait_idle(vk);
    vkFreeCommandBuffers(vk->devc.device, vk->one_time_gfx_pool, 1, &cmd);
}

int main(void) {
    VK_CHECK(volkInitialize());
    if (RGFW_init("scene_test", RGFW_initVulkan | RGFW_initX11) != 0)
        return 1;
    size_t extension_count = 0;
    const char **extensions = RGFW_getRequiredInstanceExtensions_Vulkan(&extension_count);
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};
    VkBackendDesc desc = {
        .app_name = "scene_test",
        .instance_extensions = extensions,
        .instance_extension_count = (uint32_t)extension_count,
        .device_extensions = device_extensions,
        .device_extension_count = 2,
        .width = 256,
        .height = 256,
        .swapchain_preferred_format = VK_FORMAT_B8G8R8A8_SRGB,
        .bindless_sampled_image_count = MAX_BINDLESS_TEXTURES,
        .bindless_storage_image_count = MAX_BINDLESS_TEXTURES,
        .bindless_sampler_count = MAX_BINDLESS_SAMPLERS,
        .size_of_cpu_pool = MB(1),
        .size_of_gpu_pool = MB(1),
        .size_of_staging_pool = MB(1),
    };
    VkBackend *vk;
    (void)posix_memalign((void **)&vk, _Alignof(VkBackend), sizeof(*vk));
    memset(vk, 0, sizeof(*vk));
    vk_instance_create(vk, &desc);
    RGFW_window *window = RGFW_createWindow("Scene test", 0, 0, 256, 256, 0);
    if (!window)
        return 1;
    VK_CHECK(RGFW_window_createSurface_Vulkan(window, vk->instance.instance, &vk->surface));
    vk_backend_create(vk, &desc);
    Renderer3D scene;
    if (!renderer3d_create(vk, &scene, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT))
        return 1;
    RenderTarget color = {0}, depth = {0};
    if (!rt_create(
            vk, &color,
            &(RenderTargetSpec){.width = 256,
                                .height = 256,
                                .layers = 1,
                                .mip_count = 1,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT}) ||
        !rt_create(vk, &depth,
                   &(RenderTargetSpec){.width = 256,
                                       .height = 256,
                                       .layers = 1,
                                       .mip_count = 1,
                                       .format = VK_FORMAT_D32_SFLOAT,
                                       .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT}))
        return 1;
    Buffer readback;
    if (!create_buffer(vk, 256 * 256 * 4 + 3 * sizeof(SceneDraw), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_HOST, &readback))
        return 1;
    uint8_t *reference = malloc(256 * 256 * 4);
    bool passed = true;
    // One instance, two-instance reference, GPU-compacted batch, then empty visibility.
    for (uint32_t test = 0; test < 4; ++test) {
        if (test == 3) {
            SceneGeometry *geometry = (SceneGeometry *)scene.geometry.mapping;
            forEach(i, SCENE_INSTANCE_COUNT) geometry->instances[i].center_scale.x = 100;
            vmaFlushAllocation(vk->devc.vmaallocator, scene.geometry.allocation, 0, VK_WHOLE_SIZE);
        }
        ScenePush push = scene_push(&scene, 0, 256, 256);
        VkCommandBuffer cmd = vk_begin_one_time_cmd(vk->devc.device, vk->one_time_gfx_pool);
        if (test < 2) {
            push.instance_count = test + 1;
            SceneDraw draw = {.index_count = 36, .instance_count = test + 1};
            uint32_t visible[SCENE_INSTANCE_COUNT] = {0, 1, 2};
            memcpy(scene.commands[0].mapping, &draw, sizeof(draw));
            memcpy((uint8_t *)scene.commands[0].mapping + sizeof(draw), visible, sizeof(visible));
            vmaFlushAllocation(vk->devc.vmaallocator, scene.commands[0].allocation, 0, VK_WHOLE_SIZE);
            scene_draw(vk, &scene, cmd, &color, &depth, &push);
        } else {
            renderer3d_record(vk, &scene, cmd, &color, &depth);
        }
        rt_transition_all(vk, cmd, &color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT);
        flush_barriers(vk, cmd);
        VkBufferImageCopy copy = {.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
                                  .imageExtent = {256, 256, 1}};
        vkCmdCopyImageToBuffer(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        if (test >= 2) {
            VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
            vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                           .memoryBarrierCount = 1,
                                                           .pMemoryBarriers = &barrier});
            VkBufferCopy region = {.dstOffset = 256 * 256 * 4,
                                   .size = sizeof(SceneDraw) + SCENE_INSTANCE_COUNT * sizeof(uint32_t)};
            vkCmdCopyBuffer(cmd, scene.commands[0].buffer, readback.buffer, 1, &region);
        }

        VkMemoryBarrier2 host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                 .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                 .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                 .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                       .memoryBarrierCount = 1,
                                                       .pMemoryBarriers = &host});
        submit_and_wait(vk, cmd);
        vmaInvalidateAllocation(vk->devc.vmaallocator, readback.allocation, 0, VK_WHOLE_SIZE);
        uint8_t *pixels = readback.mapping;
        uint32_t red = 0, blue = 0;
        forEach(i, 256 * 256) {
            red += pixels[i * 4] > 2 * pixels[i * 4 + 2] && pixels[i * 4] > 30;
            blue += pixels[i * 4 + 2] > 2 * pixels[i * 4] && pixels[i * 4 + 2] > 30;
        }
        passed &= test == 3 ? red == 0 && blue == 0 : red > 100 && (test == 0 ? blue == 0 : blue > 100);
        if (test == 1)
            memcpy(reference, pixels, 256 * 256 * 4);
        if (test >= 2) {
            if (test == 2)
                passed &= memcmp(reference, pixels, 256 * 256 * 4) == 0;
            SceneDraw *draw = (SceneDraw *)(pixels + 256 * 256 * 4);
            passed &= draw->index_count == 36 && draw->instance_count == (test == 2 ? 2u : 0u) &&
                      draw->first_index == 0 && draw->vertex_offset == 0 && draw->first_instance == 0;
            if (test == 2) {
                uint32_t *visible = (uint32_t *)(draw + 1);
                passed &= (visible[0] == 0 && visible[1] == 1) || (visible[0] == 1 && visible[1] == 0);
            }
        }
        printf("case %u: red=%u blue=%u, %s\n", test, red, blue, passed ? "PASS" : "FAIL");
    }
    free(reference);
    wait_idle(vk);
    delete_queue_drain(vk);
    destroy_buffer(vk, &readback);
    rt_destroy(vk, &depth);
    rt_destroy(vk, &color);
    renderer3d_destroy(vk, &scene);
    vk_backend_destroy(vk);
    vkDestroySurfaceKHR(vk->instance.instance, vk->surface, NULL);
    vk_instance_destroy(vk);
    RGFW_window_close(window);
    RGFW_deinit();
    free(vk);
    return passed ? 0 : 1;
}
