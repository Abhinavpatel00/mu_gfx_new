/* Sprite system bring-up: CPU scratch, the two device-local streams, the two
   storage-buffer bindings, and the seven pipelines. Included by renderer.c so
   it lives in the same translation unit as the rest of the backend wiring. */

#include "sprite.h"

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

void sprite_system_init(SpriteSystem *s, VkBackend *vk, const VkFormat *color_format) {
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

void sprite_system_destroy(SpriteSystem *s, VkBackend *vk) {
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
