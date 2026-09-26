#ifndef MU_SPRITE_DATA_H
#define MU_SPRITE_DATA_H

/* Sprite stream layout shared by the C flush path and the Slang cull/draw
   shaders. Every offset below is a byte offset into the sprite stream buffer
   and must be computed identically on both sides. */

#include "../slangtypes.h"

/* Instances are grouped into 256-sprite blocks and every batch is padded so it
   starts on a block boundary. That lets the GPU resolve a batch's output base
   from one block-level prefix sum with no second pass. */
#define SPRITE_BLOCK_SIZE      256u
#define SPRITE_MAX_INSTANCES   262144u
#define SPRITE_MAX_BATCHES     256u
#define SPRITE_BUFFER_CAPACITY (SPRITE_MAX_INSTANCES + SPRITE_MAX_BATCHES * SPRITE_BLOCK_SIZE)
#define SPRITE_MAX_ATLASES     4u
#define SPRITE_MAX_LAYERS      256u

#define SPRITE_INST_BYTES 32u

/* Sort key, most significant field first so a plain ascending sort gives
   layer / blend / sampler / atlas order: layer 0 and blend 0 (opaque) draw
   first and therefore sit behind everything else:
   [31:24] layer | [23:20] blend | [19:18] sampler | [17:14] atlas | [13:0] variant */
#define SPRITE_KEY_LAYER_SHIFT   24u
#define SPRITE_KEY_BLEND_SHIFT   20u
#define SPRITE_KEY_SAMPLER_SHIFT 18u
#define SPRITE_KEY_ATLAS_SHIFT   14u
#define SPRITE_KEY_LAYER_MASK    0xFF000000u

#define SPRITE_KEY(layer, blend, sampler, atlas, variant)                        \
    ((((uint)(layer)) << SPRITE_KEY_LAYER_SHIFT) |                               \
     (((uint)(blend)) << SPRITE_KEY_BLEND_SHIFT) |                               \
     (((uint)(sampler)) << SPRITE_KEY_SAMPLER_SHIFT) |                           \
     (((uint)(atlas)) << SPRITE_KEY_ATLAS_SHIFT) | ((uint)(variant)))

/* Sub-region offsets of one flush inside the stream buffer. `base` is the byte
   offset of the flush, `bc` the padded block count, `nb` the batch count. */
#define SPRITE_PADDED_COUNT(block_count) ((block_count)*SPRITE_BLOCK_SIZE)
#define SPRITE_VIS_OFF(base, block_count) ((base) + SPRITE_PADDED_COUNT(block_count) * SPRITE_INST_BYTES)
#define SPRITE_BLOCK_BASE_OFF(base, block_count) (SPRITE_VIS_OFF(base, block_count) + (block_count)*4u)
#define SPRITE_FBLOCK_OFF(base, block_count) (SPRITE_BLOCK_BASE_OFF(base, block_count) + ((block_count) + 1u) * 4u)
#define SPRITE_BATCH_BASE_OFF(base, block_count, batch_count) \
    (SPRITE_FBLOCK_OFF(base, block_count) + ((batch_count) + 1u) * 4u)
#define SPRITE_INDIRECT_OFF(base, block_count, batch_count) \
    (SPRITE_BATCH_BASE_OFF(base, block_count, batch_count) + (batch_count)*4u)
#define SPRITE_CPU_FLUSH_END(base, block_count, batch_count) \
    (SPRITE_INDIRECT_OFF(base, block_count, batch_count) + (batch_count)*16u)

/* Worst-case bytes a single flush needs beyond the instance data. */
#define SPRITE_SCRATCH_BYTES                                                       \
    ((SPRITE_BUFFER_CAPACITY / SPRITE_BLOCK_SIZE + 2u) * 8u +                      \
     (SPRITE_MAX_BATCHES + 1u) * 8u + SPRITE_MAX_BATCHES * 16u + 64u)

/* One sprite: 32 bytes, scalar fields only so SPIR-V needs no 16-bit cap. */
typedef struct {
    float x;
    float y;
    float w;
    float h;
    uint  uv0;   /* unorm16 u | unorm16 v, half-texel inset */
    uint  uv1;
    uint  color; /* linear RGBA8 */
    uint  key;   /* CPU sort key; never read by the GPU */
} SpriteInst;

/* One payload for every cull dispatch and draw. 32 bytes, no implicit padding
   in either dialect. Instances are stored in render-target pixel space, so the
   cull rectangle is always [0, 0, viewport]. */
typedef struct {
    uint  batch;      /* batch index for the draw, 0 for compute */
    uint  batch_count;
    uint  block_count;
    uint  cpu_off;    /* byte offset of this flush in sprite_stream */
    uint  gpu_off;    /* byte offset of this flush in sprite_out */
    uint  texture_id; /* bindless atlas slot */
    uint  sampler_id;
    float viewport[2];
} SpritePush;

#if defined(__STDC__)
_Static_assert(sizeof(SpriteInst) == SPRITE_INST_BYTES, "SpriteInst must stay 32 bytes");
_Static_assert(sizeof(SpritePush) % 4 == 0, "SpritePush must be a multiple of 4");
#endif

#endif
