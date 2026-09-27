#ifndef MU_TWO_D_H
#define MU_TWO_D_H

/* The 2D stage: sprite system, its device streams, atlases and pipelines.
   Opaque to the core — renderer.c holds a TwoD * that stays NULL when the app
   asked for no 2D, so a 3D-only frame never brings any of it to life. */

#include <stdbool.h>
#include <stdint.h>

#include "../../vk.h"

typedef struct TwoD TwoD;
struct SpriteSystem;

/* hdr_format is the format of the colour target two_d_render draws into. */
TwoD *two_d_create(VkBackend *vk, const VkFormat *hdr_format);
void  two_d_destroy(TwoD *two_d);

/* Upload, GPU-cull and draw this frame's sprites into hdr, clearing it first. */
void two_d_render(TwoD *two_d, VkCommandBuffer cmd, RenderTarget *hdr, const float clear_color[4]);

/* For apps that build sprite draws (sprite_begin/sprite_push live in sprite.h). */
struct SpriteSystem *two_d_sprites(TwoD *two_d);

/* Debug-overlay counters, so the core's profiler never needs sprite.h. */
typedef struct TwoDStats {
    uint32_t instances;
    uint32_t instance_cap;
    uint32_t batches;
    uint32_t batch_cap;
    uint32_t draws;
    uint32_t dropped;
    uint32_t atlases;
    uint32_t pictures;
    float    camera_x;
    float    camera_y;
    float    zoom;
} TwoDStats;

void two_d_stats(const TwoD *two_d, TwoDStats *out);

#endif