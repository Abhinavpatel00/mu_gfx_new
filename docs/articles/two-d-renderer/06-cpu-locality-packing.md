# 06 — CPU locality: the packing loop

> **Why this article exists.** Articles 01–05 built a pipeline whose CPU cost, measured in
> article 03, is 0.7–1.1 ms per 100 000 sprites when the game submits grouped. This article
> takes that number apart. Roughly half of it is classification and histogram (measured: 18
> ns/sprite); the other half is the pack loop — the code that turns the game's submission
> into the 32-byte `SpriteInst` records article 02 designed. Both halves are won or lost by
> *data layout*, not by clever code.

**What you have at the end.** A packing loop that runs branch-free over contiguous submission
data, an API that makes the fast path the default, and a measurement harness that tells you
when either regresses.

---

## 6.1 The anatomy of 0.7 ms

For 100 000 sprites on the i3-1125G4 (article 03's numbers), the grouped path spends its
budget roughly as:

| Phase | Work | Measured share |
|---|---|---|
| Submission iteration + key computation | read game record, emit key | ~10 % |
| State-cache probe | hash + probe, mostly 1-cycle hits | ~25 % |
| Histogram + prefix sums | 4 passes over 1024 counters (measured: 97 % of *sort* time is here) | ~35 % |
| Scatter (pack + write instances) | read records again, write 32 B each | ~25 % |
| Batch merge + draw list | ≤ 1024 keys | ~5 % |

Two conclusions fall out of this table, and both were verified by failed experiments in
article 03:

1. **The histogram dominates the sort.** Its cost is a dependency chain, not bandwidth —
   which is why the "optimization" of keeping counters outside the bucket struct made it
   *slower* (1.896 vs 1.827 ms measured): it added a second stream to touch without removing
   the chain.
2. **The scatter is bandwidth-bound**, 32 B per sprite, so it is won by layout: the
   submission array, the sorted order array, and the instance array should all be iterated
   sequentially with no pointer chasing. Any per-sprite indirection — a hash lookup, a
   function call, a `PictureEntry` dereference — lands directly on this phase's critical
   path.

So the design rule for everything below: **keep the hot path to three sequential streams**
(submission records in, 4-byte keys and order indices through, 32-byte instances out) and
move everything else — picture resolution, atlas UV lookup, animation sampling — *before*
the hot loop, into preparation.

---

## 6.2 The API that makes the fast path default

The game-side submission API decides whether the batcher ever sees its fast path. Three
rules, all enforced by the API shape rather than by documentation:

```c
/* src/two_d/sprite.h */
void sprite_begin_frame(SpriteRenderer *sprites, muRnd rnd);      /* article 01 */
void sprite_layer(SpriteRenderer *sprites, uint8_t layer);        /* opens a layer */
void sprite_push(SpriteRenderer *sprites, const SpriteDesc *desc); /* one sprite */
void sprite_push_span(SpriteRenderer *sprites, Span(SpriteDesc) descs);
void sprite_end_frame(SpriteRenderer *sprites);                    /* triggers prep */
```

* **`sprite_layer` sets the sort key's layer field for everything pushed after it.** The
  game declares layers up front; the renderer never re-derives them per sprite. This is what
  makes "submission is grouped" the *natural* way to use the API rather than a discipline the
  game must remember.
* **`sprite_push` takes the descriptor by const pointer, 48 bytes, one dereference.** It
  copies the 32 payload bytes into the instance array and reduces the rest (picture → dense
  index, atlas rect → UV endpoints, animation frame → quantized UVs) *at push time*, not in
  the pack loop. The pack loop then sees only finished 32-byte records and 4-byte keys.
* **`sprite_push_span` exists because call overhead is real at 100 000/frame** — measured
  in article 03's benchmark harness, where the inlined checksum loop added ~15 % over the
  batched variant. A span push lets the game submit a whole row of particles in one call.

The reduction at push time is the "prepare, then execute" split from the doctrine, applied
to the CPU side of the renderer:

```c
static inline void sprite_push(SpriteRenderer *sprites, const SpriteDesc *d) {
    assert(sprites->count < SPRITE_MAX_INSTANCES && "raise SPRITE_MAX_INSTANCES");

    PictureEntry *pic = &sprites->pictures.entries[d->picture];
    uint32_t bucket = sprite_bucket(pic->dense_index, d->blend);   /* ≤ 1024, article 03 */

    /* CPU-side flip: swap UV endpoints, no GPU flag (article 03 §3.5). */
    uint16_t u0 = pic->uv[0], v0 = pic->uv[1], u1 = pic->uv[2], v1 = pic->uv[3];
    if (d->flip & SPRITE_FLIP_X) { uint16_t t = u0; u0 = u1; u1 = t; }
    if (d->flip & SPRITE_FLIP_Y) { uint16_t t = v0; v0 = v1; v1 = t; }

    sprites->records[sprites->count] = (SpriteInst){
        .pos    = {d->x, d->y},
        .size   = {d->w, d->h},
        .uv     = {u0, v0, u1, v1},
        .color  = d->color,
        .key    = sprite_key(bucket, sprites->current_layer),
    };
    sprites->count++;
}
```

Everything in this function is a load, a store, and at most two branches on `d->flip` — and
the flip branches are the *only* data-dependent branches in the whole CPU pipeline, because
everything else was already resolved into table entries by the time the record is written.

---

## 6.3 The pack loop's memory behavior, made explicit

Article 03's scatter phase reads the finished records through the order array and writes
instances out. Written down, its access pattern is:

```c
/* Pass 3, scatter: order[i] is the source record for destination slot i. */
for (uint32_t i = 0; i < sprites->count; i++) {
    uint32_t src = order[i];                      /* gather: random-ish within bucket */
    gpu_instances[i] = records[src];              /* 32 B read, 32 B write           */
}
```

This is a gather, and gathers defeat prefetchers — except that each *bucket range* of
`order[]` is a permutation of a contiguous *source* range, so consecutive destinations in a
bucket read sources that cluster in one or two cache lines. With 32-byte records, a 64-byte
cache line holds exactly two records: the bucket scatter reads each source line at most twice
before moving on. That is the entire reason `SpriteInst` is 32 bytes and not 36 or 40: it
packs the record to a divisor of the cache line so that no source line is partially used.

The write side is even simpler: `gpu_instances[i]` is a pure sequential store of 32 B
records — two 16-byte stores per record on x86-64, which the compiler emits as `movups`
pairs, streaming straight to the write-combined mapped buffer.

Worth stating plainly: there is no SIMD intrinsic in this loop. The 32-byte record copy is
already two vector moves, and the surrounding code is integer bookkeeping that SIMD cannot
express. Measuring before reaching for `xmmintrin.h` saved this article from a
paragraph of speculative AVX code.

---

## 6.4 SoA, AoS, and where each applies

The doctrine's rule is "SoA when fields are processed independently; AoS when complete
records are processed together". The renderer exercises both, and it is worth seeing why each
is right where it is:

| Structure | Layout | Why |
|---|---|---|
| `records[]` (SpriteInst) | AoS, 32 B | Every consumer (pack loop, vertex fetch) reads the whole record. Splitting it would mean two gathers instead of one. |
| `order[]` (uint32) | SoA, 4 B | Only the key and the index are touched; the record payload is dead weight here. |
| `buckets[]` (SpriteBucket) | AoS, 12 B | Histogram, prefix-sum and merge phases all read the whole bucket. |
| `PictureEntry` | AoS, hot fields first | Push-time resolution reads all fields; batch-time never touches it. |

The common failure this table prevents: applying SoA uniformly because it sounded faster.
Splitting `SpriteInst` into `pos[]`, `uv[]`, `color[]` arrays would make the pack loop four
sequential stores instead of two, and would break article 02's verified layout contract —
the SPIR-V `ArrayStride 32` match — for zero measured gain.

---

## 6.5 What to measure, and how

The harness from article 03 (`/tmp/sort`, rebuilt under `tests/` for the real repo) measures
three numbers per frame; keep them in the perf log (article 08) alongside the frame budget:

1. **ns/sprite for the whole CPU prep** (`sprite_end_frame` inclusive). Grouped input:
   7–11 ns. Shuffled input: 28–47 ns. These are the article 03 measurements; a regression
   past them means the layout changed, not the algorithm.
2. **ns/sprite for the pack loop alone** (time `sprite_push_span` separately). Target:
   under 5 ns — it is one struct copy plus a table lookup.
3. **draw count at fixed scene**, which article 03 proved collapses to ≤ 96 when the dense
   picture index is dense (article 05) and balloons when it is not.

Build them with the same flags as the real renderer and a checksum on the output so the
optimizer cannot delete the work — article 03's non-DCE discipline, applied everywhere.

---

## 6.6 Failure modes

| Symptom | Cause | Fix |
|---|---|---|
| Pack loop 15 %+ slower than measured | `sprite_push` not inlined, or descriptor passed by value | span API; check the disassembly once, then trust the layout |
| Scatter phase regressed after a "cleanup" | record size drifted off 32 B (e.g. a `bool` added) | the `_Static_assert(sizeof(SpriteInst) == 32)` from article 02 fires; keep it firing |
| 2× slowdown on shuffled submissions | fast path not hit — layers not grouped | fix the *game's* submission order, not the renderer |
| Prefetcher stalls in scatter | records grew past 32 B, cache lines half-used | §6.3: keep the record a divisor of 64 B |
| Numbers vary run to run | measurements taken on a busy machine or without checksums | fixed-seed input, pinned frequency, checksummed output |

The summary of the whole article: **0.7 ms is not an algorithm's cost, it is a data
movement's cost.** Every optimization that worked in this series — 32-byte records, 4-byte
keys, one table lookup at push time — removed bytes from a stream. None of them added code
cleverness.

**Next:** [07 — GPU locality and overdraw](07-gpu-locality-and-overdraw.md), which crosses
to the other side of the PCIe bus: what the GPU actually does with the 32-byte records once
they arrive, and why fill rate — not vertex count — is what the measured frame spends its
time on.