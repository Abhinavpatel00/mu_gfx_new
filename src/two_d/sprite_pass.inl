/* The sprite pass: upload the CPU-built stream, cull and compact it on the GPU,
   then issue one indirect draw per batch. Included by renderer.c. */

#include "sprite.h"

static void sprite_pass_clear_only(SpriteSystem *s, VkCommandBuffer cmd, RenderTarget *target, LoadOp load,
                                   const float *clear) {
    PassAttachment color = {.target = target, .load = load, .store = STORE_KEEP};
    if (clear) {
        forEach(i, 4) color.clear[i] = clear[i];
    }
    begin_pass(s->vk, cmd, &(PassDesc){.colors = &color, .color_count = 1, .pipeline = 0});
    end_pass(cmd);

    s->last_instances = 0;
    s->last_batches   = 0;
    s->last_draws     = 0;
}

void sprite_flush(SpriteSystem *s, VkCommandBuffer cmd, RenderTarget *target, LoadOp load, const float clear[4]) {
    VkBackend  *vk = s->vk;
    SpritePush  pc = {0};
    SpriteLayout layout;

    if (!sprite_prepare(s, &layout)) {
        sprite_pass_clear_only(s, cmd, target, load, clear);
        return;
    }

    pc.batch_count = layout.batch_count;
    pc.block_count = layout.block_count;
    pc.cpu_off     = (uint32_t)layout.stream_base;
    pc.gpu_off     = (uint32_t)layout.out_base;
    pc.viewport[0] = (float)target->width;
    pc.viewport[1] = (float)target->height;

    VkDeviceSize flush_bytes = SPRITE_CPU_FLUSH_END(0, layout.block_count, layout.batch_count);
    VkDeviceSize fblock_off  = SPRITE_FBLOCK_OFF(0, layout.block_count);

    /* The instances and the block table share one upload: two copies, one
       barrier covering the whole flush. */
    BufferSlice instances = {.buffer = s->stream.buffer,
                             .offset = layout.stream_base,
                             .size   = layout.stream_bytes};
    if (!renderer_upload_buffer_to_slice(vk, cmd, instances, (ByteSpan){s->sorted, (uint32_t)layout.stream_bytes})) {
        sprite_pass_clear_only(s, cmd, target, load, clear);
        return;
    }

    BufferSlice fblock = {.buffer = s->stream.buffer,
                          .offset = layout.stream_base + fblock_off,
                          .size   = flush_bytes - fblock_off};
    renderer_upload_buffer_to_slice(vk, cmd, fblock,
                                    (ByteSpan){s->first_block, (layout.batch_count + 1) * sizeof(uint32_t)});

    cmd_buffer_barrier(cmd, s->stream.buffer, layout.stream_base, flush_bytes, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    GpuProfiler *frame_prof = &vk->gpuprofiler[vk->current_frame];

    /* 1. per-block visibility counts */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->render_pipelines.pipelines[s->cs_count - 1]);
    dispatch_push(vk, cmd, BYTE_SPAN(pc), layout.block_count, 1, 1);
    cmd_buffer_barrier(cmd, s->stream.buffer, layout.stream_base, flush_bytes,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    /* 2. block prefix sum, then each batch's output base and indirect command */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->render_pipelines.pipelines[s->cs_prefix - 1]);
    dispatch_push(vk, cmd, BYTE_SPAN(pc), 1, 1, 1);
    cmd_buffer_barrier(cmd, s->stream.buffer, layout.stream_base, flush_bytes,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    /* 3. gather the survivors into the compacted stream, keeping batch order */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->render_pipelines.pipelines[s->cs_compact - 1]);
    dispatch_push(vk, cmd, BYTE_SPAN(pc), layout.block_count, 1, 1);

    cmd_buffer_barrier(cmd, s->out.buffer, layout.out_base, (VkDeviceSize)layout.padded_count * SPRITE_INST_BYTES,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    cmd_buffer_barrier(cmd, s->stream.buffer, layout.stream_base, flush_bytes,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                       VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    uint32_t draws = 0;
    GPU_SCOPE(frame_prof, cmd, "Sprites", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
        PassAttachment color = {.target = target, .load = load, .store = STORE_KEEP};
        if (clear) {
            forEach(i, 4) color.clear[i] = clear[i];
        }
        begin_pass(vk, cmd, &(PassDesc){.colors = &color, .color_count = 1, .pipeline = 0});

        VkPipeline    bound     = VK_NULL_HANDLE;
        VkDeviceSize  indir_off = layout.stream_base + SPRITE_INDIRECT_OFF(0, layout.block_count, layout.batch_count);

        for (uint32_t b = 0; b < layout.batch_count; b++) {
            const SpriteBatch *batch = &s->batches[b];
            if (batch->real_count == 0)
                continue;

            uint32_t key     = batch->key;
            uint32_t blend   = (key >> SPRITE_KEY_BLEND_SHIFT) & 0xFu;
            uint32_t atlas   = (key >> SPRITE_KEY_ATLAS_SHIFT) & 0xFu;
            uint32_t sampler = (key >> SPRITE_KEY_SAMPLER_SHIFT) & 0x3u;

            VkPipeline pipeline = vk->render_pipelines.pipelines[s->pipelines[blend] - 1];
            if (pipeline != bound) {
                bound = pipeline;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            }

            SpritePush dp = pc;
            dp.batch      = b;
            dp.texture_id = s->atlases[atlas].texture;
            dp.sampler_id = s->sampler_ids[sampler];

            cmd_draw_indirect(vk, cmd, BYTE_SPAN(dp),
                              (BufferSlice){.buffer = s->stream.buffer,
                                            .offset = indir_off + (VkDeviceSize)b * 16,
                                            .size   = sizeof(VkDrawIndirectCommand)},
                              1, sizeof(VkDrawIndirectCommand));
            draws++;
        }

        end_pass(cmd);
    }

    s->last_instances = s->count;
    s->last_batches   = layout.batch_count;
    s->last_draws     = draws;
}

static void pass_sprites(Renderer *r, VkCommandBuffer cmd) {
    static const float clear[4] = {0.02f, 0.025f, 0.03f, 1.0f};
    sprite_flush(&r->sprites, cmd, &r->hdr_color[r->vk.swapchain.current_image], LOAD_CLEAR, clear);
}
