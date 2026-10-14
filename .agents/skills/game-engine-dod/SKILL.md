---
name: game-engine-dod
description: >
  Build or extend a complete high-performance game/engine end to end using
  data-oriented design: boot and init, fixed-timestep frame loop, input as
  events, ECS/component tables, deferred spawn/destroy, spatial broad phase and
  flow-field pathing, animation, frustum culling, LOD and collective LOD,
  renderer batching and instancing, GPU layout, asset streaming, arenas, and
  frame profiling. Use this whenever the task is to write or grow a game,
  game engine, renderer, simulation, or real-time loop in C — even if the user
  just says "make a game", "add a renderer", "add a frame loop", "add LOD",
  "batch my draw calls", or "design the engine architecture".
---

# Skill: Data-Oriented Game Engine, End to End

This skill composes a whole real-time game/engine. It is **built on**
`dod-performance`: load and apply that skill (and `AGENT.md`) first. Everything
in `dod-performance` — existence-based processing, branch discipline, SoA,
arenas, ownership-based concurrency, measure-first — is assumed here. This
skill only adds the **game-level composition**: how the pieces fit together,
in what order, and where the classic performance traps are.

Read `AGENT.md` and `dodnotes.txt` at the project root before starting.

---

## 0. The engine in one picture

```
        BOOT / INIT                     (once)
   platform, window, GPU, assets, capacities
                    |
   =================== FRAME ===================
                    |
   [1] BEGIN FRAME            reset frame arena, swap buffers
   [2] INPUT                  platform events -> input tables (no polling)
   [3] TIME                   accumulator -> N fixed steps + blend factor
   [4] SIMULATE  (x N)        systems over component tables, in order
         movement/physics -> collision -> gameplay/AI/FSM
         -> drain spawn queue -> drain destroy queue
   [5] TRANSFORMS             SoA hierarchy, topological order, propagate
   [6] ANIMATION              sample packed curves -> blend -> skin matrices
   [7] CULL + LOD             frustum bitset, relevance buckets, visibility
   [8] BUILD RENDER LIST      visible registrations -> packets -> sort/batch
   [9] SUBMIT                 per-batch draws / instancing / indirect
   [10] PRESENT
   [11] END FRAME             arena rewind, stats, telemetry
                    |
   =================== repeat ===================
```

**The frame is a sequence of transforms over tables.** Every step reads a table
and writes a table; nobody walks an object graph. The question for each step is
the DOD question: *what data, what transformation, what layout?*

---

## 1. Procedure

### Step 1 — Fix the frame contract

Decide and write down:

- **Timestep model.** Fixed timestep for simulation (`dt` constant) with an
  accumulator; clamp max steps to avoid the spiral of death. Render interpolates
  between previous and current state. Variable `dt` only for non-deterministic
  presentation effects.
- **Simulation rate vs render rate.** They are independent. Physics/AI may run
  at 30/60/10 Hz; rendering at display rate.
- **Determinism target.** If you want replays/netcode, the sim must be a pure
  function of `(state, input, dt)`; no wall-clock or unseeded randomness.
- **Threading model.** Which systems are parallel; how they synchronize.

```c
/* fixed step: deterministic, stable physics, replayable */
#define SIM_HZ      60
#define SIM_DT      (1.0f / SIM_HZ)
#define MAX_STEPS   5              /* clamp to avoid spiral of death */

accumulator += frame_dt;
int steps = 0;
while (accumulator >= SIM_DT && steps < MAX_STEPS) {
    simulate(SIM_DT);
    accumulator -= SIM_DT;
    ++steps;
}
float alpha = accumulator / SIM_DT;    /* for render interpolation */
```

This is the "when does work happen" decision; get it right before anything else.

### Step 2 — Define the world as tables

Lay out the authoritative simulation state as SoA tables keyed by entity id:

```c
/* one table per system's data, not one struct per creature */
static Vec3   positions[MAX_ENTITIES];
static Vec3   velocities[MAX_ENTITIES];
static float  health[MAX_ENTITIES];
static EntityId alive_ids[MAX_ENTITIES];  size_t alive_count;
static mu_sparse_set hurt;                /* membership = needs work */
```

Derive, don't store: position of a grid tile, health-as-state when it can be a
membership, flags that can be bitsets. Optional data is a side table or a sparse
set, never a nullable field. See `dod-performance` for the full rule set.

### Step 3 — Systems, in a fixed order

Give each system a signature and a slot in the frame. Systems are free
functions over tables:

```c
void sys_input(InputTable *in, const FrameEvents *ev, float dt);
void sys_movement(MovementTables *t, float dt);           /* parallel */
void sys_collision(CollisionTables *t);                   /* broad + narrow */
void sys_ai(AiTables *t, const SensorTables *s, float dt, uint32_t lod);
void sys_spawn(World *w);        /* drain queued spawns  */
void sys_destroy(World *w);      /* drain queued destroys */
void sys_transforms(TransformStore *t);
void sys_animate(AnimTables *t, float dt);
void sys_cull(const TransformStore *t, Frustum f, CullResult *out);
void sys_build_render_list(const CullResult *c, RenderList *out);
```

**Spawn/destroy are deferred.** Never create or destroy mid-iteration; append to
a queue and drain it at a defined point in the frame. This keeps iteration
stable and makes table membership a consistent snapshot for the whole step.

### Step 4 — Schedule the systems

Independent systems run in parallel over disjoint data. Dependent systems gate
on counters/groups. Batch at stage boundaries.

```c
mu_sched_parallel_for(sched, 0, alive_count, 1024,
                      MU_SCHED_PRIORITY_NORMAL, movement_worker, &mt);
sys_collision(&ct);                    /* needs movement done */
mu_sched_parallel_for(sched, 0, ct.candidate_count, 512,
                      MU_SCHED_PRIORITY_NORMAL, narrowphase_worker, &ct);
```

Rules: one writer per table, local accumulators, thread-local output buffers as
in `dod-performance`. Do not parallelize a system whose data structure mutates
globally.

### Step 5 — Memory by phase

- **Frame arena** for everything that lives one frame (render list, cull
  results, temporaries). Reset with a pointer assignment at step 11.
- **Level/session arena** for content that persists across frames.
- **Pool/handles** for entities and components that come and go.
- **Static capacities** for tables; assert on overflow.
- **Async asset pool** for streaming; nothing blocking inside the frame.

No `malloc` in the frame. Ever.

### Step 6 — Render through a registered list, not a scene graph

Register renderables when entities gain a mesh and unregister when they lose it.
The frame gathers the **visible** registrations into a flat packet array, sorts
it for the GPU, and submits. Never walk a tree per frame.

```c
typedef struct {
    MeshId      mesh;
    MaterialId  material;      /* contains pipeline/texture/shader */
    uint32_t    transform;     /* index into transform table */
    uint32_t    flags;         /* opaque/transparent/cast-shadow/... */
} RenderPacket;
```

### Step 7 — LOD is relevance, not distance

Compute a relevance metric per entity (screen-space size, distance, attention,
interaction history), then bucket entities into LOD tables. LOD controls update
rate, detail, and existence — not just meshes.

```
relevance = f(screen_size, distance, visibility, attention,
              time_since_seen, interaction, progression, importance)
```

### Step 8 — Profile the frame, per system

Every system gets a timer; the frame gets a timer. Keep a rolling frame budget.
See `dod-performance` step 7 and `references/subsystem-recipes.md`.

---

## 2. The frame-loop hard rules

1. **Fixed timestep for simulation**, accumulator, clamped step count.
2. **Deterministic sim**: sim = f(state, input, dt); no wall clock, no global RNG.
3. **No allocation in the frame**; frame arena reset at end.
4. **Deferred spawn/destroy**; drain at fixed points.
5. **No system walks an object graph**; systems iterate tables.
6. **Render from a registered list**, never a per-frame scene traversal.
7. **Double-buffer state the GPU reads** (or write to a buffer the GPU is not
   reading) — don't tear the frame.
8. **One writer per table**; independent systems parallel, dependent ones gated.
9. **Cull before you build, batch after you cull**: only visible objects enter
   the render list; only the render list gets sorted.
10. **Representation follows relevance** (LOD/collective LOD/mementos).
11. **Input is events and state tables**, not per-system polling of the platform.
12. **Every system is timed**, every claim measured.

---

## 3. Subsystem map (where to look)

| Subsystem | Key ideas | Recipes |
|---|---|---|
| Boot/init | static capacities, arenas, device bring-up, asset manifest | `references/frame-pipeline.md` |
| Input | platform events → tables/queues; actions derived from raw state | `references/frame-pipeline.md` |
| Time | fixed step, accumulator, interpolation, pause/scale | `references/frame-pipeline.md` |
| ECS | entity = id+generation, component pools/SoA, deferred mutate | `references/subsystem-recipes.md` |
| Movement/physics | integrate over SoA, spatial grid broad phase, narrow phase candidates | `references/subsystem-recipes.md` |
| Gameplay/AI/FSM | state = table membership, transition buffers, tick scheduler | `references/subsystem-recipes.md` |
| Pathing | flow fields from BFS/Dijkstra maps; shared by all units | `references/subsystem-recipes.md` |
| Animation | packed key arrays, local→global in topological order, skin matrices | `references/subsystem-recipes.md` |
| Culling | frustum test per chunk, visibility bitset, hierarchical bounds | `references/subsystem-recipes.md` |
| LOD | relevance metric, LOD buckets, update rates, collective, mementos | `references/subsystem-recipes.md` |
| Renderer | packets, sort/batch, instancing, indirect, GPU layout | `references/subsystem-recipes.md` |
| Assets/toolchain | offline conversion, GPU vs sim representations, streaming | `references/subsystem-recipes.md` |
| Profiling/debug | per-system timers, counters, replay, budget guard | `references/subsystem-recipes.md` |

---

## 4. Use `mylib` (`mu`) — the engine's standard library

Never hand-roll a container or allocator; extend `mylib` instead. Game-relevant
primitives (verified against the headers):

| Engine need | Use |
|---|---|
| Entities (id + generation) | `mu_ecs_*` or `mu_bulk_storage_*` + `mu_sparse_set_*` |
| Component tables | SoA arrays + `mu_sparse_set` / `mu_sparse_set_at` dense iteration |
| Transforms (SoA + hierarchy) | `mu_ecs_transform_*` (index links, propagate) |
| Dense active sets | `mu_sparse_set_*` |
| Visibility / flags | `mu_bitset_*` (`next_set_bit`, `for_each`, set algebra) |
| Ranged/resource ids | `mu_id_pool_*` |
| Mesh↔material, tag buckets | `mu_multi_index_*` |
| Variable children (nodes, edges, bones) | `mu_chunked_u32_array_*` |
| Packed strings (names, paths) | `mu_string_arena_*` |
| Frame/level/ring memory | `mu_arena`, `mu_linear_allocator`, `mu_ring_allocator` |
| Fixed-size object pools | `mu_pool_*` |
| Variable allocations, streaming | `mu_buddy_*`, `offset_allocator.h` |
| Leak/corruption tracking | `mu_mmgr.h` |
| Deterministic RNG (replays, gen) | `mu_pcg32_*` |
| World noise (terrain, warp, fBm) | `mu_noise_math.h` |
| Grids, FOV, LOS, A*, BFS/Dijkstra maps | `mu_roguelike.h` |
| Tick/scheduled events | `mu_rog_scheduler_*` |
| Jobs/parallel systems | `mu_task_scheduler.h` (`mu_sched_*`) |
| Worker pool + atomics | `mu_thread_system.h` (`mu_ts_*`) |
| Quantization for GPU attributes | `mu_bitpacking.h` |
| Frame/system timing | `MU_SCOPE_TIMER`, `mu_time_now()` |

Flow fields: build them as **Dijkstra/BFS maps** from the goal
(`mu_rog_dijkstra_map8`, `mu_rog_bfs_distance4`) over a grid of `mu_rog_grid_u8`,
then store one direction/step cost per cell. All units read the same field —
no per-unit pathfinding.

If a primitive is missing, add a small reusable one under `mylib/mu/` with the
`MU_API`/`MU_STATIC` linkage model, `mu_malloc` overrides and `MU_ASSERT` on
contract violations.

---

## 5. Getting it right (the traps)

```
✗ Variable dt for physics, then wondering why the sim explodes.
✗ Allocating per frame, or per entity spawn, from the global heap.
✗ Walking a scene graph / object tree every frame.
✗ Creating/destroying entities while iterating the table they live in.
✗ Rendering the whole world then culling in the fragment shader.
✗ Sorting every draw call individually instead of batching first.
✗ One giant vertex buffer with interleaved sim+GPU data.
✗ Per-unit pathfinding instead of a shared flow field.
✗ Full-detail simulation of things the player cannot perceive.
✗ Shared mutable state touched by every system.
✗ Parallelizing a system that mutates global tables.
✗ Persisting derived state (positions for grid tiles, flags for state).
✗ No frame/system timers, so every perf claim is a guess.
```

---

## 6. Review checklist before calling the engine "done"

```
TIME / LOOP
[ ] fixed timestep for simulation, accumulator, clamped max steps
[ ] render interpolation between prev/current state
[ ] sim is deterministic (replayable); no wall clock / global RNG in sim

FRAME
[ ] no allocation inside the frame; frame arena reset at end
[ ] spawn/destroy drained at fixed points
[ ] every system timed; frame budget tracked
[ ] systems iterate tables, never object graphs

ENTITY / COMPONENTS
[ ] entity = id + generation (mu_bulk_storage / mu_ecs)
[ ] components are dense tables / sparse sets, not nullable fields
[ ] active sets are membership, not flags scanned every frame

WORLD / AI
[ ] spatial broad phase before narrow phase
[ ] shared flow fields, not per-unit pathfinding
[ ] AI/physics update frequency scaled by relevance

RENDER
[ ] renderables registered on add/remove, not walked per frame
[ ] cull before building; sort/batch only the visible list
[ ] opaque front-to-back, transparent back-to-front
[ ] instancing / indirect where mesh+material repeat
[ ] GPU layout separate from simulation layout

LOD
[ ] relevance metric drives representation
[ ] collective groups for distant identical entities
[ ] mementos for what must survive, JIT gen for what need not

MEMORY / CONCURRENCY
[ ] arenas per phase; pools for churn; handles for references
[ ] one writer per table; independent systems parallel, dependent gated
[ ] no false sharing on per-thread state

PROCESS
[ ] measured frame time on the real workload, before and after changes
[ ] compiles warning-clean; mylib used, not reinvented
```

---

## 7. Reference material

- `references/frame-pipeline.md` — boot, input, time, the exact per-step frame
  with C, and the job graph.
- `references/subsystem-recipes.md` — ECS, collision/flow fields, animation,
  culling, LOD, renderer batching/instancing, GPU layout, streaming, profiling.
- Skill `dod-performance` — the underlying rules (load this first).
- Project root `AGENT.md` and `dodnotes.txt` — the full doctrine.
- `mylib/docs/` — per-module details (`ecs.md`, `concurrency.md`, `roguelike.md`,
  `math.md`, `containers.md`, `sets-and-ids.md`, `allocators.md`).
