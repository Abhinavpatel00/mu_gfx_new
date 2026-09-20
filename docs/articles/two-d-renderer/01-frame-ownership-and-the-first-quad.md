# 01 — Frame ownership and the first quad

> **Why this article exists.** Ninety percent of "my 2D renderer is slow / flickers /
> corrupts" bugs come from not knowing who owns which bytes while three frames are in
> flight. Before a single sprite is drawn, we establish the ownership rules from the
> source, then prove them with one quad on screen.

**What you have at the end.** `pass_sprites()` recorded between `update_global_data`
and `post_pass`, drawing one quad into `hdr_color`, visible in the GPU profiler.

---

## 1.1 The three-frame photograph

`MAX_FRAMES_IN_FLIGHT` is 3 (`src/constant.h:18`). `frame_start` advances
`r->vk.current_frame` *before* acquiring (`renderer.c:991`). At any instant:

```
slot 0: CPU is writing it           (prepare)
slot 1: GPU is executing it          (in flight)
slot 2: GPU finished; timeline not yet observed (retired)
```

Read `vk_frame_acquire` (`vk.c:3176`) as a contract. It does, in this exact order:

1. **Waits** on `frames[slot].timeline_value` — the submission that last used *this
   slot's* command buffer and upload buffers. This is the only thing that makes slot
   reuse safe.
2. `buffer_pool_linear_reset(&r->cpu_pool)` — the 32 MB host-visible linear pool is
   wiped for you every time a slot comes back around.
3. `buffer_pool_ring_free_to(&r->staging_pool, f->staging_tail)` — staging space
   consumed by the frame that owned this slot is reclaimed.
4. `gpu_profiler_collect` — GPU timings become readable.
5. `vkResetCommandPool` — the slot's command buffer is recycled wholesale.
6. `vk_swapchain_acquire`.

And `vk_frame_submit` (`vk.c:3208`) does the symmetric work: it stores the staging
ring head into `f->staging_tail`, bumps `timeline_last_submitted`, and signals the
timeline with that value. Everything that needs "after the GPU is done with frame N"
— slot reuse, `delete_queue_tick`, capture readback — keys on that one number.

Turn that into rules you are not allowed to break:

| You want to | You must do it this way | Because |
|---|---|---|
| Write per-frame CPU data (instance bytes) | `buffer_pool_alloc(&r->vk.cpu_pool, ...)` | reset per slot in `vk_frame_acquire`; sequential writes; no frees |
| Write per-frame GPU-only data | upload through `staging_pool`, or have a compute pass write `gpu_pool` | `gpu_pool` is `VMA_MEMORY_USAGE_GPU_ONLY` (`vk.c:3159`) |
| Keep a buffer alive across frames | one `Buffer` **per slot**, like `NuklearUi::uploads[MAX_FRAMES_IN_FLIGHT]` (`renderer.c:50`) | a single shared buffer is overwritten while the GPU reads it |
| Destroy/rebuild a GPU resource | `delete_queue_defer(r, r->vk.timeline_last_submitted, fn, user)` | in-flight frames may still reference it |
| Read back GPU output | wait the timeline value, like `capture_consume` (`renderer.c:319`) | matching `vkQueueSubmit` is not completion |

> **The one bug everyone writes.** A single upload buffer, one `memcpy` per frame,
> pointing the vertex fetch at it. It looks correct at 30 fps and tears at 200 fps,
> because "one buffer" means slot 1 is being overwritten while the GPU is still
> reading slot 0's data through the same allocation. Per-slot buffers exist for
> exactly this reason; the comment at `src/nuklear_pass.inl:27` is the receipt.

---

## 1.2 Choose the target, then clear it

Nothing writes `hdr_color` in `renderer_frame` today (`pass_fire` is defined at
`renderer.c:1125` and never called), while `post_pass` reads it. So the sprite pass
is the scene pass, and it must clear:

* Target: `&r->hdr_color[r->vk.swapchain.current_image]` — `R16G16B16A16_SFLOAT`
  (`renderer.c:481`), with `STORAGE | SAMPLED | COLOR_ATTACHMENT`.
* `LOAD_CLEAR` with the sky color, `STORE_KEEP`.
* No depth attachment. 2D ordering is painter's order, elaborated in article 03.

Why HDR rather than drawing straight to the swapchain: `post_pass` runs a compute
tonemapper with `exposure = 1.2` (`renderer.c:1111`) and `pass_smaa` follows. Drawing
into `hdr_color` means sprites get tonemapping, SMAA, screenshots and video capture
for free, and your color math happens in linear space. Author sprite colors in
**linear**; if your art pipeline hands you sRGB, convert once at pack time (article
02), never per pixel.

## 1.3 The pass layer's job — and what it does *not* do

`begin_pass` (`vk.c:2455`) resolves more than the boilerplate summary suggests:

1. Derives the render area from the first target (or the swapchain extent).
2. Calls `pass_transition_attachment` on every attachment. For a target with
   `VK_IMAGE_ASPECT_DEPTH_BIT` it picks depth-attachment layout and
   early+late fragment test stages; otherwise color-attachment layout and
   `COLOR_ATTACHMENT_OUTPUT`.
3. Transitions `shader_reads` to `SHADER_READ_ONLY_OPTIMAL` (fragment **and** compute
   stages) and `shader_writes` to `GENERAL`.
4. `flush_barriers` — one `vkCmdPipelineBarrier2` for the whole batch
   (`BarrierBatch` holds 64 image barriers, `vk.h:380`).
5. `vkCmdBeginRendering`, then `vk_cmd_set_viewport_scissor` with the standard
   negative-height viewport (`vk.c:2534`).
6. `vkCmdBindPipeline` if `desc->pipeline != 0`.

Skips live inside `rt_transition_all` (`vk.c:2385`): an `ImageState` whose
stage/access/layout already match produces no barrier. This is why you should *not*
be clever and set up target state yourself.

**What it does not do:** there is no descriptor binding in `begin_pass`. Set-0 bindless
is bound once per frame in `renderer_frame` (`renderer.c:1418-1422`). You never bind
---

## 1.4 The public API we commit to in this article

Two headers, one source file, one `.inl`. Everything the game is allowed to see:

```c
/* src/two_d/sprite.h */
#ifndef MU_SPRITE_H
#define MU_SPRITE_H

#include <stdbool.h>
#include "../vk.h"
#include "../src/helpers.h"

typedef uint32_t PictureID;

typedef struct SpriteRenderer SpriteRenderer;

/* Renderer-owned. A failure here is a programming error, so it asserts loudly. */
bool sprite_system_init(SpriteRenderer *sprites, VkBackend *vk);
void sprite_system_shutdown(SpriteRenderer *sprites);

/* Between these two calls the game may only append sprites. No reads, no frees. */
void sprite_frame_begin(SpriteRenderer *sprites, VkBackend *vk);
void sprite_frame_end(SpriteRenderer *sprites);

/* One call per sprite, in draw order. Article 03 shows why the game should not
   actually be calling this in order, and what it calls instead. */
void sprite_append(SpriteRenderer *sprites, float x, float y, float w, float h,
                   uint32_t rgba_linear, PictureID picture, uint32_t layer);

/* Called from renderer_frame between update_global_data and post_pass. */
void pass_sprites(SpriteRenderer *sprites, VkBackend *vk, VkCommandBuffer cmd);

/* Counters, recomputed by the tail of the pass, one submission behind. */
uint32_t sprite_predicted_count(const SpriteRenderer *sprites); /* CPU estimate */
uint32_t sprite_submitted_count(const SpriteRenderer *sprites); /* GPU truth */
uint32_t sprite_draw_call_count(const SpriteRenderer *sprites);

#endif
```

Three deliberate properties:

* **No allocation API.** The game cannot ask for an "instance buffer". The only way in
  is `sprite_append`, so the renderer owns the buffer, the capacity and the layout.
* **No GPU types.** No `VkBuffer`, no layouts, no semaphores. Callers name intent.
* **Counts, not accessors.** `sprite_predicted_count` is a *prediction*;
  the authoritative number comes from the GPU and lags by one frame. Making that
  distinction explicit in the API is what keeps article 04 an optimization instead of a
  redesign: nothing on the CPU may depend on a decision the GPU has not made yet.

For article 01 most of this is scaffolding. `pass_sprites` ignores the count and draws
one quad — deliberately. Pipeline, pass wiring and profiler scope get proven before any
data plumbing exists. Build the pipe before the water.

---

## 1.5 Shared data, declared once

Follow `src/slangtypes.h`: one header, two dialects, selected by `__STDC__` vs
`__SLANG__`. Create `src/two_d/sprite_data.h`.

```c
/* src/two_d/sprite_data.h */
#ifndef MU_SPRITE_DATA_H
#define MU_SPRITE_DATA_H

#define SPRITE_FLIP_X  (1u << 0)
#define SPRITE_FLIP_Y  (1u << 1)
#define SPRITE_TINTED  (1u << 2)

#if defined(__STDC__)

#include <stdint.h>

/* Hot record. Both the CPU culler and the vertex shader read every field. */
typedef struct ALIGNAS(16) SpriteInst {
    float    x, y, w, h;
    uint32_t color;   /* linear RGBA8 */
    uint32_t picture; /* bindless sampled-image slot */
    uint32_t layer;
    uint32_t flags;
} SpriteInst;

/* Cold per-batch record: stays in the picture table, copied into the batch. */
typedef struct SpriteAnim {
    float u0, v0, u1, v1;
} SpriteAnim;

typedef struct SpritePush {
    VkDeviceAddress   instances; /* gpu_pool base address + slice offset */
    VkDeviceAddress   animations;
    uint32_t          base;      /* first instance this batch draws */
    uint32_t          sampler;   /* bindless sampler slot */
    float             view[4];   /* x0, y0, x1, y1 world rect */
    float             viewport[2];
    float             texel[2];  /* 1/texture_w, 1/texture_h */
    uint32_t          flags;
    uint32_t          pad;
} SpritePush;

typedef struct SpritesDrawBatch {
    SpriteAnim anim;
    uint32_t   first;
    uint32_t   count;
    uint32_t   picture;
    uint32_t   blend;
} SpritesDrawBatch;

#elif defined(__SLANG__)

struct SpriteInst {
    float2 pos;
    float2 size;
    uint   color;
    uint   picture;
    uint   layer;
    uint   flags;
};

struct SpriteAnim {
    float4 uv;
};

struct SpritePush {
    SpriteInst* instances;
    SpriteAnim* animations;
    uint        base;
    uint        sampler;
    float4      view;
    float2      viewport;
    float2      texel;
    uint        flags;
    uint        pad;
};

#endif
#endif
```

Two declaration rules that are not optional, both learned from the SPIR-V output:

* `sizeof(SpriteInst) == 32`: 16 bytes of rect plus four `uint32_t`. Article 02 spends
  real effort fighting this number down; read that before you "optimize" it. For now
  note what the layout buys you: every member is at most 8-byte aligned, so C's
  natural layout and Slang's std430 layout agree member for member (`pos` 0, `size` 8,
  `color` 16, `picture` 20, `layer` 24, `flags` 28 — exactly the offsets
  `spirv-dis` printed for the test kernel). A stride of 32 is a shift, not a
  multiply, in `instances[i]`, and one instance spans at most two 64-byte cache lines
  with no straddling of the second unless it starts above offset 32 in a line.
  `ALIGNAS(16)` is not required by any of that; it is there so a future SIMD
  preparation path can load one instance with two 16-byte moves, and so the compiler
  cannot "helpfully" pack arrays tightly if the struct grows a smaller member later.
* `float view[4]` **not** `vec4 view` / `float4 view`. Measured, both dialects:
  with `float view[4] float viewport[2] float texel[2]`, Slang's std430 push-constant
  offsets are `0,8,12,16,20,36,44,52` and C's are `p=0 a=8 b=12 c=16 v=20 p2=36 t=44
  f=52` — identical, byte for byte. Declared as `float4 v; float2 p2;`, the same struct
  gets `20→32` and `36→48`: Slang applies 16-/8-byte alignment to vector types, C does
  not (and with `CGLM_ALL_UNALIGNED` at `common.h:3`, cglm's `vec4` would sit at 24 in a
  field order that puts it there). The rule that follows is mechanical: **in a struct the
  shader also sees, write every vector-shaped member as a fixed-size `float[N]` array.**
  Keep cglm types in CPU-only structs and copy across explicitly if you want them.

The `PUSH_CONSTANT` macro (`common.h:37`) is what enforces the C side: it declares
`SpritePush_init` with your body, computes `256 - sizeof(body)` as an enum, appends
that many `uint8_t` of padding, and closes with
`_Static_assert(sizeof(SpritePush) == 256, "Push constant != 256")`. Declare it once in
`sprite_init.inl`:

```c
PUSH_CONSTANT(SpritePush,
              VkDeviceAddress instances; VkDeviceAddress animations; uint32_t base;
              uint32_t sampler; float view[4]; float viewport[2]; float texel[2];
              uint32_t flags; uint32_t pad;);
```

The macro cannot live in a header the Slang compiler also reads, so the two
declarations are inevitably duplicated. Article 02 removes the risk with a
compile-time offset check plus one `spirv-dis` run in the CI-style test. Until then:
**treat the two declarations as one object that happens to be typed twice.**

---

## 1.6 The shader: no vertex buffer, no index buffer

`sprite.slang` generates its quad from `SV_VertexID`. There is no vertex buffer, no
index buffer and no input assembly cache traffic beyond four invocations:

```slang
#include "common.slang"
#include "../src/two_d/sprite_data.h"

[[vk::push_constant]] SpritePush pc;

struct SpriteVarying {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

/* Corner order (0,0) (1,0) (0,1) (1,1): two triangles for TRIANGLE_STRIP. */
static float2 quad_corner(uint id) {
    return float2(float(id & 1u), float((id >> 1u) & 1u));
}

[shader("vertex")]
SpriteVarying vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    SpriteInst s = pc.instances[pc.base + iid];

    float2 world = float2(s.pos.x + s.size.x * quad_corner(vid).x,
                          s.pos.y + s.size.y * quad_corner(vid).y);

    /* Packed colors arrive as linear RGBA8; 255.0 is exact in fp32, so unpacking is
       a convert + multiply, and the compiler keeps the packed value in a register. */
    float4 color = float4(float(s.color & 255u), float((s.color >> 8u) & 255u),
                          float((s.color >> 16u) & 255u), float(s.color >> 24u)) * (1.0 / 255.0);

    float2 uv = quad_corner(vid); /* article 01: the quad IS the sprite */
    SpriteVarying o;
    o.position = float4(world, 0.0, 1.0);
    o.uv       = uv;
    o.color    = color;
    return o;
}

[shader("fragment")]
float4 fs_main(SpriteVarying i) : SV_Target0 {
    return i.color * textures[i.picture].Sample(samplers[pc.sampler], i.uv);
}
```

Three things to notice, each of which becomes an article later:

* **`world` is written straight to clip space as `float4(world, 0, 1)`.** A 2D scene
  needs no matrix: the pixel-to-NDC mapping is a scale and a translate, and the
  negative-height viewport (`vk.c:2047-2060`) already flips Y for screen-space
  thinking. Doing this in the vertex shader costs two FMAs. Doing it on the CPU costs
  2.4 MB/frame of writes plus a per-instance matrix multiply (article 06). This only
  works because article 03 batches by *state*, not by transform.
* **`pc.base + iid` rather than `firstInstance`.** `vkCmdDraw` can carry a nonzero
  `firstInstance`, but indirect draws require the `drawIndirectFirstInstance` feature
  for that, and this backend does not enable it (`apply_caps`, `vk.c:283-315`; the
  feature is absent from the list). Keeping the base in the root payload means every
  indirect command can legally carry `firstInstance = 0`, so the CPU batching code and
  the GPU-driven code in article 04 share one shader.
* **`pc.animations` is in the struct but not read yet.** In article 01 the push value is
  `0`, i.e. a null device address. Nothing dereferences it, which is the *only* reason
  that is not a bug. Dereferencing address 0 is a validation error and a GPU fault, and
  because it lives in a root payload the validation layer cannot see it statically. The
  rule this series follows from here on: **every device address in a root payload is
  either valid for the whole pass or explicitly flag-disabled.**

---

## 1.7 State: one struct, four decisions

Everything the sprite system owns lives in one struct, visible only inside
`renderer.c`'s translation unit. Create `src/two_d/sprite_init.inl`:

```c
/* src/two_d/sprite_init.inl — included by renderer.c, like src/nuklear_pass.inl */

#define SPRITE_MAX_INSTANCES (1u << 20) /* 1 M sprites, 32 MB of instance payload */
#define SPRITE_MAX_BATCHES   512

typedef enum SpriteBlend {
    SPRITE_BLEND_OPAQUE = 0, /* blend_disabled(): writes over the target */
    SPRITE_BLEND_ALPHA,      /* blend_alpha(): src_alpha / 1-src_alpha */
    SPRITE_BLEND_ADD,        /* additive: light, fire, glow */
    SPRITE_BLEND_COUNT,
} SpriteBlend;

struct SpriteRenderer {
    VkBackend    *vk;
    RenderTarget *targets;      /* borrowed from Renderer: &r->hdr_color[0] */
    uint32_t      target_count; /* == swapchain.image_count at init */

    SpriteInst   *instances;    /* CPU staging, append-only within a frame */
    uint32_t      count;
    Buffer        gpu_instances[MAX_FRAMES_IN_FLIGHT]; /* one per in-flight frame */

    PipelineID    pipeline[SPRITE_BLEND_COUNT];
    uint32_t      draw_calls;   /* recorded this frame */
    uint32_t      submitted;    /* authoritative, one submission behind */

    bool          inited;
};

PUSH_CONSTANT(SpritePush,
              VkDeviceAddress instances; VkDeviceAddress animations; uint32_t base;
              uint32_t sampler; float view[4]; float viewport[2]; float texel[2];
              uint32_t flags; uint32_t pad;);

_Static_assert(sizeof(SpriteInst) == 32, "SpriteInst layout drifted from sprite_data.h");
_Static_assert(sizeof(SpritesDrawBatch) == 32, "batch record must stay 32 B");
```

Four decisions worth defending:

1. **`targets` is a borrowed array, not an owned image.** The sprite system renders
   into whatever `RenderTarget` array it is handed (`&r->hdr_color[0]`), so it can also
   render into an offscreen layer or a UI target with no code change. `rt_resize`
   mutates targets in place (`renderer.c:1013-1020`), so holding pointers to elements
   survives swapchain recreation.
2. **One CPU instance array, one GPU buffer per slot.** The GPU copy exists because the
   vertex shader reads it through a device address; the per-slot multiplicity exists
   because of section 1.1. Size `capacity` to the *frame budget*, not to the theoretical
   maximum: 3 × 32 MB is what `SPRITE_MAX_INSTANCES = 1 M` costs if you never trim it.
3. **Pipelines are indexed by blend class.** Blend state is baked into the PSO in this
   backend (`GraphicsPipelineConfig.blends`, `vk.h:354`), so a batch cannot change blend
   state without a pipeline switch. Three pipelines is the whole permutation space;
   article 03 sorts on this, and article 07 shows premultiplied alpha collapsing it to
   two.
4. **Counters live in the struct, not in globals.** `draw_calls` is written while
   recording, `submitted` after the frame; article 08 turns both into regression gates.

> **Pipeline config lifetime trap.** `pipeline_create_graphics` copies your
> `GraphicsPipelineConfig` into `PipelineEntry` — *including the raw
> `const VkFormat *color_formats` pointer* (`vk.c:2130-2146`). Point it at a
> long-lived `VkFormat`: a `RenderTarget`'s format field (what `pass_fire` does at
> `renderer.c:748`) or a file-scope static. A pointer to a stack `VkFormat` compiles
> cleanly and then explodes on the first `pipeline_rebuild` after a shader hot reload.

---

## 1.8 Init, the pass, and the wiring

```c
/* src/two_d/sprite_init.inl (continued) */

static void sprite_system_create(SpriteRenderer *sprites, VkBackend *vk,
                                 RenderTarget *targets, uint32_t target_count) {
    assert(!sprites->inited && "sprite_system_create called twice");
    assert(target_count > 0 && targets);

    sprites->vk           = vk;
    sprites->targets      = targets;
    sprites->target_count = target_count;
    sprites->capacity     = SPRITE_MAX_INSTANCES;

    /* One CPU staging array, allocated once. Mirrors renderer_create (renderer.c:852):
       alignment comes from _Alignof, and there is no failure check by doctrine. */
    (void)posix_memalign((void **)&sprites->instances, _Alignof(SpriteInst),
                         sizeof(SpriteInst) * SPRITE_MAX_INSTANCES);

    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        /* Host-visible + device address: the same combination NuklearUi::uploads uses
           (src/nuklear_renderer.inl:81-83), so no staging copy is needed per frame. */
        if (!create_buffer(vk, sizeof(SpriteInst) * SPRITE_MAX_INSTANCES,
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST, &sprites->gpu_instances[i])) {
            log_fatal("[sprites] instance buffer allocation failed");
            abort();
        }
    }

    GraphicsPipelineConfig cfg = pipeline_config_default();
    cfg.vert_path              = "compiledshaders/sprite.vert.spv";
    cfg.frag_path              = "compiledshaders/sprite.frag.spv";
    cfg.color_attachment_count = 1;
    cfg.color_formats          = &targets[0].format; /* long-lived, like pass_fire */
    cfg.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    cfg.depth_test_enable      = false;
    cfg.depth_write_enable     = false;

    cfg.blends[0]                          = blend_disabled();
    sprites->pipeline[SPRITE_BLEND_OPAQUE] = pipeline_create_graphics(vk, &cfg);
    cfg.blends[0]                          = blend_alpha();
    sprites->pipeline[SPRITE_BLEND_ALPHA]  = pipeline_create_graphics(vk, &cfg);

    sprites->inited = true;
}
```

`pipeline_create_graphics` returns a **1-based** `PipelineID` (`vk.c:2145`: *"public IDs
are 1-based; 0 means no pipeline"*), which is why `PassDesc.pipeline = 0` means "caller
binds later" and why any array indexing must be `pipelines[id - 1]`. `create_buffer`
fills `Buffer::address` for you, so the root payload needs no `vkGetBufferDeviceAddress`
call in the pass.

The pass is deliberately thin, and stays this shape for the whole series:

```c
/* src/two_d/sprite_pass.inl — included by renderer.c after the pipelines exist */

static void pass_sprites(Renderer *r, VkCommandBuffer cmd) {
    SpriteRenderer *sprites = &r->sprites;
    VkBackend      *vk      = &r->vk;
    uint32_t        image   = vk->swapchain.current_image;

    GPU_SCOPE(&vk->gpuprofiler[vk->current_frame], cmd, "Sprites",
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {

        PassAttachment color = {
            .target = &sprites->targets[image],
            .load   = LOAD_CLEAR, /* nothing else writes hdr_color today */
            .store  = STORE_KEEP,
            .clear  = {0.02f, 0.03f, 0.05f, 1.0f},
        };

        begin_pass(vk, cmd,
                   &(PassDesc){.colors      = &color,
                               .color_count = 1,
                               .pipeline    = sprites->pipeline[SPRITE_BLEND_ALPHA]});

        /* Article 01: one quad, to prove the pipe. Article 02 feeds real instances. */
        SpritePush push = {
            .instances  = sprites->gpu_instances[vk->current_frame].address,
            .animations = 0, /* present in the layout, not read yet: see 1.6 */
            .base       = 0,
            .sampler    = vk->default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            .view       = {0.0f, 0.0f, (float)vk->swapchain.extent.width,
                           (float)vk->swapchain.extent.height},
            .viewport   = {(float)vk->swapchain.extent.width, (float)vk->swapchain.extent.height},
            .texel      = {1.0f, 1.0f},
        };

        cmd_draw(vk, cmd, BYTE_SPAN(push), 4, 1);
        end_pass(cmd);
        sprites->draw_calls = 1;
    }
}
```

Finally, three lines in `renderer.c`:

```c
#include "src/two_d/sprite_init.inl"   /* after #include "vk.h" */
#include "src/two_d/sprite_pass.inl"   /* next to src/nuklear_pass.inl */

/* in renderer_create, after renderer_resources_create: */
sprite_system_create(&r->sprites, &r->vk, &r->hdr_color[0], r->vk.swapchain.image_count);

/* in renderer_frame, between update_global_data(r) and post_pass(r, cmd): */
pass_sprites(r, cmd);
```

plus `src/two_d/sprite.c` in the Makefile's `SRC_C`. `compileslang.sh` globs
`shaders/*.slang`, so `sprite.slang` and both SPIR-V stages appear as soon as you run
`bash compileslang.sh` — or as soon as you touch a shader while the app runs, because
`dmon` calls the script and `pipeline_rebuild` swaps the PSO on the timeline
(`vk.c:2151-2173`).

One detail that will bite you: `desc.width/height` are `1362 × 749`, and `hdr_color`
uses `R16G16B16A16_SFLOAT`. A `TRIANGLE_STRIP` with 4 procedural vertices and no
depth test means the clear color is what you see if you got the corner order wrong:
you get two triangles that overlap incorrectly, and the "hole" is your clear color.
Symptom → cause, both ways: wrong winding with culling accidentally on, or a corner
order that does not produce a fan. `pipeline_config_default()` sets
`cull_mode = VK_CULL_MODE_NONE` (`vk.h:497`), so in 2D you never care about winding —
which is one more reason 2D is cheaper than it looks.

---

## 1.9 Verification — do not skip this

```bash
bash compileslang.sh                 # emits compiledshaders/sprite.{vert,frag}.spv
make -j$(nproc) && ./build/app
```

Four checks, in this order. Each one fails for a *different* reason, so do not skip
ahead:

1. **The shader exists and validates.**
   ```bash
   spirv-val --target-env vulkan1.3 compiledshaders/sprite.vert.spv && echo ok
   ```
   The sprite shaders use `PhysicalStorageBufferAddresses` (the `SpriteInst*` in the
   root) and `DrawParameters` (`SV_InstanceID`). Both capabilities come from features
   the backend enables (`vk.c:293`, `vk.c:314`); if you ever port this shader to a
   device without `bufferDeviceAddress`, the failure is at pipeline creation, not at
   shader compile.
2. **The pass shows up in the profiler.** Open the GPU profiler window; `GPU_SCOPE`
   labels the range `"Sprites"` and `gpu_profiler_ui_update` (called from `frame_start`,
   `renderer.c:1035`) publishes it one frame later. If the row is missing, the pass was
   never recorded — check the `pass_sprites(r, cmd)` line.
3. **The image is the right color.** The clear color `{0.02, 0.03, 0.05, 1.0}` is
   linear and the pipeline tonemaps it with `exposure = 1.2`; expect a dark blue-gray,
   not black. If it is pure black, the pass recorded but drew nothing. If it is *bright*
   gray, `LOAD_CLEAR` never ran and you are seeing uninitialized `hdr_color`
   (`post_pass` reads it without a clear).
4. **Nothing changed outside the pass.** `pass_nuklear` still runs after
   `pass_ldr_to_swapchain`, so the UI must still be visible on top of your quad. If the
   UI vanished, your pass left `rt` state or a rendering scope open — the classic symptom
   is `end_pass` called on a different code path than `begin_pass`.

Then take a screenshot with the Capture window. That is your regression artifact for
article 08, and it costs nothing to produce now.

---

## 1.10 Failure modes worth memorizing

| Symptom | Cause | Fix |
|---|---|---|
| Quad tears/flickers only at high fps | wrote a single non-slot buffer | `gpu_instances[MAX_FRAMES_IN_FLIGHT]` |
| Nothing draws, no validation error | root `instances` address is 0 or stale | check `create_buffer` succeeded; it fills `address` |
| Validation: `VUID-vkCmdDraw-None-...push constant` | `BYTE_SPAN(push)` size > 256 or not a multiple of 4 | `emit_root_data` asserts (`vk.c:2415`) — it caught you |
| Crash on hot reload | `cfg.color_formats` pointed at a stack local | point at `targets[0].format` |
| Sprite color is washed out / double-gamma | authored sRGB values into a linear HDR target | unpack and convert on the CPU at pack time |
| Everything is one flat color | `textures[id]` returned black: the picture id is not registered | article 05; for now sample a real picture |
| Profiler row absent | pass recorded but `GPU_SCOPE` name collided, or the profiler window is paused | `gpu_profiler_ui_update`, `renderer.c:918` |

The pattern behind most of these: **the root payload is the one place the validation
layer cannot help you.** Push constants are plain bytes; a stale device address, a
layout mismatch between the C and Slang struct, or a wrong `base` index all produce
garbage pixels rather than API errors. That is the price of the mechanism, and it is
why article 02 spends its whole length on proving the byte layout instead of trusting it.

---

## 1.11 Exercise, then next

1. Replace the single `cmd_draw(..., 4, 1)` with `cmd_draw(..., 4, 64)` and give
   `sprite_append` a 64-sprite grid. You now have 64 sprites in **one** draw call and
   still zero per-frame allocation. Watch `Sprites` in the profiler.
2. Set `.base = 32` without changing anything else. Exactly half your sprites move off
   the intended footprint — that is what an off-by-one in the batch base looks like in
   practice. This exercise exists so you recognize it later.
3. Add `_Static_assert(offsetof(SpritePush, view) == 32, "")` and see it fail by moving
   `view` below `sampler` in one declaration only. That is the class of bug the next
   article eliminates.

**Next:** [02 — The instance record format](02-instance-record-format.md), where the
32 bytes per sprite get audited, the C/Slang layout stops being a matter of trust, and
the animation stream either earns its extra cache line or gets folded into the instance.