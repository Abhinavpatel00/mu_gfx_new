# DOD Quick Reference

Companion to `.agents/skills/dod-performance/SKILL.md`. Condensed rules,
defaults, formulas and diagrams. Full narrative: project-root `dodnotes.txt`.

---

## 1. Core transformations

```
struct Someone { something }        →   something[someone_id]

Monolithic object
    ↓ explode
Data + dependencies
    ↓
Components / tables
    ↓
Systems / transforms
    ↓
Process only active data
```

```
enum + switch           →   table membership + specialized transform
"what am I?"            →   "what processing am I currently eligible for?"
object → vptr → method  →   id → table → data
instance → components   →   component → instances
```

```
many identical entities  →  1 record + count
fixed locations          →  location → state   ("positions have wheat")
full entity → simplified → collective → aggregate/count → procedural/implicit
```

---

## 2. Layout defaults

| Situation | Layout |
|---|---|
| Fields used by different passes | SoA (parallel arrays, padded) |
| Fields always used together | AoS (single struct) |
| Same-size objects, churn, need handles | pool + freelist + generations |
| Optional per-entity data | sparse set / side table keyed by id |
| 2-valued flag over large domain | bitset (1 bit each) |
| Fixed grid / slots | index arithmetic, no stored position |
| Search keys only | packed key array + payload array (index after search) |
| Hot + cold fields | split into two tables joined by id |

Do not mechanically apply SoA. Related data consumed together stays together.

---

## 3. Existence-based processing

```
Don't ask "does this object do something?"
Store    "these objects do something."
```

- Hurt entities live in `Damaged[]`, not behind `if (health < max)`.
- Visible entities live in a bitset, not behind `bool visible[]`.
- Subscribed listeners live in the event table; dispatch processes the table.
- `dirty == true` becomes `entity ∈ DirtyTable`.
- State `Running` becomes `entity ∈ Running[]`.

Row-of-truth: `existence / membership IS the state`.

Replacement map:

```
bool flag over many rows   →  bitset
optional component         →  sparse set / side table
enum as flow               →  separate tables
dirty flag                 →  list of things needing work
object hierarchy           →  independent transforms
```

---

## 4. Branch rules

```
branch exists  ≠  branch is unpredictable
unpredictable branch  →  sort/bucket/group data, then re-check
unpredictable select  →  x = c ? a : b  (CMOV / select / min / max)
```

- Cheap predictable work can beat an expensive check that avoids it.
- `if (dirty) update()` may lose to `update()` if the update is cheap.
- Hints (`MU_LIKELY`) only on cold error/init paths.
- Process in homogeneous bursts (A A A B B B C C C) to keep code/data hot.
- Sorting networks: fixed compare-exchange sequence, no data-dependent control.

---

## 5. Memory model

```
L1 ~4 cyc · L2 ~12 cyc · L3 ~40 cyc · DRAM ~80–300 cyc
cache line = 64 B = 16 ints = 8 doubles = 16 floats
memory request  →  whole line  →  use the rest of the line
```

- `address(i) = base + i*stride` — predictable, prefetchable, vectorizable.
- `p = p->next` — next address depends on previous load; no MLP; guaranteed miss.
- Graph → CSR. Tree → wide nodes (B-tree). Fewer hops, fewer stalls.
- Separate read-only / read-write / write-only streams.
- Write contiguously, in large amounts, using the bytes you load.
- Non-temporal stores for large one-shot output (avoid cache pollution).
- False sharing: pad per-thread hot data to 64 B; use local accumulators.
- `possible alias → reload`; `known non-alias → registers, reorder, vectorize`.
  `const` does not prove non-aliasing; `restrict` does.
- Return small structs by value (RVO) instead of out-parameters.

---

## 6. SIMD / auto-vectorization

Vectorizer-friendly loop:

```
contiguous · no early exit · no unknown calls · aligned · no cross-iteration dep
input and output non-aliasing · regular trip count · whole output stream written
```

Breaks vectorization:

```
if (a[i] < 0) break;         data-dependent exit
a[i] = f(a[i]);              opaque call / side effects
a[i] += a[i + 1];            cross-iteration dependency
float *a, *b (may alias)     aliasing
```

A branch where every iteration completes is fine — it becomes a select.

Order of attack: simple scalar loop → check `-fopt-info-vec` → restructure →
intrinsics only for structural lane work the compiler cannot infer.

SIMD wins on: enough data · independent ops · suitable layout · vector-friendly
instructions. It loses on tiny workloads with setup/remainder overhead.

---

## 7. Allocation & lifetime

```
frame arena     → alloc per frame, free by pointer reset
scope arena     → bulk alloc, bulk free
fixed pool      → same-size objects, freelist
buddy / offset  → variable sizes with coalescing
static array    → zero allocation, fixed capacity
```

- One allocator strategy per region. No global malloc sprinkled in hot code.
- Stack is limited: no huge locals, no deep call chains.
- Handles: `{ index, generation }`; stale handles fail a compare instead of
  corrupting memory.
- Deletion: `table[i] = table[--count]` (swap-remove) when order is free.
- Inactive ≠ deleted: keep memory, stop processing.
- Offsets over pointers where identity must survive serialization/realloc.

---

## 8. Concurrency

```
don't make shared memory safe — design so sharing isn't required
T0 → data A · T1 → data B · T2 → data C   →   reduce
```

- One writer per datum. Local accumulators, one final shared update.
- Thread-local output buffers; gather with prefix sums.
- Keep worker data on separate cache lines.
- Map / filter / variable-output / multi-stage patterns.
- Prefix sum = parallel compaction / output offsets / parallel allocation.
- Thread-local ownership beats reference counting; assert owning thread in debug.

---

## 9. Search & sort

```
do not search if derivable · index only hot repeated queries
O(log N) poor locality  can lose to  O(N) excellent locality
bloom filter: definitely absent | possibly present (no false negatives)
maintain top-K incrementally instead of sorting every frame
```

Sort only as much as necessary:

```
full sort: 1e6 ordered | partial sort: top K | quickselect: Nth element
counting/radix sort for known discrete ranges (material/shader/buffer ids)
insertion sort for small/nearly-sorted · merge sort for parallel large
sorting networks for tiny fixed N
maintain sorted state incrementally when updates are small
```

Render order:

```
opaque       → front-to-back (depth rejection kills overdraw)
transparent  → back-to-front (blending depends on order)
cost ≈ texture + mesh + state_change + overdraw   → measure, then choose key
```

---

## 10. LOD, mementos, generation

```
LOD = f(relevance)
relevance = f(distance, screen size, visibility, attention,
              time_since_seen, interaction, progression, expertise, importance)
```

```
LOD0 full → LOD1 simplified → LOD2 approximate → LOD3 collective →
LOD4 aggregate/implicit
far+irrelevant low · far+important high · near+irrelevant medium · near+important high
```

- Collective: `100 aircraft → Squadron{count, attack_probability}`.
- Memento: keep compressed history needed to refine later (lossy is fine).
- JIT memento: `state = Generate(Hash(entity_id))` — store the recipe, not the data.
- Bias, don't uniform-random: `believable = repetition + variation + imperfections`.
- Temporal LOD: 0–10 s full · 10–60 s simplified · 1–10 min compressed · long → seed.
- Interaction history raises importance (player's parked car ≫ ambient car).

---

## 11. FSM / state as data

```
state = table membership
transition = remove from one table + insert into another

old_state + signals → conditions → transition_requests → triggers/log → commit → new_state
```

Evaluate against one consistent snapshot, then commit. Use a sequential FSM
only when signal order is fundamentally the problem (lexer/parser).

---

## 12. Normalization (the relational link)

```
1NF  atomic values only (no lists/nullable members)
2NF  no partial dependency on part of a composite key
3NF  no transitive dependency (A→B→C)
BCNF every non-trivial dependency has a superkey
```

- Start with one big table, then normalize.
- Never store instance state next to definition data.
- More tables can use less memory (optional fields → no nulls).
- Then intentionally **denormalize runtime data** for cache/branch/bandwidth.
- Normalization removes coupling; DOD re-layouts what remains by access pattern.

---

## 13. DOD naming of the recurring patterns

```
PATTERN 1  state → table membership
PATTERN 2  optional state → sparse component
PATTERN 3  scheduled event → sorted time table
PATTERN 4  object identity → ID
PATTERN 5  object → component composition
PATTERN 6  full entity → collective representation
PATTERN 7  dirty flag → list of things that need work
PATTERN 8  object hierarchy → independent data transforms
```

---

## 14. Performance model

```
performance ≈ computation
            + memory traffic
            + cache behaviour
            + branch behaviour
            + synchronization
```

Most important principle:

```
REDUCE UNNECESSARY DEPENDENCIES
    fewer dependent instructions · fewer pointer hops
    fewer unpredictable branches · fewer random writes
    fewer cache-polluting accesses
```

Core question for every conditional optimization:

```
Is avoiding the work cheaper than doing the work?
```

Workflow:

```
MEASURE → identify bottleneck → change DATA/LAYOUT/ALGORITHM → MEASURE → keep or revert
```

---

## 15. One-liners worth memorizing

```
The wheat doesn't have positions; the positions have wheat.

Don't build an object because the thing has a name;
build the data required to represent the work the program actually does.

LOD is the resolution of existence, not polygon count.

Make data flow simple enough that the CPU and compiler can see
the independence you already know.

The compiler can eliminate instructions.
It cannot rescue a fundamentally poor data layout.
```
