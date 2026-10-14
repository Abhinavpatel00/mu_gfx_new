---
name: dod-performance
description: >
  Write and refactor C (and C-like) code for maximum runtime performance using
  data-oriented design: table/SoA layout, existence-based processing, branch
  elimination, cache- and SIMD-friendly loops, explicit arenas/handles,
  ownership-based concurrency, LOD/mementos, and measure-first optimization.
  Use this whenever the task involves performance, games, engines, simulation,
  hot loops, memory layout, ECS, allocators, profiling, or when the user asks
  for "data-oriented", "high performance", "fast C", "DOD", or "cache friendly"
  code — even if they do not use those exact words.
---

# Skill: Data-Oriented Performance in C

This skill turns a request like "make this fast", "write a particle system",
"add an ECS", or "refactor this object-oriented C" into data-oriented C that is
measurably faster. It is distilled from `dod.txt`, Richard Fabian's
*Data-Oriented Design*, the Algorithmica *HPC* series, and the repository
`AGENT.md` style contract.

**Before writing code, read `AGENT.md` and `dodnotes.txt` at the project root.**
They are the authority; this skill is the operating procedure.

---

## 0. Core conviction (apply to every task)

> Program = **data + transformation**.
> Stop organizing memory around objects; organize it around the operations that
> consume the data. Represent state by **existence**, not by flags. Prefer
> **IDs and tables** over **pointers and objects**. Reduce **dependencies**.
> **Measure everything you claim.**

Do not apply the rules mechanically. Every technique here (SoA, SIMD,
branchless, non-temporal stores, fusion) has a losing case. That is why the
skill's last step is always a measurement, not an argument.

---

## 1. Procedure

Follow these steps in order. Do not skip step 1 or step 7.

### Step 1 — Frame the work as data

Answer, in writing if the task is non-trivial:

1. **What data exists?** List the values, their types/widths, their ranges.
2. **What transformations run?** Name each pass, its input, its output, and its
   frequency (per frame? per tick? on event? at load?).
3. **What is the access pattern per pass?** read-only / read-write / write-only;
   sequential / strided / random; how many elements; how big; hot or cold.
4. **What lifetime does each datum have?** per frame / per level / permanent.
5. **What can be derived, grouped, or regenerated** instead of stored?
6. **What does not need to exist individually** at the current level of detail?

### Step 2 — Choose the layout from the access pattern

- Fields touched together every frame → **AoS** rows or a single small struct.
- Fields touched by different passes → **SoA** parallel arrays.
- Same-size objects with holes → **pool / bulk storage + generational handles**.
- Optional facts → **table membership** or **sparse set**, never a nullable field.
- 2-valued flags over a large domain → **bitset**.
- Fixed locations (grid, slot, tile) → **index arithmetic**, no per-object position.
- Many identical things → **one record + count**.
- key → value → **hash table**; key → many → **multi-index / chunked list**.

### Step 3 — Remove branches the layout makes unnecessary

- Delete dead/hurt/visible/idle entities from the hot table (swap-remove);
  the `if (!dead)` disappears.
- Replace `enum + switch` dispatch with **separate tables + specialized
  transforms** (bucket once at spawn/state-change time).
- Sort/bucket by the branch's deciding axis so predictions become runs.
- Replace unpredictable selects with `?:`, `MU_MIN`, `MU_MAX`, masks, or CMOV.
- `MU_LIKELY`/`MU_UNLIKELY` only on genuinely cold error paths.

### Step 4 — Make the loop stream-friendly

- Contiguous or regular stride only. No pointer chasing in a hot loop.
- `MU_RESTRICT` on pointers that never alias; make ownership explicit.
- No function calls with unknown side effects in the vectorizable loop.
- No data-dependent `break`/early exit unless unavoidable.
- Separate read / read-write / write-only streams.
- Simple, regular loop bounds; pad alignment/count to a multiple of the vector
  width instead of writing scalar remainder code where it costs clarity.

### Step 5 — Own memory explicitly

- Pick one allocator per region: frame arena, scope arena, fixed pool, buddy.
- No `malloc`/`free` in hot code. No hidden allocation behind helpers.
- Large scratch goes in an arena or static buffer, not on the stack.
- Cross-table references are **IDs or `(index, generation)` handles**.
- Reuse slots (`inactive`) instead of delete/realloc.

### Step 6 — Concurrency from ownership

- One writer per datum. Independent transforms per worker.
- Local accumulators, one final reduce.
- Thread-local output buffers, gathered at the end (prefix sum for compaction).
- Keep per-thread warm data cache-line padded.
- Use `mu_thread_system.h` / `mu_task_scheduler.h`, not a bespoke pool.

### Step 7 — Measure, then keep or revert

- Time the real workload with `MU_SCOPE_TIMER` or a profiler.
- Inspect the hot 5% assembly (`-O3 -S`, Compiler Explorer, `-fopt-info-vec`).
- State the before/after numbers. If there is no improvement, revert and say so.
- No number → no claim.

---

## 2. Hard rules (violations are bugs)

1. No long-lived raw pointers as identity — use IDs/handles.
2. No `bool` arrays as processing filters over large domains — use tables/bitsets.
3. No `switch (type)` dispatch in a hot loop — use table membership.
4. No nullable fields for optional data — use membership in another table.
5. No pointer-chased hot loops — use arrays / CSR / wide nodes (B-trees).
6. No unchecked null chains — establish invariants at boundaries.
7. No data-dependent branches on random data in inner loops.
8. No `%` for power-of-two wrapping — mask.
9. No per-frame heap churn — arena-rewind or reuse.
10. No `int` in serialized/on-disk/network structures — fixed-width ints.
11. No unmeasured "optimization".
12. No new container/allocator in engine code: **extend `mylib` instead**.

---

## 3. Use `mylib` (`mu`) — never reinvent it

Include `#include "mu.h"` for the core; subsystems are separate headers
(`mu_ecs.h`, `mu_thread_system.h`, `mu_task_scheduler.h`, `allocators.h`,
`offset_allocator.h`, `mu_mmgr.h`, `mu_min_containers.h`, `mu_noise_math.h`,
`mu_geometric_algebra.h`, `mu_roguelike.h`).

| Need | Use |
|------|-----|
| Dense hot iteration | `array` (`array_push/size/capacity/reserve/pop/clear/free`) |
| Non-owning view | `mu_span` / `mu_span_f32_make` / `mu_span_f32_from_array` |
| Stable handles, holes | `mu_bulk_storage_*` (+ `mu_weak_handle`, generations) |
| Dense active IDs in range | `mu_sparse_set_*` (O(1), dense iteration) |
| key → value | `mu_hash32_*` / `mu_hash64_*` (fixed capacity options) |
| key → many values | `mu_multi_index_*` |
| Packed booleans / set algebra | `mu_bitset_*` (`next_set_bit`, `for_each`) |
| Ranged/contiguous ID alloc | `mu_id_pool_*` |
| Many small variable lists | `mu_chunked_u32_array_*` |
| Packed strings | `mu_string_arena_*` |
| Intrusive lists | `mu_pool_link` / `mu_index_list_*` / `mu_freelist_*` |
| Cumulative offsets / rank | `mu_length_index_*` |
| Quantization / bit packing | `mu_bitpacking.h` (`mu_quantize_unorm/snorm/half`) |
| Linear/frame/ring arena | `mu_linear_allocator`, `mu_ring_allocator`, `mu_arena` |
| Fixed-block pool | `mu_pool_*` |
| Variable sizes + coalescing | `mu_buddy_*`, `offset_allocator.h` |
| Leak/corruption tracking | `mu_mmgr.h` |
| Deterministic RNG | `mu_pcg32_*` |
| ECS + transforms + prefabs | `mu_ecs_*` |
| Worker pool + atomics | `mu_ts_*` |
| Priorities/groups/parallel_for | `mu_sched_*` |
| Timing | `MU_SCOPE_TIMER("name")`, `mu_time_now()` |
| Noise / roguelike domain math | `mu_noise_math.h`, `mu_roguelike.h` |

**If a primitive is missing:** add a small, reusable, header-only primitive to
`mylib/mu/` with an `MU_API`/`MU_STATIC` linkage model, `mu_malloc`-style macro
overrides, and `MU_ASSERT` on contract violations. Then use it. Do not add a
second competing container or allocator family to engine code.

Macros from `mu_common.h`: `MU_ASSERT`, `MU_INLINE`, `MU_RESTRICT`,
`MU_ALIGN(N)`, `MU_LIKELY/UNLIKELY`, `MU_PREFETCH`, `MU_MIN/MAX/CLAMP/SWAP`,
`MU_ALIGN_UP/DOWN`, `MU_IS_POW2`, `MU_STATIC_ASSERT`,
`mu_popcount_u64`, `mu_trailing_zeroes_u64`, `mu_leading_zeroes_u64`.

---

## 4. Design patterns to reach for

```
existence        : state → table membership (Damaged[], Visible[], Walking[])
optional data    : sparse component / sparse set, never a nullable field
scheduled work   : sort by ready_time, process while ready_time <= now
implicit entity  : Entity = id; components attached by id define it
composition      : create_entity() inserts into relevant tables (factory)
transform FSM    : evaluate conditions → transition buffer → single commit
LOD              : representation = f(relevance), not f(distance)
collective LOD   : N identical entities → 1 record + count
memento          : keep compressed history for future refinement
JIT memento      : regenerate from Hash(entity_id) instead of storing
derived state    : state = f(axis); avoid independent booleans
index on demand  : linear search until a query is hot, then build+maintain an index
bloom filter     : cheap "definitely absent" test before expensive lookup
prefix sum       : compaction/offsets; parallel allocation of output slots
```

---

## 5. Anti-pattern detector (rewrite triggers)

If you see any of these, stop and restructure before adding features:

- a struct with many fields used by different functions;
- `for (e : entities) if (e.flag) ...` where `flag` is usually false;
- `switch (e.type)` in a per-frame loop;
- `->next` walking in a hot path;
- an index built before it is needed;
- alloc/free paired inside a loop;
- a shared counter written by every thread;
- sorting in a hot path when only a top-K or an Nth element is needed;
- a scene graph traversal rebuilding a render list each frame;
- emulating every entity individually when a statistic would be believable.

---

## 6. Review checklist before reporting done

```
[ ] Data framed: passes, access patterns, lifetimes written down.
[ ] Layout matches access pattern (SoA/AoS/pool/grid/bitset chosen, justified).
[ ] Hot rows hold no pointers; ids/handles only.
[ ] State is existence; optional data is membership; no nullable members.
[ ] No enum-as-flow dispatch in hot paths (or justified).
[ ] No data-dependent branches on random data.
[ ] No pointer chasing; restrict/ownership explicit.
[ ] One allocator strategy per region; no hot malloc; stack bounded.
[ ] Concurrency: one writer, local reduce, no false sharing.
[ ] mylib used for every container/allocator; missing primitive added to mylib.
[ ] Before/after measurement recorded; revert if no gain.
[ ] Compiles warning-clean with -Wall -Wextra -Werror.
```

---

## 7. Reference material

- Skill `game-engine-dod` — composes these rules into a whole game/engine
  (frame loop, ECS, physics, animation, culling, LOD, renderer batching). Load
  that skill when the task is a game or engine rather than a single hot path.
- `references/dod-quickref.md` — condensed doctrine, formulas, and defaults.
- `references/mu-map.md` — fuller `mylib` API map with call shapes.
- Project root `AGENT.md` — the full style contract and code examples.
- Project root `dodnotes.txt` — the human-readable DOD notes.
- `mylib/docs/` — per-module documentation.
