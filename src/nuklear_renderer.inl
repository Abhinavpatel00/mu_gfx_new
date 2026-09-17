typedef struct UiVertex {
    float    position[2];
    float    uv[2];
    uint32_t color;
} UiVertex;

PUSH_CONSTANT(UiPush, VkDeviceAddress vertices; float scale[2]; uint32_t texture_id; uint32_t sampler_id;);

static void nuklear_init(Renderer *r) {
    NuklearUi *ui = &r->ui;
    nk_init_default(&ui->context, NULL);
    nk_buffer_init_default(&ui->commands);
    nk_buffer_init_default(&ui->vertices);
    nk_buffer_init_default(&ui->indices);
    nk_font_atlas_init_default(&ui->atlas);
    nk_font_atlas_begin(&ui->atlas);
    // ProggyClean has crisp pixel coverage at its native 13-pixel size.
    struct nk_font_config font_config = nk_font_config(13.0f);
    font_config.pixel_snap = true;
    struct nk_font *font = nk_font_atlas_add_default(&ui->atlas, 13.0f, &font_config);
    int width, height;
    const void *pixels = nk_font_atlas_bake(&ui->atlas, &width, &height, NK_FONT_ATLAS_RGBA32);
    if (!pixels) {
        log_fatal("[nuklear] failed to bake the default font");
        exit(EXIT_FAILURE);
    }
    if (!rt_create(r, &ui->font, &(RenderTargetSpec){
            .width = (uint32_t)width, .height = (uint32_t)height, .layers = 1, .mip_count = 1,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .debug_name = "Nuklear font"})) {
        log_fatal("[nuklear] failed to create font texture");
        exit(EXIT_FAILURE);
    }
    Buffer staging = {0};
    VkDeviceSize bytes = (VkDeviceSize)width * (VkDeviceSize)height * 4;
    if (!create_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging)) {
        log_fatal("[nuklear] failed to create font upload buffer");
        exit(EXIT_FAILURE);
    }
    memcpy(staging.mapping, pixels, (size_t)bytes);
    vmaFlushAllocation(r->devc.vmaallocator, staging.allocation, 0, bytes);
    VkCommandBuffer cmd = vk_begin_one_time_cmd(r->devc.device, r->one_time_gfx_pool);
    rt_transition_all(r, cmd, &ui->font, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    flush_barriers(r, cmd);
    VkBufferImageCopy copy = {.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
                              .imageExtent = {(uint32_t)width, (uint32_t)height, 1}};
    vkCmdCopyBufferToImage(cmd, staging.buffer, ui->font.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    rt_transition_all(r, cmd, &ui->font, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    flush_barriers(r, cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
    VkSemaphoreSubmitInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                     .semaphore = r->timeline, .value = ++r->timeline_last_submitted,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                            .commandBufferInfoCount = 1, .pCommandBufferInfos = &command,
                            .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
    VK_CHECK(vkQueueSubmit2(r->devc.graphics_queue, 1, &submit, VK_NULL_HANDLE));
    VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                .pSemaphores = &r->timeline, .pValues = &r->timeline_last_submitted};
    VK_CHECK(vkWaitSemaphores(r->devc.device, &wait, UINT64_MAX));
    vkFreeCommandBuffers(r->devc.device, r->one_time_gfx_pool, 1, &cmd);
    destroy_buffer(r, &staging);
    nk_font_atlas_end(&ui->atlas, nk_handle_id((int)ui->font.bindless_index), &ui->null_texture);
    nk_style_set_font(&ui->context, &font->handle);
    nk_font_atlas_cleanup(&ui->atlas);

    GraphicsPipelineConfig config = pipeline_config_default();
    config.vert_path = "compiledshaders/nuklear.vert.spv";
    config.frag_path = "compiledshaders/nuklear.frag.spv";
    config.color_attachment_count = 1;
    config.color_formats = &r->swapchain.format;
    config.depth_test_enable = false;
    config.depth_write_enable = false;
    config.blends[0] = blend_alpha();
    ui->pipeline = create_graphics_pipeline(r, &config);
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        if (!create_buffer(r, 256 * 1024,
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST, &ui->uploads[i])) {
            log_fatal("[nuklear] failed to create draw upload buffer");
            exit(EXIT_FAILURE);
        }
    }
}
