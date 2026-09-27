/* The 2D stage. Everything sprite-shaped lives here: system bring-up, the
   cull-and-draw pass, and the opaque handle the core carries. Nothing below
   reaches into Renderer — the stage gets a VkBackend, a colour format, and the
   target it draws into. */

#include <stdlib.h>
#include <string.h>

#include "sprite.h"
#include "two_d.h"

struct TwoD {
    SpriteSystem sprites;
};

static const ColorAttachmentBlend sprite_blend_table[SPRITE_BLEND_COUNT] = {
    [SPRITE_BLEND_OPAQUE] =
        (ColorAttachmentBlend){
            .src_color  = VK_BLEND_FACTOR_ONE,
            .dst_color  = VK_BLEND_FACTOR_ZERO,
            .color_op   = VK_BLEND_OP_ADD,
            .src_alpha  = VK_BLEND_FACTOR_ONE,
            .dst_alpha  = VK_BLEND_FACTOR_ZERO,
            .alpha_op   = VK_BLEND_OP_ADD,
            .write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
        },
    [SPRITE_BLEND_ALPHA] =
        (ColorAttachmentBlend){
            .blend_enable = true,
            .src_color    = VK_BLEND_FACTOR_SRC_ALPHA,
            .dst_color    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .color_op     = VK_BLEND_OP_ADD,
            .src_alpha    = VK_BLEND_FACTOR_ONE,
            .dst_alpha    = VK_BLEND_FACTOR_ZERO,
            .alpha_op     = VK_BLEND_OP_ADD,
            .write_mask   = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
        },
    [SPRITE_BLEND_ADD] =
        (ColorAttachmentBlend){
            .blend_enable = true,
            .src_color    = VK_BLEND_FACTOR_SRC_ALPHA,
            .dst_color    = VK_BLEND_FACTOR_ONE,
            .color_op     = VK_BLEND_OP_ADD,
            .src_alpha    = VK_BLEND_FACTOR_ONE,
            .dst_alpha    = VK_BLEND_FACTOR_ONE,
            .alpha_op     = VK_BLEND_OP_ADD,
            .write_mask   = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
        },
    [SPRITE_BLEND_PREMULTIPLIED] =
        (ColorAttachmentBlend){
            .blend_enable = true,
            .src_color    = VK_BLEND_FACTOR_ONE,
            .dst_color    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .color_op     = VK_BLEND_OP_ADD,
            .src_alpha    = VK_BLEND_FACTOR_ONE,
            .dst_alpha    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alpha_op     = VK_BLEND_OP_ADD,
            .write_mask   = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
        },
};


/* --- bring-up ------------------------------------------------------------- */

static void sprite_system_init(SpriteSystem *s, VkBackend *vk, const VkFormat *color_format) {
    memset(s, 0, sizeof(*s));
    s->vk = vk;

    s->instances = (SpriteInst *)malloc((size_t)SPRITE_MAX_INSTANCES * sizeof(SpriteInst));
    s->sorted    = (SpriteInst *)malloc((size_t)SPRITE_BUFFER_CAPACITY * sizeof(SpriteInst));
    s->slot_of   = (uint16_t *)malloc((size_t)SPRITE_MAX_INSTANCES * sizeof(uint16_t));
    if (!s->instances || !s->sorted || !s->slot_of) {
        log_fatal("[sprite] failed to allocate CPU scratch");
        exit(EXIT_FAILURE);
    }

    forEach(i, MAX_FRAMES_IN_FLIGHT) s->tail_frame[i] = UINT64_MAX;

    VkDeviceSize stream_bytes = (VkDeviceSize)SPRITE_STREAM_REGION * MAX_FRAMES_IN_FLIGHT;
    VkDeviceSize out_bytes    = (VkDeviceSize)SPRITE_OUT_REGION * MAX_FRAMES_IN_FLIGHT;

    if (!create_device_buffer(vk, stream_bytes,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                              &s->stream)) {
        log_fatal("[sprite] failed to allocate the stream buffer");
        exit(EXIT_FAILURE);
    }
    if (!create_device_buffer(vk, out_bytes,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->out)) {
        log_fatal("[sprite] failed to allocate the compacted stream buffer");
        exit(EXIT_FAILURE);
    }

    VkDescriptorBufferInfo stream_info = {.buffer = s->stream.buffer, .offset = 0, .range = stream_bytes};
    VkDescriptorBufferInfo out_info    = {.buffer = s->out.buffer, .offset = 0, .range = out_bytes};
    VkWriteDescriptorSet   writes[2]   = {
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet          = vk->bindless_system.set,
         .dstBinding      = SPRITE_CPU_BINDING,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo     = &stream_info},
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet          = vk->bindless_system.set,
         .dstBinding      = SPRITE_GPU_BINDING,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo     = &out_info},
    };
    vkUpdateDescriptorSets(vk->devc.device, 2, writes, 0, NULL);

    s->sampler_ids[SPRITE_SAMPLER_NEAREST_CLAMP] = vk->default_samplers.samplers[SAMPLER_NEAREST_CLAMP];
    s->sampler_ids[SPRITE_SAMPLER_LINEAR_CLAMP]  = vk->default_samplers.samplers[SAMPLER_LINEAR_CLAMP];
    s->sampler_ids[SPRITE_SAMPLER_NEAREST_WRAP]  = vk->default_samplers.samplers[SAMPLER_NEAREST_WRAP];
    s->sampler_ids[SPRITE_SAMPLER_LINEAR_WRAP]   = vk->default_samplers.samplers[SAMPLER_LINEAR_WRAP];

    s->cs_count   = pipeline_create_compute(vk, "compiledshaders/sprite_cull.cs_count.comp.spv");
    s->cs_prefix  = pipeline_create_compute(vk, "compiledshaders/sprite_cull.cs_prefix.comp.spv");
    s->cs_compact = pipeline_create_compute(vk, "compiledshaders/sprite_cull.cs_compact.comp.spv");

    forEach(i, SPRITE_BLEND_COUNT) {
        GraphicsPipelineConfig cfg = pipeline_config_fullscreen();
        cfg.vert_path              = "compiledshaders/sprite.vert.spv";
        cfg.frag_path              = "compiledshaders/sprite.frag.spv";
        cfg.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        cfg.depth_test_enable      = false;
        cfg.depth_write_enable     = false;
        cfg.color_formats          = color_format;
        cfg.blends[0]              = sprite_blend_table[i];

        s->pipelines[i] = pipeline_create_graphics(vk, &cfg);
    }

    picture_system_init(s);

    log_info("[sprite] %u KB stream, %u KB compacted", (uint32_t)(stream_bytes / 1024),
             (uint32_t)(out_bytes / 1024));
}

static void sprite_system_destroy(SpriteSystem *s, VkBackend *vk) {
    wait_idle(vk);
    picture_system_destroy(s);
    destroy_buffer(vk, &s->stream);
    destroy_buffer(vk, &s->out);
    free(s->instances);
    free(s->sorted);
    free(s->slot_of);
    s->instances = NULL;
    s->sorted    = NULL;
    s->slot_of   = NULL;
}

/* --- pass -----------------------------------------------------------------
   Upload the CPU-built stream, cull and compact it on the GPU, then issue one
   indirect draw per batch. */

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

/* --- stage ---------------------------------------------------------------- */

TwoD *two_d_create(VkBackend *vk, const VkFormat *hdr_format) {
    TwoD *two_d = (TwoD *)malloc(sizeof(TwoD));
    if (!two_d) {
        log_fatal("[two_d] out of memory for the sprite system");
        exit(EXIT_FAILURE);
    }
    sprite_system_init(&two_d->sprites, vk, hdr_format);
    return two_d;
}

void two_d_destroy(TwoD *two_d) {
    sprite_system_destroy(&two_d->sprites, two_d->sprites.vk);
    free(two_d);
}

void two_d_render(TwoD *two_d, VkCommandBuffer cmd, RenderTarget *hdr, const float clear_color[4]) {
    sprite_flush(&two_d->sprites, cmd, hdr, LOAD_CLEAR, clear_color);
}

struct SpriteSystem *two_d_sprites(TwoD *two_d) { return &two_d->sprites; }

void two_d_stats(const TwoD *two_d, TwoDStats *out) {
    const SpriteSystem *s = &two_d->sprites;
    *out                 = (TwoDStats){.instances    = s->last_instances,
                                      .instance_cap = SPRITE_MAX_INSTANCES,
                                      .batches      = s->last_batches,
                                      .batch_cap    = SPRITE_MAX_BATCHES,
                                      .draws        = s->last_draws,
                                      .dropped      = s->dropped,
                                      .atlases      = s->atlas_count,
                                      .pictures     = s->picture_count,
                                      .camera_x     = s->origin_x,
                                      .camera_y     = s->origin_y,
                                      .zoom         = s->zoom};
}
