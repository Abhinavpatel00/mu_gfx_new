# Subsystem Recipes

Companion to `.agents/skills/game-engine-dod/SKILL.md`. Concrete data layouts
and transforms for each engine subsystem. Underlying rules in `dod-performance`.

---

## 1. World / ECS

**Entity = id + generation. Components = dense tables. Systems = transforms.**

```c
#include "mu.h"
#include "mu_ecs.h"            /* or build the same shape from mu_bulk_storage */

typedef struct {
    mu_ecs_entity_manager entities;
    mu_bulk_storage        instances;    /* optional: capacity slots + gens */
    /* authoritative SoA component tables, keyed by entity index */
    Vec3                   positions[MAX_ENTITIES];
    Vec3                   velocities[MAX_ENTITIES];
    float                  health[MAX_ENTITIES];
    /* membership sets */
    mu_sparse_set          alive;
    mu_sparse_set          moving;
    mu_sparse_set          hurt;
    /* deferred mutation */
    EntityId               spawn_queue[MAX_ENTITIES];
    uint32_t               spawn_count;
    EntityId               destroy_queue[MAX_ENTITIES];
    uint32_t               destroy_count;
} World;
```

Deferred mutation is mandatory:

```c
EntityId world_spawn(World *w)               /* cheap: queue only */
{
    EntityId e = mu_ecs_entity_create(&w->entities);
    w->spawn_queue[w->spawn_count++] = e;
    return e;
}

void sys_spawn(World *w)                     /* drain at a fixed frame point */
{
    for (uint32_t i = 0; i < w->spawn_count; ++i) {
        EntityId e = w->spawn_queue[i];
        mu_sparse_set_insert(&w->alive, mu_ecs_entity_index(e));
        /* initialise component rows for e here */
    }
    w->spawn_count = 0;
}

void sys_destroy(World *w)
{
    for (uint32_t i = 0; i < w->destroy_count; ++i) {
        EntityId e = w->destroy_queue[i];
        uint32_t idx = mu_ecs_entity_index(e);
        mu_sparse_set_remove(&w->alive, idx);
        mu_sparse_set_remove(&w->moving, idx);
        mu_sparse_set_remove(&w->hurt, idx);
        mu_ecs_entity_destroy(&w->entities, e);
        /* leave component rows inert; reuse on next spawn */
    }
    w->destroy_count = 0;
}
```

Iterate **membership sets**, not all entities:

```c
for (uint32_t i = 0; i < w->moving.count; ++i) {
    uint32_t e = mu_sparse_set_at(&w->moving, i);
    positions[e] += velocities[e] * dt;
}
```

Optional: for many distinct component *combinations* (archetypes), bucket
entities into per-combination tables so each system gets a perfectly
homogeneous list. Start with SoA tables + a membership set per capability; add
archetypes only when measurement shows the join is the bottleneck.

---

## 2. Movement and physics

### 2.1 Integrate over SoA

```c
static void movement_worker(uint32_t begin, uint32_t end,
                            uint32_t worker_index, void *user)
{
    MovementCtx *c = user;
    for (uint32_t i = begin; i < end; ++i) {
        uint32_t e = mu_sparse_set_at(&c->w->moving, i);
        Vec3 * MU_RESTRICT p = &c->w->positions[e];
        const Vec3 * MU_RESTRICT v = &c->w->velocities[e];
        p->x += v->x * c->dt;
        p->y += v->y * c->dt;
        p->z += v->z * c->dt;
    }
}

mu_sched_parallel_for(sched, 0, w->moving.count, 1024,
                      MU_SCHED_PRIORITY_NORMAL, movement_worker, &ctx);
```

### 2.2 Spatial broad phase

Partition into a uniform grid (or hash) once per step. Collision then tests the
same and neighbouring cells, not every pair — this is what removes the O(n^2).

```c
enum { GRID_W = 128, GRID_H = 128, GRID_D = 32 };
enum { CELL_CAP = 32 };

typedef struct {
    uint32_t entity[GRID_W][GRID_H][GRID_D][CELL_CAP];  /* flatten in practice */
    uint16_t count[GRID_W][GRID_H][GRID_D];
} SpatialGrid;

static inline int cell_of(float world, float cell_size)
{
    return (int)((world + 1024.0f) / cell_size);          /* clamp to range */
}

void broadphase_clear(SpatialGrid *g);
void broadphase_insert(SpatialGrid *g, uint32_t e, Vec3 pos, float cell_size);
```

Broad phase → candidate pairs → narrow phase:

```c
/* candidates only; narrow phase is the expensive precise test */
for each cell
    for each entity in cell
        for each entity in cell (j > i)
            if (aabb_overlap(...)) narrowphase_pair(a, b);
        for neighbouring cells ...
```

Tune `cell_size` to the typical object size — too small means many neighbour
cells, too large means many false pairs.

### 2.3 Flow fields (pathing without per-unit search)

A flow field is a grid where each cell stores a direction/step cost toward one
goal, computed once per goal. Build it with a BFS/Dijkstra map from the goal,
then every unit just reads the field.

```c
#include "mu_roguelike.h"

static int  is_blocked_cb(int x, int y, void *user);
static inline int cell_index(int x, int y) { return mu_rog_grid_index(x, y, MAP_W); }

typedef struct {
    int16_t dist[MAP_W * MAP_H];       /* cost to goal from each cell */
    uint8_t dir[MAP_W * MAP_H];        /* packed direction to step next */
    mu_rog_ivec2 goals[MAX_GOALS];
    int goal_count;
    int queue[MAP_W * MAP_H];          /* scratch from the arena */
} FlowField;

void flowfield_rebuild(FlowField *f, void *blocked_user)
{
    mu_rog_dijkstra_map8(MAP_W, MAP_H,
                         f->goals, f->goal_count,
                         is_blocked_cb, blocked_user,
                         f->dist, f->queue);

    /* derive a direction per cell: point to the lowest neighbour */
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x)
            f->dir[cell_index(x, y)] = best_neighbor_dir(f, x, y);
}
```

Cost model: rebuild the field only when the goal or traversability changes
(cache it), and share it among all units. Recompute per-goal, not per-unit.
`mu_rog_bfs_distance4` gives unweighted distance when all steps cost the same;
`mu_rog_astar_find_path` remains useful for single agents and irregular maps.

---

## 3. Gameplay, AI, FSMs, scheduled events

### 3.1 State as table membership

```c
EntityId idle[MAX_ENTITIES];    uint32_t idle_count;
EntityId patrol[MAX_ENTITIES];  uint32_t patrol_count;
EntityId attack[MAX_ENTITIES];  uint32_t attack_count;

/* transition = remove from one table, insert into another */
static void fsm_transition(EntityId e, State from, State to);
```

### 3.2 Evaluate then commit

```c
typedef struct { EntityId entity; uint8_t next_state; } Transition;

void ai_evaluate(AiState *a, const InputBits *in, Transition *out, uint32_t *n)
{
    for (uint32_t i = 0; i < a->patrol_count; ++i) {
        EntityId e = a->patrol[i];
        if (seen_player(e)) out[(*n)++] = (Transition){ e, STATE_ATTACK };
    }
}

void ai_commit(AiState *a, const Transition *t, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) fsm_transition(t[i].entity, /*old*/-1, t[i].next_state);
}
```

All conditions see one consistent snapshot; commit once.

### 3.3 Update rate by relevance (AI LOD)

```c
/* only run expensive AI for entities in the "near" LOD bucket, */
/* at its bucket's frequency. Far entities use a cheap approximation. */
static void ai_update_lod(AiState *a, LODBucket *b, float dt)
{
    if (b->accumulator >= b->period) {
        b->accumulator -= b->period;
        ai_evaluate_lod(a, b);
    }
}
```

### 3.4 Scheduled events (timers, cooldowns, spawns)

Use a sorted event queue rather than polling every entity every frame:

```c
#include "mu_roguelike.h"

mu_rog_scheduler sched;          /* push by tick; peek/pop the next due */
mu_rog_scheduler_init(&sched);
mu_rog_scheduler_push(&sched, actor_id, event_tick);

while (mu_rog_scheduler_peek(&sched, &ev) == 1 && ev.tick <= now) {
    mu_rog_scheduler_pop(&sched, &ev);
    handle_event(&ev);
}
```

This is the "scheduled work replaces polling" pattern; recharge/cooldown tables
sorted by `ready_time` are the same idea without the general scheduler.

---

## 4. Animation

### 4.1 Packed curves

Search keys, index payloads once. Do not interleave payload with search keys.

```c
typedef struct { float x, y, z; } KeyValue;

typedef struct {
    float    *times;      /* contiguous; the only thing binary-searched */
    KeyValue *values;     /* indexed once the search lands             */
    uint32_t  count;
} Curve;

/* optional: keys you never binary-search (you always play in order) */
void anim_play_forward(const Curve *c, float t, float *out_x, ...);
```

### 4.2 Local → global in topological order

Store parent indices in depth-first order so `parent[i] < i`; no recursion, no
sort, no per-frame topological pass.

```c
typedef struct {
    uint16_t parent[MAX_JOINTS];
    float    local[MAX_JOINTS][16];
    float    global[MAX_JOINTS][16];
    uint32_t joint_count;
} SkeletonPose;

void skeleton_local_to_global(SkeletonPose *s)
{
    s->global[0] = s->local[0];                 /* memcpy 16 floats */
    for (uint32_t i = 1; i < s->joint_count; ++i)
        mat4_mul(s->global[i], s->global[s->parent[i]], s->local[i]);
}
```

### 4.3 Blending and skinning

- Sample all clips into local poses, blend in local space, then one
  local→global pass.
- Skinning writes a compact matrix palette the GPU consumes (one representation
  for the GPU, another for simulation).
- LOD animation: far characters get a lower key rate / fewer bones / baked
  poses; distant crowds animate collectively or not at all.
- `mu_ecs_transform_*` can carry static transform hierarchy; animation usually
  owns its own joint arrays for control.

---

## 5. Culling

Cull before building the render list. Output is a bitset, not a filtered array,
so downstream code can test membership cheaply and batch word operations.

```c
#include "mu/mu_bitset.h"

void sys_cull(const TransformStore *t, const Frustum *f, mu_bitset *visible)
{
    /* hierarchical: test groups/chunks first; only recurse into survivors */
    for (uint32_t c = 0; c < t->chunk_count; ++c) {
        if (!aabb_in_frustum(f, t->chunk_bounds[c])) continue;
        for (uint32_t i = t->chunk_begin[c]; i < t->chunk_end[c]; ++i) {
            if (aabb_in_frustum(f, &t->world_bounds[i]))
                mu_bitset_set(visible, i);
        }
    }
}
```

Also cull by distance/LOD, occlusion, and gameplay relevance. Keep bounds in a
contiguous table indexed the same way as transforms so culling is a stream, not
a pointer chase. Iterate the result with CTZ:

```c
size_t i = 0;
while (mu_bitset_next_set_bit(visible, &i)) {
    add_to_render_list(i);
    ++i;
}
```

---

## 6. LOD

### 6.1 Relevance metric

```c
static inline float relevance(const Camera *cam, Vec3 pos, float radius,
                              float importance, float attention)
{
    float d  = distance(cam->pos, pos);
    float ss = screen_space_radius(cam, d, radius);   /* projected size */
    return ss * importance * attention / (1.0f + d);
}
```

### 6.2 LOD buckets as membership sets

```c
enum { LOD_COUNT = 4 };
mu_sparse_set lod[LOD_COUNT];      /* entity belongs to exactly one bucket */
float         lod_period[LOD_COUNT];  /* update period per bucket (Hz) */
```

Rebuild buckets on a schedule (e.g. every N frames), not every frame. Systems
iterate the bucket they care about:

```c
for (uint32_t b = 0; b < LOD_COUNT; ++b) {
    if (lod_accum[b] < lod_period[b]) continue;
    lod_accum[b] -= lod_period[b];
    simulate_bucket(b);              /* detail and rate scale with b */
}
```

### 6.3 Collective LOD and mementos

- Distant identical units become one record + count, or a group
  (`Squadron { count, attack_probability }`), simulated statistically.
- A **memento** preserves the history that must survive detail reduction
  (lossy is fine); a **JIT memento** regenerates from `hash(entity_id)` instead
  of storing anything.
- Use `mu_noise_math.h` (`fbm`, `ridged`, `domain_warp`, `curl`) with a
  per-object seed for deterministic procedural detail.
- Temporal LOD: keep full state briefly, compressed state longer, seed-only for
  things unseen for a long time.

---

## 7. Renderer: batching, instancing, GPU layout

### 7.1 The render list

Flat packets, produced from the cull bitset. Never a per-frame scene graph walk.

```c
typedef struct {
    MeshId   mesh;         /* or meshlet */
    MatId    material;
    uint32_t transform;    /* index into the (possibly GPU-packed) transform array */
    uint32_t flags;        /* OPAQUE | TRANSPARENT | SHADOW | SKINNED */
    float    depth;        /* sort key, filled after cull */
} RenderPacket;

typedef struct {
    RenderPacket *packets;
    uint32_t      count;
    uint32_t      capacity;
} RenderList;
```

### 7.2 Sorting and batching

```
opaque       -> sort front-to-back, group by material/pipeline/mesh
transparent  -> sort back-to-front
shadow casters -> separate pass/list
```

- Sort/batch **only** the visible list, and only after culling.
- Use **counting/radix sort** when material/pipeline keys are small discrete IDs
  (near-linear, cache-friendly) — see `dod-performance`.
- Batch = consecutive packets sharing (material, mesh). One draw per batch.
- Overdraw is real: front-to-back depth rejection is often worth more than
  perfect material ordering. Measure with `texture + mesh + state_change +
  overdraw` costs.

### 7.3 Instancing and indirect draws

When many packets share mesh + material, emit one instanced draw and pack
per-instance data as SoA:

```c
typedef struct {
    float    *model;     /* 16 floats per instance, contiguous        */
    uint32_t *tint;      /* packed RGBA                               */
    uint32_t  count;
} InstanceBatch;
```

For a fully GPU-driven renderer, write per-batch draw arguments and let the GPU
consume an indirect buffer; the CPU only builds the instance streams. Keep this
as a separate representation — do not interleave it with the simulation arrays.

### 7.4 GPU vs simulation representations

```
SIM (SoA, full precision, easy to iterate)     GPU (packed, aligned, streamed)
positions_x/y/z[]  ---------------------------->  vertex buffer
normals            ---(quantize via mu_bitpacking)-> packed normal
bone matrices      ---------------------------->  matrix palette
```

Convert in the asset pipeline when the data is static, and in a per-frame pack
step when it is dynamic. Double-buffer (or ring-buffer) the GPU-facing arrays so
writing this frame never tears what the GPU reads last frame.

### 7.5 Draw-call discipline

- One material per batch; sort by material across the whole visible set.
- Upload textures/meshes once; never per draw.
- Avoid redundant state changes; group by pipeline.
- Separately sort opaque, transparent, and shadow lists.
- Keep the number of distinct pipelines small so batches are large.

---

## 8. Assets and streaming

- **Offline toolchain** converts source assets (source/render/physics/raycast
  variants) into runtime formats. Cache derived data.
- Only load what the shipping build needs; the editor may need all
  representations, the runtime may need render + physics only.
- **Manifest + handles**: game code refers to assets by id/handle
  (`mu_id_pool_*` for ranged ids), never by pointer into an asset object.
- Stream asynchronously; upload completed loads in step [11] of the frame.
- Pack assets to reduce memory bandwidth: `mu_bitpacking.h` quantization,
  shared string storage (`mu_string_arena`), SoA vertex streams.

---

## 9. Profiling, debug and budgets

- Time every system and the whole frame: `MU_SCOPE_TIMER("sys_x")`, and keep a
  rolling budget (ms per system, frame total).
- Count facts: entities, alive, moving, hurt, batches, draws, triangles,
  instance counts, culled fraction. `dodnotes` Part 1.5 — design from real
  distributions.
- Debug invariants with `MU_ASSERT` (table bounds, `used <= capacity`), disabled
  in release.
- Deterministic sim + input recording gives replays; a bug can be replayed
  frame by frame.
- Keep a debug overlay drawing from the same tables (stats as a system, not
  special-cased code).
- Use `mu_mmgr.h` in debug builds to catch leaks and corruption.

---

## 10. Frame budget guardrails (rules of thumb, always measure)

```
total frame budget = 1 / target_fps
assign per system, e.g. 16.6 ms (60 fps):
    input           < 0.5 ms
    simulate (N)    ~ 4-8 ms
    animations      ~ 1-3 ms
    culling         < 1 ms
    render list     < 1 ms
    submit          ~ 2-5 ms (CPU)  + GPU frame time
    present/other   remaining
```

When a system exceeds its budget, first question the **data layout**, then the
algorithm, then the micro-optimizations — in that order:
`dod-performance` step 7.
