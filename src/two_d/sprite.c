#include "sprite.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Open-addressed key -> slot map. The table is twice the batch cap, so a linear
   probe terminates quickly and the whole thing fits in L1. */
static uint32_t key_slot(SpriteSystem *s, uint32_t key) {
    uint32_t i = (uint32_t)((key * 2654435761u) >> 22) & (SPRITE_MAP_SIZE - 1u);
    for (;;) {
        uint32_t k = s->map_key[i];
        if (k == 0xFFFFFFFFu) {
            assert(s->slot_count < SPRITE_MAX_BATCHES && "sprite batch cap exceeded: raise SPRITE_MAX_BATCHES or reduce layers/samplers/atlases");
            s->map_key[i]               = key;
            s->map_slot[i]              = s->slot_count;
            s->keys[s->slot_count]      = key;
            s->counts[s->slot_count]    = 0;
            return s->slot_count++;
        }
        if (k == key)
            return s->map_slot[i];
        i = (i + 1u) & (SPRITE_MAP_SIZE - 1u);
    }
}

void sprite_begin(SpriteSystem *s, const SpriteCamera *camera) {
    s->count       = 0;
    s->batch_count = 0;

    float origin_x = camera->x;
    float origin_y = camera->y;
    if (camera->snap) {
        origin_x = floorf(origin_x);
        origin_y = floorf(origin_y);
    }

    s->origin_x = origin_x;
    s->origin_y = origin_y;
    s->zoom     = camera->zoom > 0.0f ? camera->zoom : 1.0f;
}

void sprite_push(SpriteSystem *s, SpriteDraw draw) {
    if (s->count >= SPRITE_MAX_INSTANCES) {
        s->dropped++;
        return;
    }
    assert(draw.picture < s->picture_count && "sprite_push with an unknown PictureId");
    if (draw.picture >= s->picture_count)
        return;

    const Picture *picture = &s->pictures[draw.picture];

    float w = draw.w != 0.0f ? draw.w : (float)picture->w;
    float h = draw.h != 0.0f ? draw.h : (float)picture->h;

    uint32_t blend = draw.blend < SPRITE_BLEND_COUNT ? draw.blend : SPRITE_BLEND_ALPHA;
    uint32_t sampler =
        draw.sampler == SPRITE_SAMPLER_DEFAULT ? picture->sampler : draw.sampler;
    if (sampler >= SPRITE_SAMPLER_COUNT)
        sampler = SPRITE_SAMPLER_LINEAR_CLAMP;
    uint32_t layer = draw.layer < SPRITE_MAX_LAYERS ? draw.layer : SPRITE_MAX_LAYERS - 1u;
    uint32_t color = draw.color ? draw.color : 0xFFFFFFFFu;

    SpriteInst *in = &s->instances[s->count++];
    in->x     = (draw.x - s->origin_x) * s->zoom;
    in->y     = (draw.y - s->origin_y) * s->zoom;
    in->w     = w * s->zoom;
    in->h     = h * s->zoom;
    in->uv0   = picture->uv0;
    in->uv1   = picture->uv1;
    in->color = color;
    in->key   = SPRITE_KEY(layer, blend, sampler, picture->atlas, 0);
}

/* Group the pushed sprites by key, order the groups by descending key, and lay
   them out on block boundaries. One hash pass, one gather, no data movement
   beyond the gather itself. */
static bool sprite_batch(SpriteSystem *s) {
    uint32_t n = s->count;

    memset(s->map_key, 0xFF, sizeof(s->map_key));
    s->slot_count = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot   = key_slot(s, s->instances[i].key);
        s->slot_of[i]   = (uint16_t)slot;
        s->counts[slot] += 1;
    }

    uint32_t order[SPRITE_MAX_BATCHES];
    for (uint32_t i = 0; i < s->slot_count; i++)
        order[i] = i;

    for (uint32_t i = 1; i < s->slot_count; i++) {
        uint32_t value = order[i];
        int32_t  j     = (int32_t)i - 1;
        while (j >= 0 && s->keys[order[j]] > s->keys[value]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = value;
    }

    uint32_t first = 0;
    for (uint32_t b = 0; b < s->slot_count; b++) {
        uint32_t slot   = order[b];
        uint32_t real   = s->counts[slot];
        uint32_t padded = (real + SPRITE_BLOCK_SIZE - 1u) & ~(SPRITE_BLOCK_SIZE - 1u);

        s->slot_to_batch[slot]                   = b;
        s->batches[b].key                        = s->keys[slot];
        s->batches[b].first                      = first;
        s->batches[b].count                      = padded;
        s->batches[b].real_count                 = real;
        s->first_block[b]                        = first / SPRITE_BLOCK_SIZE;
        first += padded;
    }
    s->first_block[s->slot_count] = first / SPRITE_BLOCK_SIZE;
    s->batch_count                = s->slot_count;

    uint32_t cursor[SPRITE_MAX_BATCHES];
    for (uint32_t b = 0; b < s->batch_count; b++)
        cursor[b] = s->batches[b].first;

    for (uint32_t i = 0; i < n; i++)
        s->sorted[cursor[s->slot_to_batch[s->slot_of[i]]]++] = s->instances[i];

    /* Padding instances must read as invisible: zero w/h so the cull drops them. */
    for (uint32_t b = 0; b < s->batch_count; b++) {
        uint32_t gap = s->batches[b].first + s->batches[b].real_count;
        uint32_t end = s->batches[b].first + s->batches[b].count;
        if (end > gap)
            memset(&s->sorted[gap], 0, (size_t)(end - gap) * sizeof(SpriteInst));
    }

    return first > 0;
}

bool sprite_prepare(SpriteSystem *s, SpriteLayout *out) {
    if (s->count == 0 || !sprite_batch(s))
        return false;

    uint32_t padded = s->first_block[s->batch_count] * SPRITE_BLOCK_SIZE;
    uint32_t blocks = s->first_block[s->batch_count];

    VkDeviceSize instance_bytes = (VkDeviceSize)padded * SPRITE_INST_BYTES;
    VkDeviceSize stream_bytes   = SPRITE_CPU_FLUSH_END(0, blocks, s->batch_count);

    uint32_t slot   = s->vk->current_frame;
    uint64_t frame  = s->vk->timeline_last_submitted;
    if (s->tail_frame[slot] != frame) {
        s->tail_stream[slot] = 0;
        s->tail_out[slot]    = 0;
        s->tail_frame[slot]  = frame;
    }

    if (s->tail_stream[slot] + stream_bytes > SPRITE_STREAM_REGION ||
        s->tail_out[slot] + instance_bytes > SPRITE_OUT_REGION) {
        s->dropped++;
        return false;
    }

    out->stream_base  = (VkDeviceSize)slot * SPRITE_STREAM_REGION + s->tail_stream[slot];
    out->out_base     = (VkDeviceSize)slot * SPRITE_OUT_REGION + s->tail_out[slot];
    out->stream_bytes = instance_bytes;
    out->block_count  = blocks;
    out->batch_count  = s->batch_count;
    out->padded_count = padded;

    s->tail_stream[slot] += stream_bytes;
    s->tail_out[slot] += instance_bytes;
    return true;
}
