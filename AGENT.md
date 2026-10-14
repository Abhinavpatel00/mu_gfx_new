 AGENT.md — Data-Oriented C for Maximum Performance

> **What this file is.** Operating instructions for any AI agent (or human)
> writing C in this repository. It encodes one person's C style, the
> Data-Oriented Design (DOD) doctrine, the hardware realities from the
> Algorithmica *HPC* series, and the rules for using the bundled `mylib`
> (`mu`) library.
>
> **Prime directive:**
> **Lay data out for the loop you will run, remove every branch the layout
> makes unnecessary, own your memory explicitly, reduce dependencies, and
> measure everything you claim.**
>
> Sibling documents (read them, they are part of this contract):
> - `dodnotes.txt` — the human-readable DOD notes (Fabian + Lemire + Algorithmica).
> - `.agents/skills/dod-performance/SKILL.md` — the loadable agent skill.
> - `../mycstyle.txt` — the original style notes this file distills.
>
> **Code examples in this file are illustrative fragments, not standalone
> compilable programs.** They show layout, control flow and API shape. When
> writing real code, compile it with the flags in §12.7 and measure it.

---

## 0. The one-paragraph summary

Programs are **data + transformation**. Stop organizing memory around objects;
organize it around the operations that consume the data. Design tables and
arrays whose *shape* makes the hot loop simple: contiguous, predictable,
branch-light, dependency-light. Represent state by **existence** (table
membership, bitsets, sparse sets) instead of by flags and `switch`es. Use
**IDs/handles** instead of long-lived pointers. Prefer **static, fixed-capacity,
or arena-backed** storage. When a reusable primitive is missing, **add it to
`mylib`** rather than hand-rolling a one-off. Then **measure** — the compiler
can eliminate instructions, but it cannot rescue a fundamentally poor data
layout.

---

## 1. The non-negotiables (read this first)

1. **Data layout before algorithms.** Choose the memory layout from the access
   pattern, then pick the algorithm. Not the other way round.
2. **IDs over pointers.** Long-lived references are `EntityId`, `MeshId`,
   `TextureId`, `Handle`, or an array index — never a raw pointer held across
   frames.
3. **Existence over flags.** If a thing needs processing, it lives in a table
   for that processing. Do not scan 100,000 entities to find the 1,000 that
   are hurt.
   Flags that introduce `switch`es/`if`s in a hot loop are a layout bug.
   Do not store "what am I" per row and branch on it — store *eligibility*
   as table membership and run one specialized transform per table:

   ```c
   /* WRONG: one mixed array, switch inside the loop */
   for (size_t i = 0; i < n; ++i)
       update_entity(&entities[i]);       /* switch inside */

   /* RIGHT: separate flat arrays, one specialized loop each */
   for (size_t i = 0; i < player_count; ++i)     update_player(&players[i]);
   for (size_t i = 0; i < enemy_count; ++i)      update_enemy(&enemies[i]);
   for (size_t i = 0; i < projectile_count; ++i) update_projectile(&projectiles[i]);
   ```

   A per-row field used only as a *load index* (mesh id, material id) is
   not a flag. A per-row field ever tested with `if`/`switch`/`?:` on a
   data-dependent condition inside a loop over rows is a flag: delete it,
   split the table, specialize the kernel. When in doubt between one
   mixed array plus a branch and separate flat arrays plus one
   specialized loop each, choose the separate arrays — moving rows
   between tables at insert/remove time is cheaper than branching on
   every row every frame.
4. **No pointless null checks.** Eliminate nulls by establishing *invariants*
   at boundaries, not by nesting `if (p && p->q && p->q->r)`.
5. **No data-dependent branches in hot loops.** Sort/bucket/partition the data,
   use predication (`x = c ? a : b`), masking, or `min`/`max`.
6. **Pointer chasing is the one unforgivable sin.** Each hop is a guaranteed
   cache miss with no prefetch to save you. Store graphs as arrays (CSR).
7. **One atomic value per cell (1NF).** No arrays-in-rows, no nullable members.
8. **Optional facts become membership in another table**, not a nullable field.
9. **Write contiguously, read contiguously.** Sequential or strided only.
10. **`restrict` on every non-aliasing pointer pair**, and make independence
    visible to the compiler.
11. **Own your memory.** One allocator strategy per program region; arenas,
    pools, handles. Global `malloc` sprinkled through hot code is a bug.
12. **Measure before and after.** No optimization without a number. Use the
    `mylib` timer (§9.6).
13. **Prefer `mylib`.** If `mylib` lacks a primitive you need, add a small
    reusable primitive to `mylib` — never invent a competing allocation or
    ownership model in engine code.
14. **Do not blindly apply any of this.** Every rule is conditional on the
    workload. SIMD, SoA, branchless code and non-temporal stores all have
    losing cases. That is why rule 12 exists.

---

## 2. Mindset and workflow

### 2.1 The two mindsets

```
OOP mindset                          DOD mindset
-----------                          -----------
WHAT is this object?                 WHAT DATA exists?
What does it own?                    What TRANSFORMS operate on it?
What methods does it have?           Which data actually needs each transform?
Who calls those methods?             How should that data be stored?
```

### 2.2 The transformation

```
MONOLITHIC OBJECT
      ↓  explode into elements
IDENTIFY DATA + DEPENDENCIES
      ↓
COMPONENTS / TABLES
      ↓
SYSTEMS / TRANSFORMS
      ↓
PROCESS ONLY ACTIVE DATA
```

### 2.3 The design questions to ask, in order

Ask these before writing a line of code:

- What data does this operation **actually need**?
- What is the **access pattern** (sequential? random? one-shot? read-only?
  read-write? write-only?).
- What is the **lifetime** (per frame? per level? permanent?).
- What **can be derived** instead of stored?
- What **can be grouped** (many identical things → one record + count)?
- What **can be regenerated** from a seed instead of persisted?
- What **does not need to exist individually** right now?

### 2.4 The optimization loop (always closed with a re-measure)

```
DEFINE   → what is slow? what is the target?
MEASURE  → get a number (MU_SCOPE_TIMER / perf / timings)
ANALYSE  → read the data shape; read the generated assembly of the hot 5%
IMPLEMENT→ change data layout / algorithm (not micro-tweaks first)
CONFIRM  → measure again; keep only if the real workload improved
```

Never skip the last step. "Our benchmark proved the clever fused-loop version
was slower, because of cache behaviour" is a real and common outcome.

### 2.5 What to prioritize when optimizing

```
1. Memory bandwidth
2. Cache locality
3. Contiguous data access
4. Reduced pointer chasing
5. Batch processing
6. Predictable control flow
7. Reduced branches
8. Instruction count
```

Optimize **data movement before instruction count**. The compiler can remove
instructions; it cannot fix a bad layout.

---

## 3. Data layout — the core of the craft

### 3.1 Tables, not objects

```c
/* WRONG: a struct is a category box, and the CPU has to drag all of it around */
typedef struct Enemy {
    Vec3    position;
    Vec3    velocity;
    float   health;
    float   cooldown;
    char    name[64];
    struct Enemy *target;      /* pointer chasing */
    bool    dead;
    bool    visible;
} Enemy;

/* RIGHT: parallel arrays indexed by id; each system sees only its own data */
typedef uint32_t EntityId;

Vec3   positions[MAX_ENTITIES];
Vec3   velocities[MAX_ENTITIES];
float  health[MAX_ENTITIES];
```

A struct with 20 members is five systems pretending to be one type. **Split it
by consumer, not by category.**

### 3.2 SoA vs AoS — decide from the access pattern

- **SoA** (structure of arrays) when fields are processed **separately** and
  the loop is a stream. Enables SIMD, smaller working sets, fewer wasted bytes.
- **AoS** (array of structs) when fields are **always consumed together** as a
  unit (e.g. `Vec3 x,y,z` used as one vector; a config row; a file header).

```c
/* SoA: physics only touches position + velocity */
for (size_t i = 0; i < count; ++i)
    positions[i] += velocities[i] * dt;

/* AoS: the whole record is the unit of work */
for (size_t i = 0; i < header_count; ++i)
    parse_header(&headers[i]);
```

The animation-keyframe rule: **binary-search keys in a packed array; index the
payload once.** Searching `{time, x, y, z}` loads 3 fields you do not need.

```c
typedef struct { float x, y, z; } KeyData;

typedef struct {
    float   *times;    /* only this is searched  */
    KeyData *values;   /* touched once after the search lands */
    int      count;
} AnimStream;
```

### 3.3 Hot / cold split

Fields touched every frame and fields touched at spawn/save time belong in
different tables joined by id. This shrinks the hot working set.

```c
/* hot: 20 bytes/entity */
Vec3  positions[MAX_ENTITIES];
Vec3  velocities[MAX_ENTITIES];

/* cold: touched on spawn, save, UI */
char  names[MAX_ENTITIES][32];
uint32_t model_ids[MAX_ENTITIES];
```

### 3.4 One atomic value per cell (1NF)

Every field is a single atomic value; no array/list/set inside a row, no
nullable members. Optional facts become **membership in another table**.

### 3.5 Normalize the design, then denormalize the runtime

Start from one big table of everything, then normalize:

```
1NF  — atomic values only; no repeating groups.
2NF  — no partial dependencies on part of a composite key.
3NF  — no transitive dependencies (A→B→C means B belongs in its own table).
BCNF — every non-trivial dependency X→Y has X as a superkey.
```

Example: `weapon_id → weapon_type → type_description`.

```c
/* Weapon table holds only weapon-specific data */
typedef struct { uint32_t id; const char *name; uint16_t type_id; int damage; } Weapon;

/* Type table holds the shared description once */
typedef struct { uint16_t id; const char *name; const char *description; } WeaponType;
```

More tables can use **less** memory when the denormalized version has many
repeated/optional fields and nulls. But a game runtime also cares about cache
locality, branching, bandwidth, SIMD, parallelism and latency — so
**intentionally denormalize runtime data** where the hot loop needs the join
to be free. Normalization is a design tool, not a religion: let experimentation
and experience decide how far to push it.

Also: **do not store instance/state data next to definition data.** A quest's
name/description/reward belong to `Quest`; a player's progress belongs to
`PlayerQuest`. This removes an entire class of desync bugs.

### 3.6 Let structure provide identity and position

If the world already provides fixed locations — grids, cells, tiles, inventory
slots, voxels — **do not make an object for every location**. Store
`location → state`.

```c
/* 10,000 wheat tiles, zero objects, zero pointers, zero per-object lifetime */
uint8_t wheat_100x100[10000];   /* 0 = empty, 1..100 = growth */

int index = y * width + x;
int x = index % width;
int y = index / width;
```

> "The wheat doesn't have positions. The positions have wheat."

The storage representation does **not** have to match the logical geometry
(square, hex, triangle, irregular all map onto regular arrays + specialized
traversal functions).

### 3.7 If individual identity does not matter, use a count

```
N identical entities  →  1 record + count
```

```c
typedef struct { uint32_t type; uint32_t count; } ItemStack;
```

Applies to arrows, bullets, plates, resources, identical inventory items,
distant squadrons, crowds, swarms, forests.

Ask: **"How would a stranger describe this?"** They say "there is furniture,"
not "chair #1, chair #2, bookshelf #3." Match the player's mental model.

### 3.8 Separate read-only / read-write / write-only streams

Make the memory roles explicit. It documents the transform and gives the
compiler type-level independence.

```c
void integrate(/* read/write */ Vec3  *positions,
               /* read      */ const Vec3  *velocities,
               /* write     */ int   *out_dirty,
               size_t count, float dt);
```

### 3.9 Alignment, sizes, powers of two

- Prefer power-of-two capacities, then mask instead of modulo:
  `i & (cap - 1)` (a division-free, branch-free wrap).
- Give the compiler useful guarantees: aligned buffers (`MU_ALIGN`), regular
  counts (pad `count` to a multiple of 4/8). Padding a few unused elements is
  often cheaper than a scalar remainder loop.
- Keep struct sizes cache-line friendly; pad deliberately where false sharing
  or alignment matters.

### 3.10 Sorting data along the processing axis

Sort or bucket by the axis you process: position (spatial locality), update
rate (LOD buckets), type (homogeneous transforms), material (render batches).
A sorted table also makes branches predictable and joins mergeable.

---

## 4. Existence-based processing

The central DOD idea: **don't repeatedly ask "does this object do something?"
— store "these objects do something."**

### 4.1 Don't use bools as a processing filter

```c
/* WRONG: inspect every entity every update, then branch */
for (size_t i = 0; i < entity_count; ++i) {
    bool hurt  = health[i] < MAX_HEALTH;
    bool dead  = health[i] <= 0.0f;
    bool ready = current_time - last_damage[i] > REGEN_DELAY;
    if (!dead && hurt && ready)
        health[i] = MU_MIN(MAX_HEALTH, health[i] + dt * regen_rate);
}

/* RIGHT: only hurt entities are in the table at all */
for (size_t i = 0; i < damaged_count; /* ... */) {
    EntityId id = damaged_ids[i];
    if (health[id] <= 0.0f) {
        dead_ids[dead_count++] = id;
        damaged_ids[i] = damaged_ids[--damaged_count];   /* swap-remove */
        continue;
    }
    if (current_time - last_damage[id] > REGEN_DELAY) {
        health[id] = MU_MIN(MAX_HEALTH, health[id] + dt * regen_rate);
        if (health[id] >= MAX_HEALTH) {
            damaged_ids[i] = damaged_ids[--damaged_count];
            continue;
        }
    }
    ++i;
}
```

### 4.2 Represent boolean streams as bitsets

For a 2-valued property over a large domain, one bit per element.

```c
/* 1 bit/instance instead of 1 byte, and 64 tests per instruction */
uint32_t casts_shadow[(MAX_INSTANCES + 31) / 32];
uint32_t visible_mask[(MAX_INSTANCES + 31) / 32];
```

Then set operations become whole-word operations:

```c
/* things that both cast shadows and are visible */
for (size_t w = 0; w < WORDS; ++w)
    xr[w] = casts_shadow[w] & visible_mask[w];
```

Use `mylib`'s `mu_bitset` when you need growth, set algebra and iteration
(§13). Use raw word arrays when the size is fixed and you want maximum control.

### 4.3 Represent sparse states by existence

```c
/* WRONG: a dense bool array over a huge range */
bool has_health[MAX_ENTITIES];

/* RIGHT: a dense list of just the entities that have health */
EntityId health_ids[MAX_ENTITIES];
size_t   health_count;
```

This is the "sparse component" pattern. `mu_sparse_set` gives O(1)
insert/remove/contains with dense packed iteration (§13).

### 4.4 Don't use enums as control flow

```
enum used as DATA  (Color, KeyBinding, CollisionResponse, table lookup)  → keep it.
enum used as FLOW  (forces switch/branch in every consumer)              → make it DATA/array.
```

Replace `enum + switch` with **table membership + specialized transform**:

```c
/* WRONG */
for (size_t i = 0; i < n; ++i)
    update_entity(&entities[i]);       /* switch inside */

/* RIGHT */
for (size_t i = 0; i < player_count; ++i)     update_player(&players[i]);
for (size_t i = 0; i < enemy_count; ++i)      update_enemy(&enemies[i]);
for (size_t i = 0; i < projectile_count; ++i) update_projectile(&projectiles[i]);
```

Rule of thumb: **moving an entity between tables has a cost, so rapidly
changing states may still be better as an enum.** Bucket by type once when it
is stable; keep the enum when it flips every tick.

### 4.5 Polymorphism lives in the data layout

```
object → type enum → switch → function          (still runtime dispatch)
object → vptr → vtable → virtual function       (still runtime dispatch)

DOD:  object → table membership → specialized transform
```

Type information becomes implicit in **table membership**; a polymorphic method
becomes a **specialized transform over a table**; a class constructor becomes a
**data-driven factory** that inserts rows into the relevant tables.

```c
EntityId create_enemy(/* ... */)
{
    EntityId id = new_entity();
    enemy_table_add(id);
    health_add(id, 100.0f);
    physics_add(id, pos);
    ai_add(id, AI_HOSTILE);
    return id;
}
```

An entity may belong to **many** tables at once — multidimensional composition,
similar to ECS. Because you reference entities by **ID**, they can move between
tables without changing logical identity. Event subscription uses the same
mechanism: subscribe = insert into an event table; unsubscribe = delete;
dispatch = process the subscribed dataset (and stop polling everything).

### 4.6 Entity = identity, Component = data, System = processing

```
Entity     = identity only
Component  = data
System     = processing
```

`systems` own the work. Rename thinking from `player_update()` to
`physics_manager_update()`, `animation_manager_update()`, `render_manager_update()`.

The best ECS model: **for each component, update its objects** — not for each
object, update its components.

```
Instance-oriented (weaker):
    Object 1 → {Transform, Physics}
    Object 2 → {Transform, Physics, Render}
    Object 3 → {Transform, Render}
    → processing order determined by instances

Type/system-oriented (stronger):
    Transform table [1,2,3]
    Physics   table [1,2]
    Render    table [2,3]
    → processing order determined by systems
```

Why it matters for profiling: with 500,000 entities but only 50,000 with
physics, we process 50,000 rows — and the profiler says
`PhysicsSystem = 2.4 ms` instead of mixing physics into one giant loop.

### 4.7 An entity is derived from its component data

```c
typedef uint32_t EntityId;

typedef struct { EntityId id; Vec3  position; } Position;
typedef struct { EntityId id; Vec3  velocity; } Velocity;
typedef struct { EntityId id; float health;   } Health;

/* Entity 42 is simply: 42 → Position + Velocity + Health */
```

Do not think of an entity as a class containing components. Think of it as the
**result** of the component data currently associated with its ID. No `Player`
struct is required.

### 4.8 Never represent state with more bits than it needs

- Do not use more bits/fields/precision/structure than the information requires.
- Pack small bounded values into bit fields when it improves bandwidth.
- Process at the granularity the representation supports:

```
bitset   → word/SIMD operations
packed   → vectorized processing
sparse   → affected elements only
events   → changed elements only
```

---

## 5. Branch discipline

### 5.1 A branch is only a problem when it is unpredictable

```
bad:      random branching (data-dependent, 50/50)
better:   predictable runs of true/false (sorted / bucketed / grouped data)
best:     no branch at all (predication, masking, table membership)
```

A mispredict is typically ~15–20 cycles of pipeline disruption. If the
condition is data-dependent (keys, hashes, random values), prefer sorted or
bucketed data, arithmetic selects, or masking.

### 5.2 Sorting data can make branches predictable

```c
for (size_t i = 0; i < n; ++i)
    if (a[i] > 128) sum += b[i];
```

`a[]` shuffled → `T F T F F T T F …` (hard). `a[]` sorted → `T T T T … F F F`
(easy). Same algorithm, different data order, different performance.
**Caveat:** the compiler may already have removed the branch (CMOV / SIMD
select). Inspect the assembly and measure before leaning on this.

### 5.3 Predication — compute both, select one

```c
/* branch */
if (a > b) lo = b, hi = a; else lo = a, hi = b;

/* branch-free */
hi = MU_MAX(a, b);
lo = MU_MIN(a, b);
```

`x = condition ? A : B` lets the compiler emit CMOV / SIMD select / min / max.
Sometimes doing a little extra arithmetic is faster than an unpredictable
branch. Sorting networks are the classic example: a **fixed** sequence of
compare-exchanges that does not depend on the input.

```
Sort 4:  (A,C) (B,D) → (A,B) (C,D) → (B,C)     ← fixed pattern, branch-free
```

### 5.4 Hint the cold path, never the hot one

```c
if (MU_UNLIKELY(!init_renderer()))     /* error handling, first-time init */
    return false;
```

Do not sprinkle `MU_LIKELY` on genuine 50/50 branches.

### 5.5 Sometimes computing everything is cheaper than deciding

```
if (dirty) update();          /* load flag, branch, possibly mispredict */
update();                     /* if update is cheap, this may win outright */
```

And existence-based replacement: `dirty == true` becomes `entity ∈ DirtyTable`,
and you update the table with no check at all. The recurring question is
**"is avoiding the work cheaper than doing the work?"** — sometimes it is not.

### 5.6 Group similar work into bursts

```
instead of:  A B C A B C A B C …
do:          A A A A A   B B B B B   C C C C C
```

Homogeneous bursts keep code and data hot and branches regular. Batch by type
(or by `vtable` pointer — a vtable pointer is data too, and a stream of them is
a predictable branch if batched).

### 5.7 Log decisions for debugging

When control flow is data-driven, keep a small log of *why* a decision was
taken (which condition matched, which transition fired). This turns "why is
this AI attacking?" into "here is the transition record."

---

## 6. Memory, cache and the machine

### 6.1 The memory hierarchy is the real medium

```
L1   ~4 cycles     ~32–48 KB
L2   ~12 cycles    ~256 KB–1 MB
L3   ~40 cycles    MBs
DRAM ~80–300 cycles
```

Every memory request brings in a whole **cache line (commonly 64 bytes)** —
that is 16 ints, 8 doubles, 16 floats, or 512 bits. **Use the rest of the
line.** Load useful metadata together with data you already had to fetch.
Store per-entity presence masks next to the entity so one line answers many
"does it have X?" questions.

Cache levels of caching, cheapest to best:

```
no cache → cache presence → cache partial query → cache complete result
```

### 6.2 Pointer chasing vs arrays

```c
/* BAD: next address depends on current data; no prefetch possible */
for (Node *p = head; p; p = p->next)
    process(p);

/* GOOD: addresses are predictable — address(i) = base + i * stride */
for (size_t i = 0; i < count; ++i)
    process(&nodes[i]);
```

The problem is not "pointers are slow." The problem is
**the next address is unknown until the previous load completes**:

```
previous result → next address → next load    (a serial chain, no MLP)
```

For graphs, use CSR (offsets + edge array). For trees, prefer **wide nodes**
(B-trees / B*-trees) — many keys per node, fewer hops, shallower tree, more
useful data per cache line. Reduce the number of dependent hops everywhere:

```
entity → component manager → component pointer → component → target   (BAD)
entity_id → direct table/index → data                                  (GOOD)
```

### 6.3 Memory-level parallelism (MLP)

Independent loads can be in flight simultaneously. Break dependency chains and
let several misses overlap (software prefetch, unrolled independent streams,
`MU_PREFETCH`). One accumulation must be serial; **reductions should use
multiple accumulators**.

### 6.4 Aliasing, `restrict`, and why the compiler goes conservative

```c
/* The compiler must assume p and q may be the same memory. */
void foo(int *p, const int *q, int n)
{
    for (int i = 0; i < n; ++i)
        p[i] = *q;            /* *q reloaded every iteration! */
}
```

`const` prevents modification *through that reference*; it does **not** prove
non-aliasing. Fix it with `restrict` and explicit ownership:

```c
void foo(int * MU_RESTRICT p, const int * MU_RESTRICT q, int n);
```

```
possible aliasing  → unknown side effects → fewer optimizations
known non-aliasing → register reuse, reordering, unrolling, vectorization
```

### 6.5 Return by value — small structs are cheap

Modern compilers construct the return value directly in the caller's storage
(RVO). Prefer `Vec3 add(Vec3 a, Vec3 b)` over `void add(const Vec3*, const Vec3*, Vec3 *out)`.
Value-oriented code makes dependencies explicit and avoids aliasing and
out-parameter bugs. C caveat: returning a *large* struct by value on a
small/embedded ABI may still copy — check the assembly for big structs.

### 6.6 False sharing

```
T0 → counter[0] ─┐
                  ├─ same cache line → cache-coherence traffic
T1 → counter[1] ─┘
```

Pad rapidly-modified per-thread data to cache lines:

```c
typedef struct { int64_t count; char _pad[56]; } MUCacheLineAligned;  /* 64B */
```

and aggregate locally, then do one shared update.

### 6.7 Non-temporal / streaming stores

Data written once and not read again soon does not deserve cache capacity.
Streaming stores bypass the cache to avoid **cache pollution** (evicting your
useful animation/AI/player data). Prefer normal stores for anything reused
soon. First write the transform in a simple, predictable form so the compiler
can spot the streaming opportunity; reach for intrinsics only when it cannot.

### 6.8 Prefetching and paging

- Software prefetch (`MU_PREFETCH`) works when you know future addresses and
  they are not already in cache. Over-prefetching wastes bandwidth.
- Random access over data larger than RAM thrashes the TLB. Keep hot working
  sets page-friendly; block/tile algorithms for out-of-cache sizes.
- **Cache-oblivious and blocked algorithms** beat naive nests when the working
  set exceeds L2. Before blaming hardware, restructure the loop nest.

### 6.9 Keep hot code and data small

Hot code evicts hot code; hot data evicts other hot data. Keep the hot 5%
small by separating hot/cold data (§3.3) and batching work (§5.6).

### 6.10 Be aware of speculation

`if (has_component) load_component_data;` may speculatively load data that
turns out to be unnecessary — wasted bandwidth on top of an unpredictable
branch. Predicate caching ("does this query return anything?") is cheap and
prevents the wasted load.

---

## 7. SIMD and auto-vectorization

### 7.1 Let the compiler vectorize first

```c
/* vectorizable: independent, contiguous, no early exit */
for (size_t i = 0; i < n; ++i)
    a[i] *= mult;
```

Check with `gcc -O3 -fopt-info-vec` / clang `-Rpass=loop-vectorize`. Only write
intrinsics for structural transforms the compiler cannot infer (lane masking,
pair folding, shuffles).

### 7.2 What breaks vectorization

```
- data-dependent loop exit:  if (a[i] < 0) break;      ← loop length depends on data
- unknown function calls in the loop (side effects)
- possible aliasing between input and output pointers
- cross-iteration dependencies:  a[i] += a[i+1];
- unaligned pointers
- partial writes to the output stream
```

Note the asymmetry: a branch that lets every iteration **complete** can still
vectorize (the compiler emits a select):

```c
for (size_t i = 0; i < n; ++i) {
    float v = a[i] * mult;
    a[i] = (v > 0.0f) ? v : 0.0f;      /* select, not control flow */
}
```

### 7.3 Make scalar code vectorizer-friendly before writing intrinsics

Simple loop, no early exits, contiguous access, inlined callees, `restrict`,
regular counts. Write the whole output stream rather than partially updating it.

### 7.4 SIMD is not automatically faster

Vectorization wins when there is enough data, independent operations, a
suitable layout, and vector-friendly instructions. On tiny workloads, SIMD
setup and remainder handling can lose. Measure.

### 7.5 SoA enables SIMD because it makes streams explicit

```
positions[] + velocities[] → MovementSystem → positions[]
```

One vector instruction operating on `[x0 x1 x2 x3]` instead of four scalar ops.

---

## 8. Concurrency

### 8.1 Don't make shared memory safe — design so sharing isn't required

```
SHARED MUTABLE STATE:            DATA-ORIENTED:
T0 ─┐                            T0 → data A
T1 ─┼→ same memory → lock        T1 → data B
T2 ─┘                            T2 → data C
                                     ↓
                                   Reduce
```

### 8.2 Local accumulators, one final combine

```c
int64_t local_sum = 0;
for (size_t i = begin; i < end; ++i)
    local_sum += calc(i);

mu_ts_atomic64_add_relaxed(&global_sum, (uint64_t)local_sum);
```

### 8.3 The concurrency rules

```
1.  Prefer one writer per piece of data.
2.  Separate independent transforms.
3.  Avoid shared mutable state.
4.  Use local accumulators and reduce later.
5.  Keep worker outputs separate (thread-local buffers, then gather).
6.  Use queues/gateways for variable-size output.
7.  Batch synchronization at stage boundaries.
8.  Avoid false sharing (§6.6).
9.  Keep frequently modified data on separate cache lines.
10. Use immutable/old versions for readers when possible.
11. Think in terms of data ownership, not locks.
12. Design for arbitrary N workers, not today's core count.
```

### 8.4 Patterns

```
MAP:              input[N] → independent transforms → output[N]
FILTER:           input[N] → tests → local outputs → gather (prefix sum)
VARIABLE OUTPUT:  workers → private queues → gather/reduce
MULTI-STAGE:      Stage A → Stage B → Stage C, independent within each stage
```

Prefix sum for compaction:

```
input:   A B C D E F
keep:    1 0 1 1 0 1
offsets: A→0  C→1  D→2  F→3
final:   A C D F
```

Parallel reduction works because addition/multiplication/matrix-multiply are
**associative**: `(A+B) + (C+D)` has O(log N) depth instead of O(N).

### 8.5 Lifetime in multithreaded code

Prefer **thread-local ownership** (assert `current_thread == owning_thread`;
compile the check out in release) over reference counting. Prefer **inactive
over deleted** for entities: keep the storage, mark it free, reuse it later.

```
deleted:  memory no longer exists
inactive: memory still exists, entity is not processed
```

This separates **memory lifetime** from **logical existence**, can be
represented by an `active_count`, a freelist, or inactive table entries, and
removes allocation churn plus cross-thread free hazards.

### 8.6 Use the library

`mu_thread_system.h` (worker pool + atomics) and `mu_task_scheduler.h`
(priority queues, task groups, `mu_sched_parallel_for`) are the sanctioned
backends. Do not hand-roll a thread pool per subsystem.

---

## 9. Allocation and lifetime

### 9.1 One allocator strategy per program region

Global `malloc`/`free` in hot code is a design failure mode. Make lifetime
questions visible.

- **Frame arena** — allocate per frame, free everything by resetting a pointer.
- **Level/scope arena** — bulk allocation, bulk free.
- **Fixed pool** — same-size objects, freelist recycling.
- **Buddy / offset allocator** — general sizes with coalescing.
- **Static / fixed-capacity arrays** — zero allocation, zero surprises.

> If your game frame produces garbage, don't free — rewind.

### 9.2 Prefer static memory, but do not blindly make everything static

Excessive static allocation wastes memory and creates arbitrary capacity
limits. Choose per-region. Use `enum { MAX_X = ... }` next to the table.

### 9.3 Treat the stack as limited

Avoid large local allocations, oversized temporary structs, and unnecessarily
deep call chains. Stack overflows are hard to trace. Big scratch buffers belong
in an arena or a static buffer, not on the stack.

### 9.4 Handles with generations

```c
typedef struct { uint32_t id; uint32_t generation; } Handle;
```

A stale reference becomes a **stale handle that fails a compare** instead of
corrupting memory. `mu_bulk_storage` implements this (§13). Offsets over
pointers wherever identity must survive serialization or realloc.

### 9.5 O(1) deletion and reuse

```c
/* swap-remove: order need not be stable inside a table */
table[i] = table[--count];
```

Keep order only where iteration order matters; otherwise swap-remove.

### 9.6 Measure allocation and timing

```c
#include "mu/mu_perf.h"

MU_SCOPE_TIMER("integrate");
```

or wrap a block with `mu_time_now()` before/after in nanoseconds. Timings live
next to the code they measure so re-measuring is one line.

### 9.7 Avoid hidden allocation

No allocation behind iterators, virtual calls, or "just a helper." Allocation
points must be visible in the calling code.

---

## 10. Searching and sorting

### 10.1 Do not search when the answer can be derived

If a value is sorted, indexed, directly addressable, or in a fixed slot, a
separate search is unnecessary. **The biggest optimization is avoiding
unnecessary work.**

### 10.2 Build indexes for hot repeated queries, not speculatively

```
cold query → linear search
repeated query → detect → build index → maintain on insert/update/delete
```

Only index data that actually becomes a hotspot. Runtime telemetry tells you
which queries requested indexes — so indexing infrastructure is also profiling
infrastructure.

### 10.3 Complexity does not equal performance

```
O(log N) with poor locality  can lose to  O(N) with excellent locality
```

for small enough ranges. A binary search jumping across memory can lose to a
linear scan through cache. When you load one cache line, use as much of it as
possible.

### 10.4 Bloom filters for cheap existence

```
possibly present (may be a false positive) | definitely absent (never a false negative)
```

```c
if (!bloom_maybe_contains(&health_bloom, id))
    continue;                       /* skip the expensive lookup */
/* ...otherwise do the real lookup... */
```

Use the `mu` hashing helpers rather than rolling your own.

### 10.5 Repeated "find best" becomes a maintained subset

Need nearest 3? Maintain `best[3]` and update it when the data changes, instead
of sorting everything every frame.

### 10.6 Choose the data structure by workload

```
frequent modifications   → hash table
mostly static data       → tree
very static data         → perfect hash
```

### 10.7 Sorting: sort only as much as necessary

```
1.  Do not sort unless sorting is required.
2.  Partition data before sorting when groups need different orders.
3.  Sort only as much as necessary.
4.  Partial sort when only the top/bottom N matters.
5.  Quickselect when only the Nth boundary matters.
6.  Use unstable operations when order is irrelevant.
7.  Maintain sorted data incrementally when updates are small (insertion sort).
8.  Radix/counting sort for suitable discrete ranges.
9.  Merge sort / parallel-friendly algorithms for large data.
10. Sorting networks for small fixed-size datasets.
11. Prefer cache-friendly algorithms over comparison-minimal ones.
12. Measure the real workload.
13. More threads ≠ proportionally more performance (bucket contention).
14. Sometimes computing everything beats branching to decide.
```

Discrete, known-range keys (material IDs, shader IDs, buffer indices) suit
**counting/radix sort**; continuous/analogue ranges suit comparison sort.

```c
#define MAX_MATERIALS 256u
void counting_sort_material(RenderCall *src, RenderCall *dst, uint32_t n)
{
    uint32_t bucket[MAX_MATERIALS] = {0};
    uint32_t offset[MAX_MATERIALS];
    for (uint32_t i = 0; i < n; ++i) ++bucket[src[i].material_id];
    uint32_t sum = 0;
    for (uint32_t m = 0; m < MAX_MATERIALS; ++m) { offset[m] = sum; sum += bucket[m]; }
    for (uint32_t i = 0; i < n; ++i) dst[offset[src[i].material_id]++] = src[i];
}
```

### 10.8 Rendering sort order (a concrete example)

- Sort **opaque** front-to-back: depth rejection kills overdraw before the
  fragment shader runs (`A` writes depth; `B` is rejected if covered).
- Sort **transparent** back-to-front: blending depends on draw order.
- Opaque cost is roughly `texture_cost + mesh_cost + state_change_cost +
  overdraw_cost` — measure the profile and choose the sort key. Support
  multiple criteria and pick per scene.
- Sort sound channels by amplitude relative to sample position for priority.

### 10.9 Keep search data separate from result data

Index/payload split keeps the searched structure small. Then joins become
intersections of sorted/posted lists:

```
EntityRenderable { entityID }
EntityPosition   { entityID }
join: entityID in both → AddRenderCall()
```

Repeated joins justify an index or a cached join result.

---

## 11. Simulation: state, LOD and memory of the world

### 11.1 State is data

```
Traditional:  state variable → branch → behaviour
DOD:          table membership → processing set → behaviour
```

```c
EntityId idle[MAX_ENTITIES];    size_t idle_count;
EntityId walking[MAX_ENTITIES]; size_t walking_count;
EntityId jumping[MAX_ENTITIES]; size_t jumping_count;
/* entity 42 is walking iff 42 ∈ walking[] */
```

Transition = remove from one table, insert into another.

### 11.2 Evaluate, then commit (transform-oriented FSM)

```
old_state + signals
      ↓
  condition tables
      ↓
 transition_requests
      ↓
   triggers / logging
      ↓
     commit
      ↓
  new_state
```

```c
typedef struct { EntityId entity; int next_state; } Transition;
Transition transitions[MAX_ENTITIES]; size_t transition_count;

/* evaluation: reads old state, writes requests */
for (size_t i = 0; i < walking_count; ++i) {
    EntityId id = walking[i];
    if (jump_pressed(id))
        transitions[transition_count++] = (Transition){ id, STATE_JUMPING };
}

/* commit: single mutation point */
for (size_t i = 0; i < transition_count; ++i)
    apply_transition(transitions[i].entity, transitions[i].next_state);
```

Benefits: deterministic update step, no signal-order dependence, easy
transition logging/telemetry, and state tables double as processing sets
(`Damaged[] → DamageSystem`, `Burning[] → FireSystem`,
`PotentiallyVisible[] → CullingSystem`).

Limitations: when ordering between signals is *fundamentally* part of the
problem (a lexer/parser), a traditional sequential FSM is correct. Use the
transform model when signals can be evaluated together and committed after.

### 11.3 LOD is a function of *relevance*, not distance

```
LOD = f(relevance)
relevance = f(distance, screen-space size, visibility, player attention,
              time since last seen, interaction history, progression,
              player expertise, gameplay importance)
```

LOD controls far more than polygons:

```
LOD 0: full physics, full AI, animation, weapons, high-res mesh
LOD 1: simplified physics/AI, low-res animation, medium mesh
LOD 2: approximate movement, 1 Hz AI, billboard
LOD 3: no individual aircraft — squadron data
LOD 4: no squadron entity — wave data
```

```
far  + irrelevant → low detail
far  + important  → high detail
near + irrelevant → medium detail
near + important  → high detail
```

Interaction history matters: an ambient car the player never touched may
regenerate from a seed; the player's parked car must retain its state.

### 11.4 Reduce instance count before optimizing instances

```
Collective LOD:  do these things need to exist as separate objects at all?
```

```
100 aircraft → 1 Squadron { count = 100, attack_probability }
```

```c
typedef struct { int aircraft_count; float attack_probability; } Squadron;
int simulate_attack(const Squadron *s)
{
    return (int)(s->aircraft_count * s->attack_probability);   /* believable, not exact */
}
```

The escalation chain:

```
full entity → simplified entity → collective → aggregate/count → implicit/procedural
```

### 11.5 Mementos — preserve what matters, discard precision

A memento is the **compressed historical information required for future
refinement**, not "save data":

```
High-detail state
   ├── important information ──► Memento
   ▼
Low-detail state

later:  Low-detail state + Memento → High-detail state
```

Lossy is allowed: `2/3 of windows are broken` instead of listing every window.
The player perceives consistency; they do not audit history.

### 11.6 JIT mementos — don't store derivable history

```
Ordinary memento: detail → store compressed memory → rebuild
JIT memento:      store a recipe (seed) → generate on demand
```

```c
static uint64_t hash64(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

uint64_t seed = hash64(car_id);
driver       = generate_driver(seed);
passenger[0] = generate_passenger(seed + 1);
```

The same seed gives the same result every visit, so the identity itself carries
the memory. Global RNG (`R_{t+1} = f(R_t)`) is wrong here — it depends on
everything that happened before. Use `R = f(seed)`.

### 11.7 Procedural generation wants bias, not uniform randomness

```
believable world = repetition + variation + imperfections
                   structured bias × randomness
```

```c
/* P(driver | convertible) = young-and-fashionable-biased,
   not "convertible MUST have a young driver" */
```

### 11.8 Derived state beats duplicated state

```c
/* WRONG: two booleans that can disagree */
bool in_options; bool in_volume_menu;

/* RIGHT: derive from one axis */
int menu_depth;
bool options_visible = menu_depth >= 1;
bool volume_visible  = menu_depth >= 2;
```

`state = f(other_state)` is safer than maintaining many independent flags.
Explicit state machines multiply transitions and inconsistent states; an
axis-based representation (`state = f(axis)`) shrinks the transition surface.

---

## 12. C style mechanics (my style)

### 12.1 Naming

```
Types:              PascalCase         (EntityId, RenderTarget)
Functions:          snake_case         (physics_update, array_push)
Variables/fields:   snake_case         (mesh_id, visible_count)
Constants/macros:   SCREAMING_CASE     (MAX_ENTITIES, MU_ALIGN_UP)
```

Align related declarations so the table scans vertically (Carmack style):

```c
typedef struct {
    TextureId  texture_id;
    MeshId     mesh_id;
    MaterialId material_id;

    uint32_t   width;
    uint32_t   height;
    uint32_t   stride;
} DrawItem;
```

### 12.2 Declarations at the top of the function

C89-style, block-visible, one place to see everything the function owns.

```c
static bool physics_step(Physics *p, const Input *in, float dt)
{
    EntityId id;
    Vec3      accel;
    size_t    i;

    /* ... */
    return true;
}
```

### 12.3 IDs, not long-lived pointers ("weak pointers")

```c
/* WRONG */
Texture *texture;  Mesh *mesh;  Material *material;

/* RIGHT */
TextureId  texture;
MeshId     mesh;
MaterialId material;
```

### 12.4 Static or arena-backed storage, with care

Prefer explicit, predictable allocation. Do not make everything static
(§9.2), and do not overfill the stack (§9.3).

### 12.5 Design out null checks

Do not eliminate null checks by ignoring errors — eliminate them with
**stronger invariants**.

```c
/* avoid this */
Texture *t = get_texture(id);
if (t) { if (t->image) { if (t->view) { /* ... */ } } }

/* prefer: resolve and validate at the boundary, then pass valid data in */
```

If an operation can genuinely fail, make failure **explicit** through a boolean
result, an invalid ID, an error value, or an output parameter. The goal is not
"fewer null checks," the goal is **stronger contracts**.

### 12.6 Functions and APIs

- Prefer meaningful, substantial functions. Do not split code into tiny
  functions for style alone. Extract a function when it gives meaningful reuse,
  a useful boundary, or easier testing.
- Prefer APIs that make misuse difficult and that state ownership, lifetime,
  validity and failure semantics.
- Resolve/validate resources at system boundaries, not in inner loops.
- Systems are free functions over tables:
  `void physics_update(PhysicsTable *p, const InputTable *in, float dt);`
- Pass state explicitly: `render_frame(Renderer *r, Frame *f);` — no hidden
  global access. Minimize global state and side effects.
- Separate preparation from execution; push validation to boundaries and keep
  inner loops clean.

### 12.7 Compiler flags and invariants

```sh
cc -std=c99 -O2 -Wall -Wextra -Werror -fno-strict-aliasing -march=native \
   -D_POSIX_C_SOURCE=199309L      # needed by mu_perf.h on glibc
```

- `-O3` / `-march=native` when you have measured; verify with `-fopt-info-vec`.
- `assert` / `MU_ASSERT` for table invariants in debug builds only.
- `MU_STATIC_ASSERT` for layout facts (struct sizes, power-of-two caps).
- No `int` in serialized structs — fixed-width integers everywhere.

### 12.8 Comments

Explain **why**, not what. A comment restating the code is noise; a comment
explaining an invariant, a tradeoff, or a hardware reason is gold.

### 12.9 Code quality checklist

```
- control flow obvious
- ownership obvious
- lifetimes predictable
- no hidden work, no hidden allocation
- no unnecessary indirection
- no unnecessary abstraction
- simple mechanisms with measurable cost
- debug-only assertions for invariants
```

---

## 13. `mylib` (`mu`) — the standard library. Use it.

**Rule:** prefer `mylib` for data structures and allocators. If `mylib` lacks a
primitive that the data-oriented design needs, **add a small reusable primitive
to `mylib`** (e.g. under `mylib/mu/`) rather than introducing an ad-hoc
container or a competing allocation/ownership model. External libraries should
not dictate engine architecture.

Include the core with the umbrella header:

```c
#include "mu.h"        /* containers, allocators, bitsets, spans, RNG, perf */
```

Subsystems are separate single headers:

```c
#include "mu_ecs.h"               /* entities, pools, transforms, prefabs */
#include "mu_thread_system.h"     /* worker pool + atomics                 */
#include "mu_task_scheduler.h"    /* priorities, groups, parallel_for      */
#include "allocators.h"           /* arena, pool, buddy                    */
#include "offset_allocator.h"     /* two-level bin free-space allocator    */
#include "mu_mmgr.h"              /* leak/corruption tracking malloc shim   */
#include "mu_min_containers.h"    /* standalone stretchy buffer + hash64    */
#include "mu_geometric_algebra.h" /* PGA / conformal math (needs cglm)      */
#include "mu_noise_math.h"        /* value/perlin/simplex/worley/fBm/warp   */
#include "mu_roguelike.h"         /* grid gen, FOV, pathing, tick scheduler  */
```

### 13.1 Pick by access pattern

| Need                        | Use                              |
|-----------------------------|----------------------------------|
| Dense hot iteration         | `array` (`mu_array.h`) / frame arena |
| Non-owning view             | `mu_span` (`mu_span.h`)          |
| Stable handles + holes      | `mu_bulk_storage`                |
| Dense active IDs in a range | `mu_sparse_set`                  |
| key → one value             | `mu_hash32` / `mu_hash64`        |
| key → many values           | `mu_multi_index`                 |
| Packed booleans             | `mu_bitset`                      |
| Ranged / contiguous ID alloc| `mu_id_pool`                     |
| Variable children per key   | `mu_chunked_u32_array`           |
| Packed strings              | `mu_string_arena`                |
| Intrusive lists             | `mu_pool_link` / `mu_index_list` |
| Length index / cumulative offsets | `mu_length_index`          |
| Bit/quantization packing    | `mu_bitpacking.h`                |

### 13.2 The pieces you will actually use

```c
/* ---- arrays: header-prefixed stretchy buffer ------------------------- */
float *a = NULL;                 /* NULL is a valid empty array */
array_push(a, 1.0f);
array_size(a);  array_capacity(a);  array_reserve(a, n);
array_pop(a);   array_clear(a);     array_free(a);

/* ---- spans: non-owning views ---------------------------------------- */
mu_span_f32 s = mu_span_f32_make(v, 4);      /* typed alias: count = elements */
mu_span     b = mu_span_from_array(v);       /* plain mu_span: count = bytes  */
mu_span_f32_find(s, v);  mu_span_f32_sub(s, 1, 3);  mu_span_f32_copy_to(s, dst, n);

/* ---- bitsets: packed 64-bit words, full set algebra ------------------ */
mu_bitset *bs = mu_bitset_create_with_capacity(130);
mu_bitset_set(bs, 63);  mu_bitset_test(bs, 63);
mu_bitset_inplace_union(bs, other);
size_t i = 0; while (mu_bitset_next_set_bit(bs, &i)) { /* member */ ++i; }
mu_bitset_for_each(bs, visitor, user);

/* ---- sparse set: O(1) membership over [0, capacity) ------------------ */
mu_sparse_set_init(&set, MAX_ENTITIES);
mu_sparse_set_insert(&set, 42);
mu_sparse_set_contains(&set, 42);
mu_sparse_set_remove(&set, 42);          /* swap-delete, dense iteration */
for (uint32_t k = 0; k < set.count; ++k) { uint32_t v = mu_sparse_set_at(&set, k); }

/* ---- bulk storage: slots with holes + generational handles ---------- */
mu_bulk_storage_init(&st, sizeof(MyItem), 64);
uint32_t id = mu_bulk_storage_alloc(&st);
MyItem  *it = (MyItem *)mu_bulk_storage_ptr(&st, id);
mu_weak_handle h = mu_bulk_storage_make_handle(&st, id);
MyItem *live = (MyItem *)mu_bulk_storage_resolve_handle(&st, h);  /* NULL if stale */

/* ---- hashes: fixed-capacity, no hidden growth ----------------------- */
mu_hash32_t h; mu_hash32_init(&h, 1024);
uint32_t out; mu_hash32_get(&h, key, &out);      /* true/false, no magic 0 */
mu_hash32_set(&h, key, value);  mu_hash32_remove(&h, key);

/* ---- id pool: single ids or contiguous ranges ----------------------- */
mu_id_pool_init(&pool, 4096);
uint32_t first; mu_id_pool_create_range_id(&pool, &first, 16);

/* ---- multi index: one key → many values ----------------------------- */
mu_multi_index_init(&idx, node_cap, map_cap);
uint32_t first = mu_multi_index_first(&idx, key);
mu_multi_index_add(&idx, key, value);
for (uint32_t n = first;
     mu_multi_index_node_valid(&idx, n);
     n = mu_multi_index_next(&idx, first, n))
{ uint32_t v = mu_multi_index_value(&idx, n); }

/* ---- chunked u32 arrays: many small variable lists ------------------ */
mu_chunked_u32_array_push(&poolc, &arr, 42);

/* ---- allocators ----------------------------------------------------- */
mu_arena arena; mu_arena_alloc(&arena, size, align); mu_arena_reset(&arena);
mu_pool  p;     mu_pool_init(&p, buf, blk, n);  mu_pool_alloc(&p); mu_pool_free(&p, ptr);
mu_buddy bb;    mu_buddy_init(&bb, arena_mem, arena_size, leaf, meta, meta_size);

/* ---- RNG: deterministic, seedable (PCG) ----------------------------- */
mu_pcg32 rng; mu_pcg32_init(&rng, seed, seq);
uint32_t r = mu_pcg32_next_u32(&rng);

/* ---- perf ----------------------------------------------------------- */
MU_SCOPE_TIMER("physics");
```

### 13.3 `mu_common.h` macros worth knowing

```c
MU_ASSERT(cond);            MU_PANIC();
MU_INLINE;  MU_NOINLINE;    MU_RESTRICT;      MU_ALIGN(N);
MU_LIKELY(x); MU_UNLIKELY(x); MU_PREFETCH(addr);
MU_MIN(a,b); MU_MAX(a,b);   MU_CLAMP(x,lo,hi);  MU_SWAP(T,a,b);
MU_ARRAY_COUNT(a);  MU_IS_POW2(x);  MU_ALIGN_UP(x,a);  MU_ALIGN_DOWN(x,a);
MU_KB/MB/GB(x);  MU_CEIL(x,y);
MU_BIT(n); MU_HAS_FLAG/SET_FLAG/CLEAR_FLAG;
MU_STATIC_ASSERT(cond, msg);
mu_trailing_zeroes_u64(x);  mu_leading_zeroes_u64(x);  mu_popcount_u64(x);
```

### 13.4 `mu_ecs.h`

30-bit-ish entities (index + generation), component pools backed by dense
arrays + an entity→dense map, SoA transforms with **index**-based hierarchy
links, and prefabs/resource registry. Key calls: `mu_ecs_entity_manager_init(&em, min_free)`,
`mu_ecs_entity_create(&em)`, `mu_ecs_entity_destroy(&em, e)`,
`mu_ecs_pool_init(&p, elem_size)` / `mu_ecs_pool_add/get/remove(&p, e)`,
`mu_ecs_transform_init(&t)` / `mu_ecs_transform_add(&t, e)` /
`mu_ecs_transform_set_local(&t, inst, m16)` / `mu_ecs_transform_propagate(&t, root)`.
Use this before writing a new ECS.

### 13.5 `mu_thread_system.h` / `mu_task_scheduler.h`

```c
/* TaskFunc: void (*)(void *user, uint64_t thread_id) */
mu_ts_init_desc td = MU_TS_INIT_DESC_DEFAULT;
td.thread_count = 4;                       /* 0 = deterministic dummy mode */
mu_ts_handle ts; mu_ts_init(&ts, &td);
mu_ts_add_task(ts, fn, user);
mu_ts_add_tasks(ts, fn, count, sizeof(user_t), user_array);
mu_ts_wait_idle(ts);

/* TaskFunc: void (*)(void *user, uint32_t worker_index) */
mu_sched_init_desc sd = MU_SCHED_INIT_DESC_DEFAULT;
sd.worker_count = 4;
mu_task_scheduler *sched; mu_sched_init(&sched, &sd);
mu_sched_task_desc desc = { .fn = fn, .user = user,
                            .priority = MU_SCHED_PRIORITY_NORMAL,
                            .completion_counter = &counter };
mu_sched_submit(sched, &desc);
mu_sched_parallel_for(sched, begin, end, grain,
                      MU_SCHED_PRIORITY_NORMAL, fn, user);
mu_sched_counter_wait(&counter);
```

Customization is by macros, not vtables (`MU_TS_MALLOC`, `MU_SCHED_*`). Assert
on contract violations; return `NULL`/`false` for recoverable failures.

---

## 14. Anti-patterns — do not do these

```
✗ A struct with 20+ unrelated fields consumed by different systems.
✗ Pointers as long-lived identity (they break serialization, realloc, compaction).
✗ bool flags checked in a hot loop to decide whether work happens.
✗ enum + switch as runtime polymorphism in a hot loop.
✗ Linked lists / pointer trees in a hot path.
✗ Fixed structs kept as array-of-structs when every loop touches one field.
✗ Building every index up front instead of the hot ones.
✗ Freeing and reallocating per frame instead of arena-rewind / inactive reuse.
✗ Shared mutable accumulator written by all threads.
✗ Small per-thread data left unpadded on a shared cache line.
✗ Branching on random data in an inner loop.
✗ Searching when the answer is directly addressable.
✗ Emulating the real world exactly when a believable approximation is cheaper.
✗ Optimizing a hypothesis without a measurement.
✗ Assuming SIMD / SoA / branchless / non-temporal is always faster.
✗ Splitting code into tiny functions "for style" in the hot path.
✗ Hidden allocation inside helpers, iterators or virtual calls.
✗ Serving every subsystem's lifetime from the global allocator.
```

---

## 15. Review checklist (run before declaring code done)

```
LAYOUT
[ ] Does the hot loop touch only the data it needs?
[ ] SoA vs AoS chosen from the access pattern, not by habit.
[ ] Hot and cold data separated.
[ ] No pointers inside hot rows; ids/handles only.
[ ] One atomic value per cell; no nullable members.

CONTROL FLOW
[ ] No branches on random data in hot loops.
[ ] State is existence (tables/bitsets/sparse sets), not flags.
[ ] No enum-as-flow dispatch in hot paths (or justified if state flips often).
[ ] Unpredictable branches replaced by sorting/bucketing/predication.

MEMORY
[ ] Sequential or strided access; no pointer chasing.
[ ] restrict + ownership explicit enough for the compiler to vectorize.
[ ] Deletion is swap-remove when order is free.
[ ] Buffers/alignment regular enough for vectorization.

ALLOCATION
[ ] One allocator strategy per region; no stray malloc in hot code.
[ ] Stack usage bounded; large scratch in an arena/static buffer.
[ ] Handles carry generations where cross-table references live.

CONCURRENCY
[ ] One writer per datum; local accumulators + single reduce.
[ ] No false sharing; worker outputs kept separate.
[ ] Lifetime is inactive-reuse, not delete-while-referenced.

PROCESS
[ ] Before/after measurements exist (MU_SCOPE_TIMER or profiler).
[ ] Assembly of the hot 5% inspected.
[ ] Compiles warning-clean with -Wall -Wextra -Werror.
[ ] Missing primitive added to mylib, not reinvented locally.
```

---

## 16. Reference examples (illustrative, not compilable as-is)

### 16.1 The full shape of a system

```c
/* ------------------------------------------------------------------ *
 * Rendering: register on add/remove, do not walk a scene graph.       *
 * ------------------------------------------------------------------ */

enum { MAX_RENDERABLES = 1 << 16 };

typedef uint32_t RenderableId;

/* Packed per-renderable state consumed by the renderer, hot loop only. */
static float      positions[MAX_RENDERABLES][3];   /* SoA-friendly x,y,z */
static MeshId     meshes[MAX_RENDERABLES];
static MaterialId materials[MAX_RENDERABLES];
static uint32_t   flags[MAX_RENDERABLES];          /* bit 0 = opaque */
static uint32_t   renderable_count;

static RenderableId renderable_add(MeshId mesh, MaterialId mat, const float pos[3])
{
    RenderableId id = renderable_count++;
    MU_ASSERT(id < MAX_RENDERABLES);

    positions[id][0] = pos[0];
    positions[id][1] = pos[1];
    positions[id][2] = pos[2];
    meshes[id]       = mesh;
    materials[id]    = mat;
    flags[id]        = 1u;                          /* opaque by default */
    return id;
}

static void renderable_remove(RenderableId id)
{
    RenderableId last = --renderable_count;
    positions[id][0] = positions[last][0];
    positions[id][1] = positions[last][1];
    positions[id][2] = positions[last][2];
    meshes[id]       = meshes[last];
    materials[id]    = materials[last];
    flags[id]        = flags[last];
}

/* Stable sort by material id (counting sort, known range). */
static void sort_renderables(RenderableId *order, RenderableId *scratch)
{
    uint32_t bucket[256] = {0};
    for (uint32_t i = 0; i < renderable_count; ++i) ++bucket[materials[order[i]]];
    uint32_t sum = 0;
    for (uint32_t m = 0; m < 256u; ++m) {
        uint32_t n = bucket[m];
        bucket[m]  = sum;
        sum       += n;
    }
    for (uint32_t i = 0; i < renderable_count; ++i)
        scratch[bucket[materials[order[i]]]++] = order[i];
    memcpy(order, scratch, renderable_count * sizeof *order);
}
```

### 16.2 Existence-based damage system with bitset visibility

```c
enum { MAX_ENTITIES = 1 << 16, WORDS = (MAX_ENTITIES + 63) / 64 };

static float    health[MAX_ENTITIES];
static float    regen_delay_left[MAX_ENTITIES];
static uint64_t visible_bits[WORDS];
static uint64_t damaged_bits[WORDS];

static inline bool bit_test(const uint64_t *w, uint32_t i) { return (w[i >> 6] >> (i & 63)) & 1u; }
static inline void bit_set(uint64_t *w, uint32_t i)        { w[i >> 6] |=  UINT64_C(1) << (i & 63); }
static inline void bit_clear(uint64_t *w, uint32_t i)      { w[i >> 6] &= ~(UINT64_C(1) << (i & 63)); }

/* One pass: only iterate the damaged bits; one subtract per element. */
static void regen_update(float dt, float regen_rate)
{
    for (size_t wi = 0; wi < WORDS; ++wi) {
        uint64_t word = damaged_bits[wi];
        while (word) {
            uint32_t bit = (uint32_t)mu_trailing_zeroes_u64(word);
            uint32_t id  = (uint32_t)(wi * 64 + bit);
            word &= word - 1;                            /* clear lowest set bit */

            regen_delay_left[id] -= dt;
            if (regen_delay_left[id] > 0.0f) continue;

            health[id] = MU_MIN(100.0f, health[id] + dt * regen_rate);
            if (health[id] >= 100.0f)
                bit_clear(damaged_bits, id);            /* no longer needs work */
        }
    }
}

/* Visibility is a bitset, so culling output is word operations, not branches. */
static void cull(const Frustum *f, const float (*pos)[3])
{
    for (uint32_t i = 0; i < MAX_ENTITIES; ++i)
        if (in_frustum(f, pos[i])) bit_set(visible_bits, i);
}
```

### 16.3 Skeletal animation: parent indices + DFS order, no recursion

```c
/* Joints are stored in depth-first order, so parent[i] < i. */
typedef struct {
    uint16_t  parent[MAX_JOINTS];
    Matrix    local[MAX_JOINTS];
    Matrix    world[MAX_JOINTS];
    uint32_t  joint_count;
} Skeleton;

/* Recursion became a flat loop because the array encodes the traversal order. */
static void skeleton_local_to_world(Skeleton *s)
{
    s->world[0] = s->local[0];
    for (uint32_t i = 1; i < s->joint_count; ++i)
        s->world[i] = matrix_mul(s->world[s->parent[i]], s->local[i]);
}
```

Why it works: `parent[i] < i` guarantees `world[parent[i]]` is already computed.
The dependency is a **topological order baked into the array layout**. Indices
are 2 bytes instead of an 8-byte pointer, serialize cleanly, and need no pointer
fixups when the skeleton moves.

### 16.4 Handles + generations + arenas assembled

```c
typedef struct { uint32_t index; uint32_t generation; } Handle;

typedef struct {
    Particle *slots;        /* arena-backed pool of MAX_PARTICLES */
    uint32_t *generations;
    uint8_t  *live;
    uint32_t  capacity;
    uint32_t  live_count;
    uint32_t  free_head;
} ParticleStore;

static bool particle_alive(const ParticleStore *s, Handle h)
{
    return h.index < s->capacity
        && s->live[h.index]
        && s->generations[h.index] == h.generation;
}

static Particle *particle_get(ParticleStore *s, Handle h)
{
    if (!particle_alive(s, h)) return NULL;   /* boundary check, not inner-loop check */
    return &s->slots[h.index];
}
```

### 16.5 The transform signature convention

```c
/* read-only input, read/write state, write-only output, explicit count */
void movement_update(Vec3       * MU_RESTRICT positions,   /* rw  */
                     const Vec3 * MU_RESTRICT velocities,  /* r   */
                     size_t                    count,
                     float                     dt);
```

### 16.6 Loop fusion — carefully, and only after measuring

Merge passes over multiple streams when the same set of streams exists over a
range:

```
array1 ...............
array2 .......................................
array3 .........................

naive:
    for i in array1: update1
    for i in array2: update2
    for i in array3: update3

fused (ranges where the same streams exist):
    for i in [0, |a1|):        update1, update2, update3
    for i in [|a1|, |a2|):     update2, update3
```

**Measure it.** Fusion can hurt cache behaviour; the benchmark is the final
word. (This repo has a `demo/loop_fusion.c` benchmark for exactly this.)

---

## 17. Philosophy, condensed

```
Program = data + transformation.

Don't organize data because it belongs to the same noun;
organize it because it is processed together.

The player perceives the world, not the object graph — design
the representation around the relevant mental model.

The goal is not maximum simulation fidelity.
The goal is minimum necessary computation while preserving
the intended experience.

Don't build an object because the thing has a name.
Build the data required to represent the work the program
actually does.

Then measure.
```

> "The compiler can eliminate instructions. It cannot rescue a
> fundamentally poor data layout."

---

## 18. Where to go next

- `dodnotes.txt` — the full narrative notes with diagrams and formulas.
- `.agents/skills/dod-performance/SKILL.md` — the operational skill for agents
  (the rules in this file, as a procedure).
- `.agents/skills/game-engine-dod/SKILL.md` — the end-to-end game/engine skill
  built on `dod-performance` (frame loop, ECS, physics, animation, culling, LOD,
  renderer batching, streaming, profiling).
- `mylib/docs/` — per-module `mu` documentation.
- `dodbook/` — Fabian's *Data-Oriented Design* (chapter sources).

