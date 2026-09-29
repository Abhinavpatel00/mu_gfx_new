#include "scene.h"

#include <stdlib.h>
#include <string.h>

typedef struct InstanceSlot {
    bool     used;
    uint32_t generation;
} InstanceSlot;

static InstanceSlot *g_slots;
static uint32_t     g_capacity;
static uint32_t     g_allocated;

void scene_instances_init(uint32_t capacity) {
    g_capacity = capacity;
    g_slots = (InstanceSlot *)calloc(capacity, sizeof(InstanceSlot));
    g_allocated = 0;
}

void scene_instances_destroy(void) {
    free(g_slots);
    g_slots = NULL;
    g_capacity = 0;
    g_allocated = 0;
}

bool scene_instance_reserve(Scene *s, uint32_t count, uint32_t *out_first) {
    (void)s;
    if (count == 0 || !out_first || g_allocated + count > g_capacity)
        return false;

    uint32_t first = 0;
    uint32_t found = 0;
    for (uint32_t i = 0; i < g_capacity; i++) {
        if (!g_slots[i].used) {
            if (found == 0)
                first = i;
            found++;
            if (found == count) {
                for (uint32_t j = 0; j < count; j++) {
                    g_slots[first + j].used = true;
                    g_slots[first + j].generation++;
                }
                g_allocated += count;
                *out_first = first;
                return true;
            }
        } else {
            found = 0;
        }
    }
    return false;
}

bool scene_upload_instances(Scene *s, VkCommandBuffer cmd, ByteSpan updates) {
    const InstanceUpdate *u = (const InstanceUpdate *)updates.data;
    uint32_t count = updates.size / (uint32_t)sizeof(InstanceUpdate);
    if (!u || count == 0)
        return true;

    for (uint32_t i = 0; i < count; i++) {
        if (u[i].slot >= g_capacity)
            return false;
        BufferSlice dst = s->instance_slice;
        dst.offset += (VkDeviceSize)u[i].slot * sizeof(SceneGpuInstance);
        dst.size = sizeof(SceneGpuInstance);
        ByteSpan src = {.data = &u[i].data, .size = (uint32_t)sizeof(SceneGpuInstance)};
        if (!renderer_upload_buffer_to_slice(s->vk, cmd, dst, src))
            return false;
        cmd_buffer_barrier(cmd, dst.buffer, dst.offset, dst.size,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        uint32_t end = u[i].slot + 1;
        if (end > s->instance_count)
            s->instance_count = end;
        if (end > s->instance_total)
            s->instance_total = end;
    }

    s->last_submitted = s->instance_count;
    s->instance_upload_pending = true;
    return true;
}

void scene_instance_free(Scene *s, uint32_t first, uint32_t count) {
    (void)s;
    for (uint32_t i = 0; i < count; i++) {
        if (first + i < g_capacity && g_slots[first + i].used) {
            g_slots[first + i].used = false;
            g_allocated--;
        }
    }
}

