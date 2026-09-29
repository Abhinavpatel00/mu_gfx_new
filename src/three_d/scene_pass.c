#include "scene.h"
#include "scene_internal.h"

#include <stdlib.h>
#include <string.h>

static VkDeviceSize scene_hiz_dim(uint32_t src, uint32_t level) {
    uint32_t d = src >> (level + 1);
    return d ? d : 1u;
}

static bool scene_hiz_create(Scene *s, uint32_t src_w, uint32_t src_h) {
    uint32_t levels = 1;
    while (levels < SCENE_HIZ_MAX_LEVELS &&
           (scene_hiz_dim(src_w, levels - 1) != 1 || scene_hiz_dim(src_h, levels - 1) != 1))
        levels++;

    for (uint32_t i = 0; i < levels; i++) {
        RenderTargetSpec spec = {.width      = scene_hiz_dim(src_w, i),
                                 .height     = scene_hiz_dim(src_h, i),
                                 .layers     = 1,
                                 .format     = VK_FORMAT_R32_SFLOAT,
                                 .usage      = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                 .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .mip_count  = 1,
                                 .debug_name = "scene_hiz"};
        if (!rt_create(s->vk, &s->hiz[i], &spec)) {
            for (uint32_t j = 0; j < i; j++)
                rt_destroy(s->vk, &s->hiz[j]);
            s->hiz_levels = 0;
            return false;
        }
    }

    for (uint32_t i = 1; i < levels; i++) {
        if (s->hiz[i].bindless_index != s->hiz[0].bindless_index + i) {
            for (uint32_t j = 0; j < levels; j++)
                rt_destroy(s->vk, &s->hiz[j]);
            s->hiz_levels = 0;
            return false;
        }
    }

    s->hiz_levels = levels;
    s->hiz_src_w  = src_w;
    s->hiz_src_h  = src_h;
    s->hiz_warmup = 2;
    return true;
}

void scene_hiz_destroy(Scene *s) {
    for (uint32_t i = 0; i < s->hiz_levels; i++)
        rt_destroy(s->vk, &s->hiz[i]);
    s->hiz_levels = 0;
    s->hiz_src_w  = 0;
    s->hiz_src_h  = 0;
}

typedef struct RetiredHiz {
    RenderTarget targets[SCENE_HIZ_MAX_LEVELS];
    uint32_t     levels;
} RetiredHiz;

static void scene_hiz_retire_free(VkBackend *vk, void *user) {
    RetiredHiz *retired = (RetiredHiz *)user;
    for (uint32_t i = 0; i < retired->levels; i++)
        rt_destroy(vk, &retired->targets[i]);
    free(retired);
}

static void scene_hiz_retire(Scene *s) {
    if (s->hiz_levels == 0)
        return;
    RetiredHiz *retired = (RetiredHiz *)malloc(sizeof(RetiredHiz));
    memcpy(retired->targets, s->hiz, sizeof(s->hiz));
    retired->levels = s->hiz_levels;
    s->hiz_levels   = 0;
    s->hiz_src_w    = 0;
    s->hiz_src_h    = 0;
    memset(s->hiz, 0, sizeof(s->hiz));
    delete_queue_defer(s->vk, s->vk->timeline_last_submitted, scene_hiz_retire_free, retired);
}

void scene_hiz_build(Scene *s, VkCommandBuffer cmd, RenderTarget *src, uint32_t src_index, uint32_t level) {
    RenderTarget *dst = &s->hiz[level];

    rt_transition_all(s->vk, cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    rt_transition_all(s->vk, cmd, dst, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    flush_barriers(s->vk, cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->vk->render_pipelines.pipelines[s->pipeline_hiz - 1]);

    struct HizPush {
        uint32_t src;
        uint32_t dst;
        uint32_t src_w, src_h;
        uint32_t dst_w, dst_h;
        uint32_t pad[2];
    } push = {.src   = src_index,
              .dst   = dst->bindless_index,
              .src_w = src->width,
              .src_h = src->height,
              .dst_w = dst->width,
              .dst_h = dst->height};
    dispatch_push(s->vk, cmd, BYTE_SPAN(push), (dst->width + 7u) / 8u, (dst->height + 7u) / 8u, 1);
}

void scene_cull(Scene *s, VkCommandBuffer cmd, uint32_t view) {
    if (view >= s->view_count)
        return;

    bool hiz_on = s->occlusion_mode == OCCLUSION_HIZ_PREV_FRAME && s->hiz_warmup == 0 && s->hiz_disable_frames == 0 &&
                  s->hiz_levels > 0;
    uint32_t hiz_levels = hiz_on ? s->hiz_levels : 0;
    uint32_t hiz_base   = hiz_on ? s->hiz[0].bindless_index : 0;

    uint32_t mesh_count     = scene_mesh_count();
    uint32_t instance_count = s->instance_total;
    if (mesh_count == 0 || instance_count == 0)
        return;

    renderer_upload_buffer_to_slice(s->vk, cmd, s->view_slices[view],
                                    (ByteSpan){&s->views[view], sizeof(SceneGpuView)});
    cmd_buffer_barrier(cmd, s->view_slices[view].buffer, s->view_slices[view].offset,
                       sizeof(SceneGpuView), VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    uint32_t total_threads = instance_count * MAX_MESHES_PER_SET;
    uint32_t groups        = (total_threads + 63u) / 64u;

    SceneViewPush push = {
        .view       = scene_slice_addr(s, s->view_slices[view]),
        .meshes     = scene_slice_addr(s, s->mesh_slice),
        .skin_table = scene_slice_addr(s, s->skin_slice),
        .misc       = (hiz_base & 0xFFFF) | ((hiz_levels & 0xFF) << 8),
    };

    BufferSlice inst_read = s->instance_slice;
    inst_read.size        = (VkDeviceSize)instance_count * sizeof(SceneGpuInstance);
    BufferSlice mesh_read = s->mesh_slice;
    mesh_read.size        = (VkDeviceSize)mesh_count * sizeof(SceneGpuMesh);
    BufferSlice set_read  = s->mesh_set_slice;
    set_read.size         = (VkDeviceSize)scene_mesh_set_count() * sizeof(SceneMeshSet);
    BufferSlice lod_read  = s->lod_slice;
    lod_read.size         = (VkDeviceSize)mesh_count * SCENE_MAX_LOD_LEVELS * sizeof(SceneLodRow);
    BufferSlice cmd_write = s->command_slices[view];
    BufferSlice vis_write = s->visible_slices[view];
    vis_write.size        = (VkDeviceSize)SCENE_MAX_COMMANDS * sizeof(uint32_t);
    BufferSlice cnt_write = s->count_slices[view];
    BufferSlice ctr_write = s->counter_slice;
    ctr_write.size        = 4 * sizeof(uint32_t);

    BufferAccess reads[] = {
        {inst_read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        {mesh_read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        {set_read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        {lod_read, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        {s->view_slices[view], VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
    };
    BufferAccess writes[] = {
        {cmd_write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
        {vis_write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
        {cnt_write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
        {ctr_write, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
    };

    begin_pass(s->vk, cmd,
               &(PassDesc){.color_count     = 0,
                           .pipeline        = s->pipeline_cull,
                           .push            = BYTE_SPAN(push),
                           .buf_reads       = reads,
                           .buf_read_count  = 5,
                           .buf_writes      = writes,
                           .buf_write_count = 4});
    vkCmdDispatch(cmd, groups, 1, 1);

    cmd_buffer_barrier(cmd, s->counter_slice.buffer, s->counter_slice.offset, 8 * sizeof(uint32_t),
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    cmd_buffer_barrier(cmd, s->command_slices[view].buffer, s->command_slices[view].offset, 2 * sizeof(SceneGpuDraw),
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    cmd_buffer_barrier(cmd, s->visible_slices[view].buffer, s->visible_slices[view].offset, 8 * sizeof(uint32_t),
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    uint32_t     slot       = s->vk->current_frame % MAX_FRAMES_IN_FLIGHT;
    VkBufferCopy regions[3] = {
        {.srcOffset = s->counter_slice.offset, .dstOffset = 0, .size = 8 * sizeof(uint32_t)},
        {.srcOffset = s->command_slices[view].offset, .dstOffset = 32, .size = 2 * sizeof(SceneGpuDraw)},
        {.srcOffset = s->visible_slices[view].offset, .dstOffset = 96, .size = 8 * sizeof(uint32_t)},
    };
    vkCmdCopyBuffer(cmd, s->counter_slice.buffer, s->readback[slot].buffer, 1, &regions[0]);
    vkCmdCopyBuffer(cmd, s->command_slices[view].buffer, s->readback[slot].buffer, 1, &regions[1]);
    vkCmdCopyBuffer(cmd, s->visible_slices[view].buffer, s->readback[slot].buffer, 1, &regions[2]);
    cmd_buffer_barrier(cmd, s->count_slices[view].buffer, s->count_slices[view].offset, sizeof(uint32_t),
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    cmd_buffer_barrier(cmd, s->command_slices[view].buffer, s->command_slices[view].offset,
                       s->command_slices[view].size, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                       VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    cmd_buffer_barrier(cmd, s->visible_slices[view].buffer, s->visible_slices[view].offset,
                       s->visible_slices[view].size, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    cmd_buffer_barrier(cmd, s->readback[slot].buffer, 0, 128, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}

void scene_draw(Scene *s, VkCommandBuffer cmd, RenderTarget *color, RenderTarget *depth, uint32_t view) {
    if (view >= s->view_count)
        return;
    if (scene_mesh_count() == 0 || s->instance_total == 0)
        return;

    if (s->occlusion_mode == OCCLUSION_OFF) {
        scene_hiz_retire(s);
    } else if (s->hiz_levels == 0 || s->hiz_src_w != depth->width || s->hiz_src_h != depth->height) {
        scene_hiz_retire(s);
        scene_hiz_create(s, depth->width, depth->height);
    }
    forEach(v, SCENE_MAX_VIEWS) {
        s->views[v].hiz_size[0] = s->hiz_levels ? s->hiz[0].width : 0;
        s->views[v].hiz_size[1] = s->hiz_levels ? s->hiz[0].height : 0;
    }

    PassAttachment color_a = {.target = color, .load = LOAD_KEEP, .store = STORE_KEEP};
    PassAttachment depth_a = {.target = depth, .load = LOAD_CLEAR, .store = STORE_KEEP};
    depth_a.clear[0]       = 0.0f;

    SceneViewPush push = {
        .view       = scene_slice_addr(s, s->view_slices[view]),
        .meshes     = scene_slice_addr(s, s->mesh_slice),
        .skin_table = scene_slice_addr(s, s->skin_slice),
        .misc       = 0,
    };

    BufferSlice index_pool = scene_index_pool();
    BufferSlice mesh_read  = s->mesh_slice;
    mesh_read.size         = (VkDeviceSize)scene_mesh_count() * sizeof(SceneGpuMesh);
    BufferSlice skin_table_read = s->skin_slice;
    skin_table_read.size        = (VkDeviceSize)scene_mesh_count() * sizeof(SceneSkinRecord);
    BufferSlice inst_read  = s->instance_slice;
    inst_read.size         = (VkDeviceSize)s->instance_total * sizeof(SceneGpuInstance);
    BufferSlice mat_read   = s->material_slice;
    BufferSlice vis_read   = s->visible_slices[view];
    vis_read.size          = (VkDeviceSize)SCENE_MAX_COMMANDS * sizeof(uint32_t);
    BufferSlice  pal_read[8];
    uint32_t     pal_count = scene_skin_palette_slices(pal_read, 8);
    BufferSlice skin_read[SCENE_MAX_MESH_SLOTS];
    uint32_t skin_count = scene_skin_stream_slices(skin_read, SCENE_MAX_MESH_SLOTS);
    BufferSlice vertex_read[SCENE_MAX_MESH_SLOTS];
    uint32_t vertex_count = scene_vertex_stream_slices(vertex_read, SCENE_MAX_MESH_SLOTS);
    BufferAccess reads[8 + 8 + SCENE_MAX_MESH_SLOTS * 2] = {
        {s->command_slices[view], VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT},
        {s->count_slices[view], VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT},
        {vis_read, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        {inst_read, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        {mesh_read, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        {skin_table_read, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        {index_pool, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT},
        {mat_read, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
    };
    uint32_t read_count = 8;
    for (uint32_t i = 0; i < pal_count; i++) {
        reads[read_count++] =
            (BufferAccess){pal_read[i], VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    }
    for (uint32_t i = 0; i < skin_count; i++) {
        reads[read_count++] =
            (BufferAccess){skin_read[i], VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    }
    for (uint32_t i = 0; i < vertex_count; i++) {
        reads[read_count++] =
            (BufferAccess){vertex_read[i], VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    }

    begin_pass(s->vk, cmd,
               &(PassDesc){.colors         = &color_a,
                           .color_count    = 1,
                           .depth          = &depth_a,
                           .pipeline       = s->pipeline_scene,
                           .push           = BYTE_SPAN(push),
                           .buf_reads      = reads,
                           .buf_read_count = read_count});
    vkCmdBindIndexBuffer(cmd, index_pool.buffer, index_pool.offset, VK_INDEX_TYPE_UINT16);

    BufferSlice commands = s->command_slices[view];
    BufferSlice count    = s->count_slices[view];
    cmd_draw_indexed_indirect_count(s->vk, cmd, BYTE_SPAN(push), commands, count, SCENE_MAX_COMMANDS,
                                    sizeof(SceneGpuDraw));

    end_pass(cmd);

    if (s->hiz_levels) {
        scene_hiz_build(s, cmd, depth, depth->bindless_index, 0);
        for (uint32_t i = 1; i < s->hiz_levels; i++)
            scene_hiz_build(s, cmd, &s->hiz[i - 1], s->hiz[i - 1].bindless_index, i);
        for (uint32_t i = 0; i < s->hiz_levels; i++)
            rt_transition_all(s->vk, cmd, &s->hiz[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        flush_barriers(s->vk, cmd);
        if (s->hiz_warmup)
            s->hiz_warmup--;
    }
}

void scene_frame_begin(Scene *s, VkCommandBuffer cmd) {
    s->instance_count          = 0;
    s->view_count              = 0;
    s->instance_upload_pending = false;
    if (s->hiz_disable_frames > 0)
        s->hiz_disable_frames--;

    /* Frames in flight overlap: the previous frame's vertex/compute/indirect
       reads of the count, palette, view, and instance buffers may still be
       executing when this frame's transfer writes below begin. Submission
       order adds no memory dependency, so fence every prior read (and prior
       write) before this frame's first transfer write. dst=TRANSFER orders
       all subsequent copies in this command buffer, not just these zeros. */
    cmd_memory_barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    for (uint32_t i = 0; i < SCENE_MAX_VIEWS; i++) {
        uint32_t zero = 0;
        renderer_upload_buffer_to_slice(s->vk, cmd, s->count_slices[i], (ByteSpan){&zero, sizeof(zero)});
    }
    uint32_t czero[8] = {0};
    renderer_upload_buffer_to_slice(s->vk, cmd, s->counter_slice, (ByteSpan){&czero, sizeof(czero)});
}

void scene_view_set(Scene *s, uint32_t view, const SceneViewDesc *desc) {
    if (view >= SCENE_MAX_VIEWS)
        return;
    if (!desc)
        return;

    memcpy(s->views[view].rows, desc->rows, sizeof(desc->rows));
    memcpy(s->views[view].sun, desc->sun, sizeof(desc->sun));
    s->views[view].hiz_size[0]   = s->hiz_levels ? s->hiz[0].width : 0;
    s->views[view].hiz_size[1]   = s->hiz_levels ? s->hiz[0].height : 0;
    s->views[view].caps          = desc->caps;
    s->views[view].draws         = scene_slice_addr(s, s->command_slices[view]);
    s->views[view].visible       = scene_slice_addr(s, s->visible_slices[view]);
    s->views[view].instances     = scene_slice_addr(s, s->instance_slice);
    s->views[view].mesh_sets     = scene_slice_addr(s, s->mesh_set_slice);
    s->views[view].lod_table     = scene_slice_addr(s, s->lod_slice);
    s->views[view].command_count = scene_slice_addr(s, s->count_slices[view]);
    s->views[view].counters      = scene_slice_addr(s, s->counter_slice);
    s->views[view].materials     = scene_slice_addr(s, s->material_slice);
    s->views[view].sampler       = s->sampler;
    s->views[view].lod_target    = s->lod_target;
    s->views[view].draw_count    = 0;

    if (view >= s->view_count)
        s->view_count = view + 1;
}

void scene_debug_dump(Scene *s) {
    uint32_t       slot = (s->vk->current_frame + MAX_FRAMES_IN_FLIGHT - 2) % MAX_FRAMES_IN_FLIGHT;
    const uint8_t *raw  = (const uint8_t *)s->readback[slot].mapping;
    if (!raw)
        return;
    const uint32_t *c    = (const uint32_t *)(raw + 0);
    const uint32_t *cmd0 = (const uint32_t *)(raw + 32);
    const uint32_t *cmd1 = (const uint32_t *)(raw + 64);
    const uint32_t *vis  = (const uint32_t *)(raw + 96);
    fprintf(stderr,
            "[cull] frustum=%u hiz=%u dropped=%u drawn=%u | cmd0={idx=%u inst=%u first=%u voff=%u base=%u id=%u} "
            "cmd1={idx=%u inst=%u first=%u} vis0=%08x vis1=%08x vis2=%08x vis3=%08x\n",
            c[0], c[1], c[2], c[3], cmd0[0], cmd0[1], cmd0[2], cmd0[3], cmd0[4], cmd0[5], cmd1[0], cmd1[1], cmd1[2],
            vis[0], vis[1], vis[2], vis[3]);
}

bool scene_counters_read(Scene *s, SceneCounters *out) {
    if (!out)
        return false;

    uint32_t        slot = (s->vk->current_frame + MAX_FRAMES_IN_FLIGHT - 2) % MAX_FRAMES_IN_FLIGHT;
    const uint32_t *raw  = (const uint32_t *)s->readback[slot].mapping;
    if (!raw)
        return false;

    out->submitted        = s->last_submitted;
    out->culled_frustum   = raw[0];
    out->culled_hiz       = raw[1];
    out->dropped_commands = raw[2];
    out->drawn            = raw[3];
    out->commands         = raw[3];
    out->lod[0]           = raw[4];
    out->lod[1]           = raw[5];
    out->lod[2]           = raw[6];
    out->lod[3]           = raw[7];
    return true;
}

void scene_set_occlusion(Scene *s, OcclusionMode mode) { s->occlusion_mode = mode; }

void scene_disable_occlusion(Scene *s, uint32_t frames) { s->hiz_disable_frames = frames; }

void scene_set_lod_target(Scene *s, float target) { s->lod_target = target; }
