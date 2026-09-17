static void pass_nuklear(Renderer *r, VkCommandBuffer cmd) {
    NuklearUi *ui = &r->ui;
    static const struct nk_draw_vertex_layout_element layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, offsetof(UiVertex, position)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, offsetof(UiVertex, uv)},
        {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, offsetof(UiVertex, color)},
        {NK_VERTEX_LAYOUT_END}
    };
    struct nk_convert_config config = {
        .global_alpha = 1.0f, .line_AA = NK_ANTI_ALIASING_ON, .shape_AA = NK_ANTI_ALIASING_ON,
        .circle_segment_count = 22, .arc_segment_count = 22, .curve_segment_count = 22,
        .tex_null = ui->null_texture, .vertex_layout = layout,
        .vertex_size = sizeof(UiVertex), .vertex_alignment = _Alignof(UiVertex)
    };
    nk_buffer_clear(&ui->commands);
    nk_buffer_clear(&ui->vertices);
    nk_buffer_clear(&ui->indices);
    nk_flags result = nk_convert(&ui->context, &ui->commands, &ui->vertices, &ui->indices, &config);
    assert(result == NK_CONVERT_SUCCESS);
    (void)result;
    ui->vertex_count = (uint32_t)(ui->vertices.allocated / sizeof(UiVertex));
    ui->index_count = (uint32_t)(ui->indices.allocated / sizeof(nk_draw_index));
    ui->draw_count = 0;
    VkDeviceSize index_offset = (ui->vertices.allocated + 3) & ~(VkDeviceSize)3;
    VkDeviceSize bytes = index_offset + ui->indices.allocated;
    Buffer *upload = &ui->uploads[r->current_frame];
    // frame_start waited for this slot's timeline value; its mapped storage is now reusable.
    if (bytes > upload->buffer_size) {
        VkDeviceSize capacity = upload->buffer_size;
        while (capacity < bytes)
            capacity *= 2;
        destroy_buffer(r, upload);
        if (!create_buffer(r, capacity,
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST, upload)) {
            log_fatal("[nuklear] failed to grow draw upload buffer");
            exit(EXIT_FAILURE);
        }
    }
    if (bytes) {
        memcpy(upload->mapping, nk_buffer_memory_const(&ui->vertices), ui->vertices.allocated);
        memcpy(upload->mapping + index_offset, nk_buffer_memory_const(&ui->indices), ui->indices.allocated);
        vmaFlushAllocation(r->devc.vmaallocator, upload->allocation, 0, bytes);
        GpuProfiler *frame_prof = &r->gpuprofiler[r->current_frame];
        GPU_SCOPE(frame_prof, cmd, "Nuklear Render", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
            PassAttachment color = {.swapchain_view = r->swapchain.image_views[r->swapchain.current_image],
                                     .load = LOAD_KEEP, .store = STORE_KEEP};
            begin_pass(r, cmd, &(PassDesc){.colors = &color, .color_count = 1});
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui->pipeline);
            vkCmdBindIndexBuffer(cmd, upload->buffer, index_offset, VK_INDEX_TYPE_UINT32);
            UiPush push = {.vertices = upload->address,
                           .scale = {2.0f / (float)r->swapchain.extent.width, -2.0f / (float)r->swapchain.extent.height},
                           .sampler_id = r->default_samplers.samplers[SAMPLER_LINEAR_CLAMP]};
            const struct nk_draw_command *draw;
            uint32_t first_index = 0;
            nk_draw_foreach(draw, &ui->context, &ui->commands) {
                int32_t x0 = (int32_t)MAX(0.0f, floorf(draw->clip_rect.x));
                int32_t y0 = (int32_t)MAX(0.0f, floorf(draw->clip_rect.y));
                int32_t x1 = (int32_t)MIN((float)r->swapchain.extent.width, ceilf(draw->clip_rect.x + draw->clip_rect.w));
                int32_t y1 = (int32_t)MIN((float)r->swapchain.extent.height, ceilf(draw->clip_rect.y + draw->clip_rect.h));
                if (draw->elem_count && x1 > x0 && y1 > y0) {
                    VkRect2D scissor = {.offset = {x0, y0}, .extent = {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
                    vkCmdSetScissor(cmd, 0, 1, &scissor);
                    push.texture_id = (uint32_t)draw->texture.id;
                    emit_root_data(r, cmd, BYTE_SPAN(push));
                    vkCmdDrawIndexed(cmd, draw->elem_count, 1, first_index, 0, 0);
                    ui->draw_count++;
                }
                first_index += draw->elem_count;
            }
            end_pass(cmd);
        }
    }
    nk_clear(&ui->context);
}

static void nuklear_shutdown(Renderer *r) {
    forEach(i, MAX_FRAMES_IN_FLIGHT)
        destroy_buffer(r, &r->ui.uploads[i]);
    vkDestroyPipeline(r->devc.device, r->ui.pipeline, NULL);
    rt_destroy(r, &r->ui.font);
    nk_buffer_free(&r->ui.commands);
    nk_buffer_free(&r->ui.vertices);
    nk_buffer_free(&r->ui.indices);
    nk_free(&r->ui.context);
    nk_font_atlas_clear(&r->ui.atlas);
}


static void render_capture_ui(Renderer *r) {
    CaptureState *c = &r->capture;
    if (!c->inited)
        return;
    struct nk_context *ctx = &r->ui.context;
    if (nk_begin(ctx, "Capture", nk_rect(10, 10, 260, 235), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, 28, 1);
        if (nk_button_label(ctx, "Screenshot")) {
            char path[256];
            snprintf(path, sizeof(path), "screenshot_%llu.png", (unsigned long long)(mu_time_now() / 1000000ull));
            if (!capture_take_screenshot(r, path))
                log_warn("[ui] screenshot request rejected");
        }
        if (!c->recording) {
            if (nk_button_label(ctx, "Start Recording")) {
                char path[256];
                snprintf(path, sizeof(path), "recording_%llu.mp4", (unsigned long long)(mu_time_now() / 1000000000ull));
                if (!capture_start_video(r, path, 60))
                    log_error("[ui] failed to start recording");
            }
        } else {
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(255, 75, 75), "REC  %llu frames", (unsigned long long)c->frames_written);
            if (nk_button_label(ctx, "Stop Recording"))
                capture_stop_video(r);
        }
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_labelf(ctx, NK_TEXT_LEFT, "%ux%u | %d slots | bgra=%d", c->width, c->height, CAPTURE_SLOTS, (int)c->src_is_bgra);
        nk_label(ctx, "F = shot   F9 = record", NK_TEXT_LEFT);
    }
    nk_end(ctx);
}
