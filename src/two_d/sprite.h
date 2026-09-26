#ifndef MU_SPRITE_H
#define MU_SPRITE_H

/* 2D sprite renderer.
 *
 * Flow per render target:
 *   sprite_begin(s, &camera);
 *   sprite_push(s, (SpriteDraw){...});        // 0..SPRITE_MAX_INSTANCES times
 *   sprite_flush(s, cmd, target, load, clear);
 *
 * sprite_flush sorts and batches on the CPU, uploads the sorted stream, then
 * culls and draws it with three compute dispatches and one indirect draw per
 * batch. Sprites are stored in render-target pixel space; the camera decides
 * how world coordinates land there.
 *
 * A flush uploads at most SPRITE_STREAM_REGION bytes of instances, so the
 * MAX_FRAMES_IN_FLIGHT regions hold one full-size flush each. Several smaller
 * flushes share a region until it is full. */

#include "../../vk.h"
#include "sprite_data.h"

#define SPRITE_MAX_PICTURES 4096u

/* Sentinel: take the sampler from the picture. */
#define SPRITE_SAMPLER_DEFAULT UINT32_MAX

typedef uint32_t PictureId;

typedef enum SpriteBlend {
    SPRITE_BLEND_OPAQUE = 0,
    SPRITE_BLEND_ALPHA,
    SPRITE_BLEND_ADD,
    SPRITE_BLEND_PREMULTIPLIED,
    SPRITE_BLEND_COUNT,
} SpriteBlend;

/* Dense sampler indices; they double as the sort-key sampler field. */
typedef enum SpriteSampler {
    SPRITE_SAMPLER_NEAREST_CLAMP = 0,
    SPRITE_SAMPLER_LINEAR_CLAMP,
    SPRITE_SAMPLER_NEAREST_WRAP,
    SPRITE_SAMPLER_LINEAR_WRAP,
    SPRITE_SAMPLER_COUNT,
} SpriteSampler;

/* Sub-rectangle of one atlas. uv0/uv1 are pre-quantised half-texel-inset
   unorm16 pairs, so a push costs no division. */
typedef struct Picture {
    uint32_t atlas;
    uint32_t sampler; /* SpriteSampler */
    uint32_t uv0;
    uint32_t uv1;
    uint32_t w;
    uint32_t h;
} Picture;

typedef struct SpriteAtlas {
    TextureID texture;
    uint32_t  width;
    uint32_t  height;
    uint32_t  cursor_x;     /* next free column of the open shelf */
    uint32_t  cursor_y;     /* top of the open shelf */
    uint32_t  shelf_height; /* height of the open shelf */
    bool      uploaded;     /* first upload has moved it out of UNDEFINED */
} SpriteAtlas;

/* A run of sprites sharing layer/blend/sampler/atlas. `first` and `count` are
   padded to SPRITE_BLOCK_SIZE so the GPU can index the batch by block. */
typedef struct SpriteBatch {
    uint32_t key;
    uint32_t first;
    uint32_t count;
    uint32_t real_count;
} SpriteBatch;

typedef struct SpriteCamera {
    float x;
    float y;    /* world coordinate of the top-left view corner */
    float zoom; /* screen pixels per world unit */
    bool  snap; /* quantise the origin to whole world units */
} SpriteCamera;

typedef struct SpriteDraw {
    PictureId picture;
    float     x;
    float     y;
    float     w;       /* 0 = picture width; negative mirrors in place */
    float     h;       /* 0 = picture height; negative mirrors in place */
    uint32_t  layer;   /* 0..255; 0 is drawn first, so it ends up behind */
    uint32_t  blend;   /* SpriteBlend */
    uint32_t  sampler; /* SpriteSampler, or SPRITE_SAMPLER_DEFAULT */
    uint32_t  color;   /* linear RGBA8, 0 = untinted */
} SpriteDraw;

/* Byte footprint of one flush inside each of the two device-local buffers. */
#define SPRITE_STREAM_REGION (SPRITE_BUFFER_CAPACITY * SPRITE_INST_BYTES + SPRITE_SCRATCH_BYTES)
#define SPRITE_OUT_REGION    (SPRITE_MAX_INSTANCES * SPRITE_INST_BYTES)

#define SPRITE_MAP_SIZE 1024u

typedef struct SpriteSystem {
    VkBackend *vk;

    /* CPU build state, rebuilt by every flush. */
    SpriteInst *instances;  /* pushed, unsorted */
    SpriteInst *sorted;     /* batch-ordered, padded to block boundaries */
    uint32_t    count;
    SpriteBatch batches[SPRITE_MAX_BATCHES];
    uint32_t    batch_count;
    uint32_t    first_block[SPRITE_MAX_BATCHES + 1]; /* uploaded to the GPU */
    uint16_t   *slot_of;                            /* per instance: key slot */

    /* Key -> slot map. Open addressed, capped at SPRITE_MAX_BATCHES entries. */
    uint32_t map_key[SPRITE_MAP_SIZE];
    uint32_t map_slot[SPRITE_MAP_SIZE];
    uint32_t keys[SPRITE_MAX_BATCHES];   /* distinct keys, insertion order */
    uint32_t counts[SPRITE_MAX_BATCHES]; /* sprites per key, insertion order */
    uint32_t slot_count;
    uint32_t slot_to_batch[SPRITE_MAX_BATCHES];

    /* Screen-space transform baked in by sprite_push. */
    float origin_x;
    float origin_y;
    float zoom;

    /* One device-local buffer each; MAX_FRAMES_IN_FLIGHT disjoint regions. */
    Buffer       stream; /* sorted instances + cull scratch + indirect commands */
    Buffer       out;    /* compacted stream read by the vertex shader */
    VkDeviceSize tail_stream[MAX_FRAMES_IN_FLIGHT];
    VkDeviceSize tail_out[MAX_FRAMES_IN_FLIGHT];
    uint64_t     tail_frame[MAX_FRAMES_IN_FLIGHT];

    PipelineID cs_count;
    PipelineID cs_prefix;
    PipelineID cs_compact;
    PipelineID pipelines[SPRITE_BLEND_COUNT];

    SpriteAtlas atlases[SPRITE_MAX_ATLASES];
    uint32_t    atlas_count;
    Picture    *pictures;
    uint32_t    picture_count;
    uint32_t    picture_capacity;

    uint32_t sampler_ids[SPRITE_SAMPLER_COUNT];

    /* Last flush, for the HUD. */
    uint32_t last_instances;
    uint32_t last_batches;
    uint32_t last_draws;
    uint32_t dropped;
} SpriteSystem;

/* Result of the CPU half of a flush; sprite_flush owns the region it names. */
typedef struct SpriteLayout {
    VkDeviceSize stream_base;
    VkDeviceSize out_base;
    VkDeviceSize stream_bytes; /* bytes of instance data at stream_base */
    uint32_t     block_count;
    uint32_t     batch_count;
    uint32_t     padded_count;
} SpriteLayout;

/* sprite_init.inl */
void sprite_system_init(SpriteSystem *s, VkBackend *vk, const VkFormat *color_format);
void sprite_system_destroy(SpriteSystem *s, VkBackend *vk);

/* picture.c */
void      picture_system_init(SpriteSystem *s);
void      picture_system_destroy(SpriteSystem *s);
PictureId picture_load(SpriteSystem *s, const char *path);
PictureId picture_load_rgba(SpriteSystem *s, const void *pixels, uint32_t width, uint32_t height);

/* sprite.c */
void sprite_begin(SpriteSystem *s, const SpriteCamera *camera);
void sprite_push(SpriteSystem *s, SpriteDraw draw);
bool sprite_prepare(SpriteSystem *s, SpriteLayout *out);

/* sprite_pass.inl */
void sprite_flush(SpriteSystem *s, VkCommandBuffer cmd, RenderTarget *target, LoadOp load, const float clear[4]);

#endif
