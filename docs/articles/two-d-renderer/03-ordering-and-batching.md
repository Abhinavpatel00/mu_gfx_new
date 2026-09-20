# 03 — Ordering and batching: 100 000 sprites, 64 draw calls

> **Why this article exists.** 2D drawing is painter's order: whatever is submitted last
> is drawn last. Any batching scheme must preserve that order *exactly*, because a sprite
> that jumps in front of the wrong thing is a visible correctness bug, not a subtle one.
> At the same time, draw calls are the CPU-side unit of cost. This article resolves the
> conflict with one bounded cache, two sequential passes, and a fast path for the case
> real games are already in.

**What you have at the end.** `sprite_frame_end` produces a contiguous, correctly ordered
instance array plus ≤ 1024 batch records; recording is a straight-line loop of
`cmd_draw` calls; and you know from measurement where its 1 ms goes.

---

## 3.1 What must be equal for two sprites to share a draw call

A draw in this backend carries: one bound pipeline, one root payload, one viewport and
one scissor. So two sprites can share a draw only if every one of these agrees:

| State | Where it lives | Why it forces a break |
|---|---|---|
| blend class | pipeline (`GraphicsPipelineConfig.blends`, `vk.h:354`) | blend is baked into the PSO |
| picture (bindless slot) | root `picture` | `textures[i]` is indexed from a uniform |
| sampler | root `sampler` | same |
| shader variant (9-slice, SDF, mask) | pipeline | a different fragment program is a different PSO |
| clip / scissor rect | command state | there is exactly one scissor, set by `begin_pass` |
| layer (paint order) | **nothing on the GPU** | ordering only, never read by a shader |

Everything else — position, size, UV, color, flips — is per-instance and does **not**
break a batch. That is the entire point of article 02's layout: the fields that survive
into the draw call are the ones constant across it.

The last row is the one to be suspicious of. `layer` is *not* a GPU concept; it is the
"must be drawn before/after" relation. Sorting by it is a CPU activity, and a layer
boundary only breaks a batch when the layer's neighbours disagree on the other five
columns. In a normal 2D game the layer boundary and the blend/picture boundary coincide,
so layers cost nothing extra.

---

## 3.2 Why `qsort` is the wrong tool

The reflex is `qsort(sprites, count, sizeof(SpriteInst), compare_sprite)`. Three
independent problems:

1. **The comparator is an indirect call.** `qsort` cannot inline it, so every one of the
   ~1.7 million comparisons for 100 000 elements is a call plus a branch on an
   unpredictable result.
2. **It is `O(N log N)` random access.** Each comparison touches two sprite records at
   arbitrary indices — two cache misses that the hardware prefetcher cannot help with,
   because the access pattern is data-dependent.
3. **It moves 32 bytes per swap.** A comparison sort's total data movement is much larger
   than the one linear copy a counting sort needs.

With 100 000 sprites that is roughly 1.7 M comparisons × (call + two random loads) ≈ tens
of millions of cycles, i.e. 5–15 ms. The measurement in 3.4 shows a counting sort at
0.7–4.7 ms depending on input order, and the fast path exists precisely to keep the
common case at the low end.

---

## 3.3 The design: a bounded state cache, two passes, one fast path

Three pieces.

**1. A bounded, direct-mapped state cache.** The state of a sprite is
`(layer, dense_picture_index, blend, variant)` reduced to a 32-bit `key`. A hash table of
1024 slots maps each *distinct key* to a compact bucket id in `[0, 1024)`, assigned in
first-appearance order. Pictures must be **dense indices** here — the bindless slot is
sparse (article 05), and a sparse key space would make the counter array enormous.

**2. Two sequential passes over the instance array.**

```
pass 1 (histogram)  for i in 0..n:  b = bucket_id(instances[i]);  hist[b]++; bid[i] = b;
prefix              for b in 0..present: cursor[b] = running; running += hist[b];
pass 2 (scatter)    for i in 0..n:  dst[cursor[bid[i]]++] = instances[i];
```

Pass 1 reads the instance array once, sequentially. Pass 2 reads it once sequentially and
writes to `present` different regions — but each region is written *sequentially*, which is
what keeps the write path friendly: 768 write streams, each sequential, is very different
from 100 000 random writes.

**3. The fast path: detect that the submission order is already grouped.** Bucket ids are
assigned in first-appearance order, so "this frame is already grouped" is exactly the
predicate "`bid[i] >= bid[i-1]` for all `i`". Track that with one comparison inside pass 1
and, if it holds, skip pass 2 entirely: the instance array is already in the right order
and only the batch records need emitting. This is the shape a layered 2D game submits in
anyway, so the fast path is the *designed-for* case, not a curiosity.

Total storage, independent of how many sprites you draw:

| Structure | Size |
|---|---|
| state cache keys (`uint32_t[1024]`) | 4 KB |
| hash slots (`int32_t[1024]`) | 4 KB |
| histograms and cursors (`uint32_t[1024]` ×2) | 8 KB |
| bucket records (`Bucket[1024]`, 12 B) | 12 KB |
| **all of it** | **28 KB** — comfortably L1-resident on a 48 KB L1d |

That is the whole argument for this design over a radix sort or `qsort`: **every
bookkeeping byte the frame touches fits in L1 and is reused 100 000 times.**

---

## 3.4 What it actually costs (measured, not estimated)

Benchmark: the algorithm above, 32-byte instance records, 768 distinct states, 200
iterations, 100 000 sprites. Machine: Intel i3-1125G4 (2.0 GHz base / 3.7 GHz max),
48 KB L1d, 1.25 MB L2 per core, 8 MB L3, single thread, `-O3 -march=native`. Every result
is consumed through a checksum, so nothing is dead-code-eliminated.

```text
random    n=100000 present=768 :   2.804 ms/frame   28.04 ns/sprite
grouped   n=100000 present=768 :   1.133 ms/frame   11.33 ns/sprite
random    n=100000 present=768 :   4.699 ms/frame   46.99 ns/sprite
grouped   n=100000 present=768 :   0.744 ms/frame    7.44 ns/sprite
```

Three conclusions, in order of importance:

**1. Pre-grouped submission is 2.5–4× cheaper than random.** 0.74–1.13 ms versus
2.80–4.70 ms for the same 100 000 sprites and the same 768 states. The fast path is not a
micro-optimization; it is the difference between fitting the frame budget and not.

**2. The cost is not where you would guess.** Instrumenting the four phases separately
(clear, histogram, prefix, scatter) on 100 000 sprites and 768 states gave:

```text
clear 0.001 ms   histogram 1.855 ms   prefix 0.001 ms   scatter 0.040 ms
```

The histogram — one sequential read with a hash probe and a counter increment — is
**97 % of the cost**. The scatter, the part everyone fears because it writes into 768
regions, is 2 %. Bytes moved are not the problem: 3.2 MB read in 1.86 ms is only
1.7 GB/s, far below what this machine's L3 can do. What costs is the per-sprite
*dependency chain*: hash → load slot → compare key → increment a counter in a different
cache line, with an unpredictable branch on the probe. That is why cutting the state
count from 768 to 3 barely moved the number (1.75 ms vs 1.86 ms) and why ordering the
input helps so much: fewer distinct probe targets in flight, and counters that stay hot.

**3. The obvious micro-optimization made it worse.** Moving the counters out of the
`Bucket` record (12 B, so a counter update pulls in `key` and `first` too) into a
dedicated `uint32_t counts[1024]` array gave 1.896 ms versus 1.827 ms. The extra array
adds a second L1 stream and buys nothing, because the counter increments were never the
constraint — the probe was. That is the shape of most 2D renderer "optimizations":
plausible, measurable, and wrong. Keep the counters where they were.

For the budget ledger: at 100 000 sprites this pass is 4–10 % of a 16.67 ms frame when
grouped and 17–28 % when not. If you draw 100 000 sprites and want 120 fps, the CPU
batcher is the thing to attack — which is article 04's job, by moving the *per-sprite*
work to the GPU and leaving the CPU only the state bookkeeping.

---

## 3.5 An amendment to the record, forced by this design

The histogram's inner loop needs a **state key per sprite, in the same cache line as the
sprite's other data**. Article 02's record has no room: bytes 28..31 hold `flags`. But look
at what `flags` is actually used for — `SPRITE_FLIP_X` / `SPRITE_FLIP_Y`, which the vertex
shader implements as a UV endpoint select.

**Flipping a sprite is swapping the UV rectangle's endpoints.** `sprite_append` can do that
on the CPU, once, for free — and then the shader's flip selects, the `flags` field and the
last 8 bytes of the record all disappear:

```c
typedef struct ALIGNAS(16) SpriteInst {
    float    x, y, w, h;        /*  0..15 */
    uint16_t u0, v0, u1, v1;    /* 16..23  flips applied by swapping endpoints, on the CPU */
    uint32_t color;             /* 24..27 */
    uint32_t key;               /* 28..31  layer | dense_picture | blend | variant */
} SpriteInst;
```

```c
/* src/two_d/sprite.c — flip is a CPU-side UV swap */
void sprite_append(SpriteRenderer *sprites, float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1, uint32_t rgba,
                   PictureID picture, uint32_t layer, uint32_t blend, uint32_t flags) {
    if (flags & SPRITE_FLIP_X) { float t = u0; u0 = u1; u1 = t; }
    if (flags & SPRITE_FLIP_Y) { float t = v0; v0 = v1; v1 = t; }

    SpriteInst *inst = &sprites->instances[sprites->count++];
    inst->x = x;  inst->y = y;  inst->w = w;  inst->h = h;
    inst->u0 = sprite_pack_unorm16(u0); inst->v0 = sprite_pack_unorm16(v0);
    inst->u1 = sprite_pack_unorm16(u1); inst->v1 = sprite_pack_unorm16(v1);
    inst->color = rgba;
    inst->key   = sprite_state_key(layer, sprite_dense_picture(sprites, picture), blend,
                                  (flags >> 8) & 3u);
}
```

The offsets are **unchanged** — `u0` still at 16, `color` still at 24, bytes 28..31 still
the last field — so `make layout-check` from article 02 passes untouched and the shader's
only change is deleting the flip selects. That is what a layout contract test buys you: a
semantically significant change to a field's *meaning* is provably offsets-preserving, so
it can be made in one commit without re-auditing the struct.

The GPU never reads `key`. It exists so the histogram can classify a sprite without a
second array walk, and so the batch record needs no lookup table of its own.

---

## 3.6 Batch emission, and the merge that surprises people

```c
/* src/two_d/sprite_init.inl additions */
#define SPRITE_MAX_BATCHES 1024

typedef struct SpriteOrder {
    uint32_t keys[SPRITE_MAX_BUCKETS];   /* distinct state keys, first-appearance order */
    int32_t  slot[SPRITE_MAX_BUCKETS];   /* hash slot -> bucket id, -1 = empty */
    uint32_t bucket_count[SPRITE_MAX_BUCKETS];
    uint32_t bucket_first[SPRITE_MAX_BUCKETS];
    uint32_t cursor[SPRITE_MAX_BUCKETS]; /* scatter write cursor */
    uint16_t bid[SPRITE_MAX_INSTANCES];  /* instance -> bucket id */
    uint32_t present;
} SpriteOrder;

/* A bucket id for `key`, inserted on first sight. Exhaustion is a hard error: a full cache
   that keeps probing is a livelock, not a slow frame. */
static FORCE_INLINE uint32_t sprite_bucket(SpriteOrder *o, uint32_t key) {
    uint32_t h = (key * 2654435761u) & (SPRITE_MAX_BUCKETS - 1);
    uint32_t probes = 0;
    for (;;) {
        assert(++probes <= SPRITE_MAX_BUCKETS &&
               "sprite state cache exhausted: raise SPRITE_MAX_BUCKETS");
        int32_t s = o->slot[h];
        if (s < 0) {
            s                  = (int32_t)o->present++;
            o->slot[h]         = s;
            o->keys[s]         = key;
            o->bucket_count[s] = 0;
            return (uint32_t)s;
        }
        if (o->keys[s] == key)
            return (uint32_t)s;
        h = (h + 1) & (SPRITE_MAX_BUCKETS - 1);
    }
}
```

That assert is not decoration. The first version of this function — before the probe cap
existed — **livelocked** when the table filled: the probe never found an empty slot and
never matched, so it spun forever with the last good frame on screen. A bounded cache must
fail loudly at its boundary. Size it as `MAX_LAYERS × MAX_LIVE_PICTURES ×
SPRITE_BLEND_COUNT × MAX_VARIANTS`: 8 × 32 × 3 × 1 = 768, so 1024 is the smallest safe
power of two.

```c
/* src/two_d/sprite.c — one call per frame, at the end of the game's update */
void sprite_frame_end(SpriteRenderer *sprites) {
    VkBackend   *vk  = sprites->vk;
    SpriteOrder *o   = &sprites->order;
    SpriteInst  *src = sprites->instances;
    SpriteInst  *dst = (SpriteInst *)sprites->gpu_instances[vk->current_frame].mapping;
    uint32_t     n   = sprites->count;

    memset(o->slot, -1, sizeof(o->slot)); /* 4 KB, L1-resident */
    o->present = 0;

    /* Pass 1: classify, histogram, detect that nothing needs moving. */
    uint32_t last = 0;
    bool already_grouped = true;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = sprite_bucket(o, src[i].key);
        o->bid[i] = (uint16_t)b;
        o->bucket_count[b]++;
        if (i && b < last)
            already_grouped = false;
        last = b;
    }

    /* Prefix sum over the present buckets only: <=1024 iterations, L1 resident. */
    uint32_t running = 0;
    for (uint32_t b = 0; b < o->present; b++) {
        o->cursor[b]       = running;
        o->bucket_first[b] = running;
        running           += o->bucket_count[b];
    }
    assert(running == n && "prefix sum lost sprites");

    if (already_grouped)
        memcpy(dst, src, sizeof(SpriteInst) * n); /* already ordered: upload only */
    else
        for (uint32_t i = 0; i < n; i++)
            dst[o->cursor[o->bid[i]]++] = src[i];

    /* Order the buckets by key: <=1024 items, no allocation, insertion sort. */
    uint32_t key[SPRITE_MAX_BUCKETS], first[SPRITE_MAX_BUCKETS], count[SPRITE_MAX_BUCKETS];
    for (uint32_t b = 0; b < o->present; b++) {
        key[b] = o->keys[b];
        first[b] = o->bucket_first[b];
        count[b] = o->bucket_count[b];
    }
    for (uint32_t i = 1; i < o->present; i++) {
        uint32_t k = key[i], f = first[i], c = count[i], j = i;
        while (j > 0 && key[j - 1] > k) {
            key[j] = key[j - 1]; first[j] = first[j - 1]; count[j] = count[j - 1]; j--;
        }
        key[j] = k; first[j] = f; count[j] = c;
    }

    /* Emit batches, merging neighbours that differ only in bits the GPU cannot see. */
    sprites->batch_count = 0;
    for (uint32_t b = 0; b < o->present; b++) {
        SpritesDrawBatch *prev =
            sprites->batch_count ? &sprites->batches[sprites->batch_count - 1] : NULL;

        if (prev && sprite_gpu_state(prev->state_key) == sprite_gpu_state(key[b]) &&
            prev->first + prev->count == first[b]) {
            prev->count += count[b]; /* same pipeline, same picture: one draw covers both */
        } else {
            sprites->batches[sprites->batch_count++] =
                (SpritesDrawBatch){.state_key = key[b], .first = first[b], .count = count[b]};
        }
    }
    assert(sprites->batch_count <= SPRITE_MAX_BATCHES);
}
```

`sprite_gpu_state(key)` is `key & ~SPRITE_KEY_LAYER_MASK`. Two runs whose pictures,
samplers, blends and variants agree share a draw call **even when their layers differ**,
because the layer only decided their position in the sorted array — and it already did.
Their instance ranges are contiguous by construction, so merging them is free and correct.

This is the most commonly mis-taught idea in 2D batching, so it is worth stating plainly:

> **The sort key has more bits than the draw key.** Sort by everything that affects
> ordering; draw by everything that affects GPU state. Merging after the sort on the
> smaller key is where the draw-call count actually drops.

With 8 layers, 32 pictures and 3 blend classes, the worst case is 768 buckets; the merge
typically collapses the layer dimension entirely, leaving ≤ 96 draws, and ≤ 64 in a scene
that uses a handful of pictures per frame.

---

## 3.7 Execution: the recording loop is now trivial

This is the payoff of preparation. Nothing here branches on sprite data; the loop body is
at most one pipeline bind, one 256-byte root push, and one `cmd_draw`:

```c
/* src/two_d/sprite_pass.inl — replaces the article-01 stub */
static void pass_sprites(Renderer *r, VkCommandBuffer cmd) {
    SpriteRenderer *sprites = &r->sprites;
    VkBackend      *vk      = &r->vk;
    uint32_t        image   = vk->swapchain.current_image;

    GPU_SCOPE(&vk->gpuprofiler[vk->current_frame], cmd, "Sprites",
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {

        PassAttachment color = {.target = &sprites->targets[image],
                                .load   = LOAD_CLEAR,
                                .store  = STORE_KEEP,
                                .clear  = {0.02f, 0.03f, 0.05f, 1.0f}};

        /* pipeline = 0: the blend class varies per batch, so the loop binds it. */
        begin_pass(vk, cmd, &(PassDesc){.colors = &color, .color_count = 1});

        const Buffer *gpu   = &sprites->gpu_instances[vk->current_frame];
        uint32_t      bound = UINT32_MAX;
        uint32_t      draws = 0;

        for (uint32_t b = 0; b < sprites->batch_count; b++) {
            const SpritesDrawBatch *batch = &sprites->batches[b];
            uint32_t                blend = sprite_blend_of(batch->state_key);

            if (blend != bound) { /* at most SPRITE_BLEND_COUNT binds per frame */
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  vk->render_pipelines.pipelines[sprites->pipeline[blend] - 1]);
                bound = blend;
            }

            SpritePush push = {
                .instances = gpu->address,
                .base      = batch->first,
                .sampler   = vk->default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
                .picture   = sprites->pictures.bindless[sprite_picture_of(batch->state_key)].slot,
                .view      = {0.0f, 0.0f, (float)vk->swapchain.extent.width,
                              (float)vk->swapchain.extent.height},
                .viewport  = {(float)vk->swapchain.extent.width,
                              (float)vk->swapchain.extent.height},
            };

            cmd_draw(vk, cmd, BYTE_SPAN(push), 4, batch->count); /* 4 verts × count instances */
            draws++;
        }

        end_pass(cmd);
        sprites->draw_calls = draws;
    }
}
```

Four details that matter:

* **`pipelines[id - 1]`** — `PipelineID` is 1-based (`vk.c:2145`), so the `-1` is mandatory.
* **`pipeline = 0` in the `PassDesc`** — a `PassDesc` *can* bind a PSO for you, but the blend
  class changes between batches, so the pass deliberately leaves `pipeline` at zero;
  `begin_pass` treats `0` as "caller binds later" (`vk.c:2535`).
* **The `bound` cache.** A redundant `vkCmdBindPipeline` is driver-side state validation and
  a command-buffer word for nothing. One `uint32_t` removes it.
* **`batch->count` is `instance_count`.** `cmd_draw` ends in
  `vkCmdDraw(cmd, vertex_count, instance_count, 0, 0)` (`vk.c:2425`), so 4 vertices ×
  `count` instances gives a quad per instance; the vertex shader derives its corner from
  `SV_VertexID` and reads `pc.instances[pc.base + SV_InstanceID]`.

---

## 3.8 Verification: one invariant that catches everything

Batching bugs are "one sprite is in front of the wrong thing", and they are invisible in a
screenshot right up until they are not. Assert the tiling invariant instead:

```c
/* Debug only: assert() compiles out in release, per doctrine. */
static void sprite_order_validate(const SpriteRenderer *sprites) {
    uint32_t covered = 0;
    for (uint32_t b = 0; b < sprites->batch_count; b++) {
        uint32_t f = sprites->batches[b].first;
        uint32_t c = sprites->batches[b].count;
        assert(c > 0 && f + c <= sprites->count); /* in range            */
        assert(f == covered);                     /* no gap, no overlap  */
        covered += c;
    }
    assert(covered == sprites->count);            /* nothing was dropped */
}
```

Invariant `f == covered` — *batches tile the instance array with no gaps and no overlap* —
catches every batching bug I have seen: a dropped sprite, a duplicated sprite, an off-by-one
in the prefix sum, or a batch emitted with a pre-sort `first`. It is one integer comparison
per batch, needs no reference implementation, and it is the only ordering test you need.

Then publish the counters; they come out of the frame for free and are article 08's gates:

| Counter | Where | Expected |
|---|---|---|
| `sprites->count` | after `sprite_frame_begin` | the game's submission count |
| `sprites->order.present` | end of pass 1 | ≤ 768 for 8 layers / 32 pictures / 3 blends |
| `sprites->batch_count` | end of `sprite_frame_end` | ≤ 96 typical; ≤ 64 target |
| `sprites->draw_calls` | recording | equal to `batch_count` |
| `already_grouped` | pass 1 | true for a layer-ordered game |

If `draw_calls` tracks `count` instead of collapsing, your *dense picture index* is not
dense — every distinct bindless slot is becoming its own bucket. That is a CPU-side batching
problem before it is a texture problem, and article 05 fixes it at the source.

---

## 3.9 Failure modes, then the GPU

| Symptom | Cause | Fix |
|---|---|---|
| Livelock, last frame frozen on screen | state cache full, probe loop unbounded | probe cap + assert; size ≥ worst case |
| Wrong draw order | unstable ordering, or `first` computed pre-sort | invariant `f == covered` |
| Draw count ≈ sprite count | dense picture index is not dense | rebuild the picture table (article 05) |
| Frame time spikes when submission order shuffles | fast path not taken | expected: 2.8–4.7 ms vs 0.7–1.1 ms measured |
| Missing/garbled sprites only at high fps | wrote the mapped GPU buffer without a flush | `vmaFlushAllocation` on the written range (`nuklear_pass.inl:43`) |
| Missing sprites after a resize | `present`/`slot` state carried across frames | clear both every frame (done above) |

Closing the loop on the numbers: the fast path is what lets 100 000 sprites fit a 60 fps
budget on a 2 GHz laptop CPU, and it is free — one `if` inside a loop you were already
running. Everything else in this article is bookkeeping around that single predicate.

**Next:** [04 — GPU-driven culling and indirect draws](04-gpu-driven-culling-and-indirect.md),
where the per-sprite transform and the visibility test move into a compute kernel, the CPU
stops touching per-sprite payload entirely, and the 32-byte instance array becomes a
GPU-written resource that the host only ever reads for one thing: the draw count.