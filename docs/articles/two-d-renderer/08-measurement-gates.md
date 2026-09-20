# 08 — Measurement gates

> **Why this article exists.** Every optimization in this series was justified by a measured
> number: 18 ns/sprite for classification, 0.7–1.1 ms for grouped prep, 2.8–4.7 ms shuffled,
> 1.827 vs 1.896 ms for the rejected counter refactor, 132 MB of overdraw traffic. Numbers
> rot. Code changes, compilers change, drivers change — and six months later the article's
> claims are folklore unless something enforces them. This article builds that something.

**What you have at the end.** A counter set, a fixed benchmark scene, a perf log, and a
`make bench` target that fails when a regression lands.

---

## 8.1 The counter set

Every number worth defending is already emitted by code this series wrote. Collect them in
one place, once per frame, into a fixed-size snapshot struct:

```c
/* src/two_d/sprite_stats.h */
typedef struct SpriteStats {
    uint32_t submitted;        /* sprite_frame_begin count                      */
    uint32_t drawn;            /* sprite_submitted_count (GPU, 1 frame late)    */
    uint32_t culled;           /* computed kernel count (article 04)            */
    uint32_t batches;          /* sprite->batch_count                           */
    uint32_t draw_calls;       /* pass_sprites recorded                         */
    uint32_t pipeline_binds;   /* counted in the recording loop                 */
    bool     already_grouped;  /* article 03 fast path taken                    */
    uint64_t prep_ns;          /* mu_time_now around sprite_end_frame           */
    uint64_t gpu_ns;           /* GpuProfiler scope around the sprite pass      */
} SpriteStats;
```

The invariants between them are the regression detector. Each one caught a real class of bug
in this series:

| Invariant | Catches |
|---|---|
| `draw_calls == batches` | a batch recorded twice, or skipped |
| `batches ≤ 96` at fixed scene | dense picture index broken (article 05) |
| `culled ≤ submitted`, both stable at fixed camera | culling kernel wrong, or degenerate marks not applied |
| `prep_ns / submitted` in 7–11 ns grouped, 28–47 ns shuffled | layout regression (article 06) |
| `already_grouped` true in the benchmark scene | game-side submission order broke |
| `gpu_ns` flat as `submitted` doubles | good news — you are vertex-bound, fill rate is fine |

Nothing here needs a profiler attached; the counters are the profiler, cheap enough to leave
on in debug builds.

---

## 8.2 The fixed benchmark scene

Ad-hoc benchmarks lie: different sprite distributions, different cache states, different
machine load. The scene must be a *fixture* — same seed, same counts, same layer
distribution, checked into the repo:

```c
/* tests/sprite_bench.c — the fixture, not a playground */
static void scene_build(SpriteRenderer *sprites, uint32_t count, bool shuffle) {
    muRnd rnd = mu_rnd_seed(0x5EED0000);       /* fixed: same scene every run */

    sprite_begin_frame(sprites, rnd);
    for (uint32_t layer = 0; layer < 8; layer++) {          /* grouped by layer */
        sprite_layer(sprites, (uint8_t)layer);
        for (uint32_t i = 0; i < count / 8; i++) {
            SpriteDesc d = {
                .x = (float)mu_rnd_range(rnd, 0, 1920),
                .y = (float)mu_rnd_range(rnd, 0, 1080),
                .w = 64.0f, .h = 64.0f,
                .picture = pic_ids[mu_rnd_range(rnd, 0, 32)],  /* 32 pictures */
                .blend   = (uint8_t)mu_rnd_range(rnd, 0, 3),
                .color   = 0xFFFFFFFFu,
            };
            sprite_push(sprites, &d);
        }
    }
    sprite_end_frame(sprites);

    if (shuffle)     /* the anti-benchmark: measure the slow path on purpose */
        sprite_records_shuffle(sprites, &rnd);
}
```

Three disciplines from article 03's harness, kept:

* **Fixed seed.** The histogram's branch predictor behavior is deterministic; a changing
  seed adds ±10 % noise that swamps a real regression.
* **Checksum.** The final batch list is folded into a `uint64_t` and printed; the bench
  asserts it matches the previous run's value. This is what stops the optimizer from
  deleting the work — and what catches *correctness* regressions in the sort at the same
  time.
* **Both input orders.** Grouped and shuffled are separate measurements, not one average.
  The grouped number is the product's claim; the shuffled number is the fast path's
  insurance.

Run each measurement as `min over N runs` (not mean): the minimum is the least-noisy
estimator of the true cost on a multitasking OS.

---

## 8.3 The perf log

A committed file, one line per measurement:

```text
# docs/perf-log.md — one row per change worth remembering
# date        | commit | machine    | scene              | grouped ms | shuffled ms | draws
2026-09-18    | d8b39e | i3-1125G4  | 100k/8L/32P/3B     | 0.74–1.13  | 2.80–4.70   | 96
2026-09-19    | (this) | i3-1125G4  | same               | 0.74–1.13  | 2.80–4.70   | 96   <- no change
```

Rules that make the log useful instead of decorative:

* **Same machine for a given claim.** Article 03's numbers are i3-1125G4 numbers; comparing
  them to a desktop's would be meaningless. The machine column is part of the datum.
* **A change with no perf impact still gets a row** when it *could* have had one — the
  "no change" row is what future-you checks before assuming a regression is old.
* **Ranges, not points.** A single number implies precision the machine does not have.
* **Rejected experiments get rows too.** Article 03's counter refactor (1.896 vs 1.827 ms)
  is in the log; otherwise the next person re-runs the same experiment and pays for the
  lesson twice.

---

## 8.4 The gate: `make bench`

The log records history; the gate prevents regressions. A bench target that the build can
fail on:

```make
# Makefile — article 08
BENCH_SCENE     ?= 100000
BENCH_GROUPED_NS ?= 12000   # gate: 12 ns/sprite, 1.2x over the measured 7-11
BENCH_SHUFFLED_NS ?= 50000  # gate: 50 ns/sprite, 1.06x over the measured 28-47

bench: $(BENCH_BIN)
	@$(BENCH_BIN) --scene $(BENCH_SCENE) --checksum --min-runs 5 \
	    --gate-grouped $(BENCH_GROUPED_NS) --gate-shuffled $(BENCH_SHUFFLED_NS)
```

The bench binary exits nonzero when a gate trips, printing the measured number and the
gate's origin (which perf-log row set it). Gates are set at **~1.1–1.2× the measured value**:
tight enough to catch real regressions, loose enough to survive a noisy CI machine. When a
deliberate change (a new feature, a format switch) moves the number, update the gate and the
perf log *in the same commit* — that is the discipline that keeps both honest.

What the gate does not cover, and why: GPU frame time is not CI-stable (driver versions,
thermal state, iGPU vs dGPU), so `gpu_ns` is logged but never gated. The overdraw arithmetic
of article 07 is enforced by the counter invariants instead — `draws ≤ 96` at the fixture
scene is a pure CPU-side number and *is* gated.

---

## 8.5 Tool inventory for the things counters cannot see

| Question | Tool | Where it fits this series |
|---|---|---|
| Where does GPU time go, per pass? | `GpuProfiler` timestamp scopes | already wired: `GPU_SCOPE` in `pass_sprites` (article 03 §3.7) |
| What does the CPU do between frames? | `mu_time_now` pairs around prep phases; Tracy if wired | article 06's phase table came from this |
| Why is a draw call slow? | RenderDoc / GPU capture | the instance record's 32 B layout is easiest to verify in the mesh view: offsets {0,8,16,18,20,22,24,28} |
| Is the atlas actually resident? | `vmaBudget` query | article 05's eviction policy needs this when `MAX_PICTURES` grows |
| Are barriers minimal? | synchronization validation layer | article 04's `cmd_buffer_barrier` is the one hand-written barrier; let the layer prove it once |

The doctrine's rule applies to tooling as to code: counters first (always on, nearly free),
timestamps second (per-pass, cheap), full captures last (per-bug, expensive). A frame that
needs a GPU capture to understand is a frame whose counters were insufficient — fix the
counter set, not just the frame.

---

## 8.6 Closing exercises

1. **Gate the layout contract.** Extend `make layout-check` (article 02) to assert the
   SPIR-V `ArrayStride` equals `sizeof(SpriteInst)` from the *compiled renderer*, not just
   the standalone test — so the contract survives a header change that only the renderer
   sees.
2. **Add a shuffled-input gate.** The current gate protects the grouped path; add
   `BENCH_SHUFFLED_NS` enforcement and a fixture variant that shuffles *between* layers, and
   verify the histogram phase (not the scatter) still dominates via the phase counters.
3. **Count degenerate marks.** Article 04's kernel knows how many sprites it degenerated;
   wire that counter into `SpriteStats` and gate `culled + drawn == submitted`.
4. **Measure the iGPU vs dGPU delta.** Run the fixture on both, log both rows, and check
   which of article 07's mitigations the delta attributes the difference to. This is the
   exercise that turns §7.2's arithmetic from theory into your machine's numbers.
5. **Break something on purpose.** Revert the fast path (force the shuffled path always),
   run `make bench`, and watch the gate fail. Then revert the revert. You should never ship
   a regression, but you should know exactly what the alarm sounds like.

---

## Series index

1. [Frame ownership and the first quad](01-frame-ownership-and-the-first-quad.md)
2. [The instance record format](02-instance-record-format.md)
3. [Ordering and batching](03-ordering-and-batching.md)
4. [GPU-driven culling and indirect draws](04-gpu-driven-culling-and-indirect.md)
5. [Pictures: atlas, residency, uploads](05-pictures-atlas-residency.md)
6. [CPU locality: the packing loop](06-cpu-locality-packing.md)
7. [GPU locality and overdraw](07-gpu-locality-and-overdraw.md)
8. [Measurement gates](08-measurement-gates.md) — this article

The series' one-sentence thesis, restated from article 01: **a fast renderer is a pipeline
that moves few bytes, measured at every step.** Everything else — the layout contracts, the
sort, the atlas, the gates — is machinery for keeping that sentence true.