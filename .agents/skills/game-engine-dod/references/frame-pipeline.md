# Frame Pipeline

Companion to `.agents/skills/game-engine-dod/SKILL.md`. The concrete boot, time,
input and frame structure with C. Read `dod-performance` first.

---

## 1. Boot / init (once, before the loop)

Do all allocation, capacity planning and validation here. The frame must be
allocation-free and branch-light.

```c
typedef struct {
    /* platform */
    Platform        platform;
    InputState      input;

    /* memory: one arena per phase, pools for churn */
    mu_arena        frame_arena;        /* reset every frame          */
    mu_arena        level_arena;        /* reset on level change       */
    void           *arena_mem;
    size_t          arena_mem_size;

    /* device */
    GpuDevice       gpu;
    AssetPool       assets;             /* meshes/materials/textures   */

    /* world */
    World           world;              /* entity + component tables  */
    TransformStore  transforms;
    AnimStore       anim;
    Physics         physics;
    AiState         ai;
    mu_rog_scheduler sched_events;      /* scheduled gameplay events  */

    /* jobs */
    mu_task_scheduler *sched;

    /* rendering */
    RenList         render_list;
    CullResult      cull;

    /* frame */
    float           accumulator;
    double          now;
} Engine;

bool engine_init(Engine *e, const EngineConfig *cfg)
{
    /* 1. platform/window first: the rest may depend on it */
    if (!platform_init(&e->platform, &cfg->platform)) return false;

    /* 2. memory: everything downstream draws from these */
    e->arena_mem_size = MU_MB(cfg->arena_mb);
    e->arena_mem      = platform_reserve(e->arena_mem_size);
    if (!e->arena_mem) return false;
    /* mu_arena has no init function: set the three fields directly */
    e->frame_arena = (mu_arena){ e->arena_mem,
                                 cfg->frame_arena_bytes, 0 };
    /* level arena uses the remainder of arena_mem */

    /* 3. device + static-sized rendering resources */
    if (!gpu_init(&e->gpu, &e->platform)) return false;

    /* 4. deterministic RNG for world generation, replays and JIT mementos */
    mu_pcg32 rng; mu_pcg32_init(&rng, cfg->world_seed, 0);

    /* 5. world: fixed capacities, assert on overflow */
    world_init(&e->world, cfg->max_entities);
    transform_init(&e->transforms, cfg->max_transforms);
    physics_init(&e->physics);
    ai_init(&e->ai);
    mu_rog_scheduler_init(&e->sched_events);

    /* 6. jobs: thread_count == 0 is deterministic dummy mode for debugging */
    mu_sched_init_desc sd = MU_SCHED_INIT_DESC_DEFAULT;
    sd.worker_count = cfg->worker_count;
    if (!mu_sched_init(&e->sched, &sd)) return false;

    /* 7. camera, input, render list capacity */
    input_init(&e->input);
    cull_init(&e->cull, cfg->max_entities);
    render_list_init(&e->render_list, cfg->max_draws);

    /* 8. load manifest (or stream asynchronously; never block the frame) */
    return assets_begin_load(&e->assets, cfg->asset_manifest);
}
```

Project layout for a C game:

```
  main.c            platform entry, argv, run loop
  engine.c/.h       Engine struct, init/shutdown, frame()
  world/            entity + components + systems
  render/           render list, batch, submit, gpu layouts
  sim/              movement, collision, gameplay, ai
  anim/             curves, blending, skinning
  assets/           manifest, loaders, offline conversion
  math/             vec/mat/transform (or crop from mylib/cglm)
  plat/             window, input, files, timers
```

---

## 2. Input as data

Platform events are converted to tables once per frame. Systems read the tables;
they never poll the platform.

```c
typedef struct {
    /* edge events this frame (append; cleared at end of frame) */
    InputEvent events[MAX_INPUT_EVENTS];
    uint32_t   event_count;

    /* held state as a bitset so systems test 64 keys per instruction */
    uint64_t   down[KEY_WORDS];
    float      mouse_x, mouse_y, mouse_dx, mouse_dy;

    /* derived: gameplay actions, resolved from raw state + bindings */
    uint32_t   action_bits;      /* bit i = action i active this frame */
} InputState;

void input_begin(InputState *in) { in->event_count = 0; }

void input_feed(InputState *in, const PlatformEvent *ev)
{
    switch (ev->kind) {
    case PLAT_KEY_DOWN:
        if (ev->key < KEY_WORDS * 64u)
            in->down[ev->key >> 6] |= UINT64_C(1) << (ev->key & 63);
        in->events[in->event_count++] = (InputEvent){ .key = ev->key, .down = true };
        break;
    case PLAT_KEY_UP:
        if (ev->key < KEY_WORDS * 64u)
            in->down[ev->key >> 6] &= ~(UINT64_C(1) << (ev->key & 63));
        in->events[in->event_count++] = (InputEvent){ .key = ev->key, .down = false };
        break;
    case PLAT_MOUSE_MOVE:
        in->mouse_x = ev->x; in->mouse_y = ev->y;
        in->mouse_dx += ev->dx; in->mouse_dy += ev->dy;
        break;
    default: break;
    }
}

static inline bool input_down(const InputState *in, uint32_t key)
{
    return (in->down[key >> 6] >> (key & 63)) & 1u;
}
```

Key points:
- **Events are appended**, so a system can react to a press edge without diffing
  previous state.
- **Held state is bitset**, so "is any of these keys down" is one AND.
- **Actions are derived once**, so rebinding is a table change, not a code change.
- Input is consumed by systems through the `InputState`, not by a global.

---

## 3. Time model

```c
#define SIM_HZ      60
#define SIM_DT      (1.0f / (float)SIM_HZ)
#define MAX_STEPS   5
#define MAX_FRAME_DT 0.25f

void engine_tick(Engine *e, float real_dt)
{
    if (real_dt > MAX_FRAME_DT) real_dt = MAX_FRAME_DT;  /* e.g. after a stall */
    e->accumulator += real_dt;

    int steps = 0;
    while (e->accumulator >= SIM_DT && steps < MAX_STEPS) {
        /* snapshot the previous authoritative state for interpolation */
        world_snapshot_prev(&e->world);
        simulate(e, SIM_DT);
        e->accumulator -= SIM_DT;
        ++steps;
    }
    e->alpha = e->accumulator / SIM_DT;   /* render interpolation factor */
}
```

Why fixed step:
- physics is stable and tunable
- gameplay is reproducible (replays, netcode, deterministic tests)
- all systems see the same `dt`, so no per-system time slicing

Why clamp: without `MAX_STEPS`, a slow frame makes you run more steps, which
makes you slower, which runs more steps — the **spiral of death**.

Render interpolation: keep `prev` and `curr` positions for renderables and lerp
by `alpha`; never feed a variable `dt` into the authoritative sim.

Pause/time scale: keep the accumulator; zero or scale `real_dt` before the loop.
A debug "step one frame" button adds exactly `SIM_DT` and runs one step.

---

## 4. The frame, step by step

```c
void engine_frame(Engine *e)
{
    float real_dt = platform_frame_dt(&e->platform);

    /* [1] BEGIN FRAME ---------------------------------------------------- */
    mu_arena_reset(&e->frame_arena);       /* all per-frame allocation dies here */
    input_begin(&e->input);

    /* [2] INPUT ---------------------------------------------------------- */
    PlatformEvent ev;
    while (platform_poll_event(&e->platform, &ev))
        input_feed(&e->input, &ev);
    input_resolve_actions(&e->input);

    /* [3]+[4] TIME and SIMULATE ------------------------------------------ */
    engine_tick(e, real_dt);

    /* [5] TRANSFORMS ----------------------------------------------------- *
     * SoA hierarchy; parent index < child index (topological order).       *
     * Only entities flagged as moved (or always, if cheaper) propagate.    */
    sys_transforms(&e->transforms);

    /* [6] ANIMATION ------------------------------------------------------ *
     * sample packed curves, blend, write skin matrices.                    */
    sys_animate(&e->anim, real_dt);         /* presentation: real dt is fine */

    /* [7] CULL + LOD ----------------------------------------------------- *
     * frustum test into a visibility bitset; assign LOD buckets.           *
     * Cull first: only survivors enter the render list.                    */
    sys_cull(&e->transforms, e->world.camera.frustum, &e->cull);
    sys_lod_assign(&e->world, &e->cull, e->world.camera);

    /* [8] BUILD RENDER LIST ---------------------------------------------- *
     * gather visible renderables into a flat packet array.                 */
    render_list_begin(&e->render_list);
    sys_build_render_list(&e->world, &e->cull, &e->render_list);
    render_list_sort(&e->render_list);        /* opaque f2b; transparent b2f */

    /* [9] SUBMIT --------------------------------------------------------- */
    gpu_begin_frame(&e->gpu);
    render_submit(&e->gpu, &e->render_list, &e->assets, &e->transforms, e->alpha);

    /* [10] PRESENT ------------------------------------------------------- */
    gpu_present(&e->gpu);

    /* [11] END FRAME ----------------------------------------------------- */
    frame_stats_end(e);                       /* per-system timers -> budget */
    assets_pump(&e->assets, &e->frame_arena); /* finish/upload completed loads */
}
```

Notes on each step:

- **[1] Begin frame.** `mu_arena_reset` is the whole "clean up last frame"
  story: one pointer assignment. Anything that outlives the frame was allocated
  in the level arena or a pool — not here.
- **[2] Input.** Events are drained, actions derived once.
- **[3]+[4] Time and simulate.** Fixed step, possibly several steps. All
  authoritative mutation happens here — no mutation during render.
- **[5] Transforms.** Movement updated local transforms; propagate local→world
  in one linear pass. Store hierarchy as parent indices in DFS order so
  `parent[i] < i` and no recursion/topological sort is needed.
- **[6] Animation.** Sampling/blending/skinning is presentation; it may use the
  real render `dt` and interpolation. Keep animation data as packed key arrays
  (times searched, payloads indexed once).
- **[7] Cull + LOD.** Frustum test writes bits; LOD assigns buckets. Rejecting
  early is cheaper than building then dropping.
- **[8] Render list.** Flat packets, then a specialized sort (counting/radix for
  discrete keys) and batching. This is where "sort only what needs sorting"
  applies.
- **[9] Submit.** Per-batch draws; instancing/indirect where mesh+material
  repeat. GPU reads buffers written in [5]/[6] — double-buffer if the GPU may
  still be reading last frame's data.
- **[10] Present.**
- **[11] End frame.** Stats, streaming uploads, telemetry. Nothing here should
  touch simulation state.

---

## 5. The job graph

Systems are nodes; data dependencies are edges. Independent systems run
concurrently over disjoint tables; dependent systems are gated by counters.

```
        movement ──────────────┐
             │                 │
             v                 v
        collision          animation sample
             │                 │
             v                 v
        gameplay/AI        skin matrices
             │                 │
             v                 │
        spawn/destroy drain    │
             │                 │
             └────────┬────────┘
                      v
                  transforms
                      │
                      v
                    cull
                      │
                      v
                 render list build
                      │
                      v
                    sort
                      │
                      v
                   submit
```

```c
/* independent: run in parallel over the same entity range */
mu_sched_parallel_for(e->sched, 0, alive_count, 1024,
                      MU_SCHED_PRIORITY_NORMAL, movement_worker, &mv);
mu_sched_parallel_for(e->sched, 0, anim_count, 512,
                      MU_SCHED_PRIORITY_NORMAL, anim_sample_worker, &an);

/* dependent stage: gated on completion counters */
mu_sched_counter counter;
mu_sched_counter_init(&counter, 0);

mu_sched_task_desc d = {
    .fn = collision_task, .user = &col,
    .priority = MU_SCHED_PRIORITY_NORMAL,
    .depends_on = NULL,
    .completion_counter = &counter,
};
mu_sched_submit(e->sched, &d);
mu_sched_counter_wait(&counter);        /* collision is done */
sys_gameplay(e);                        /* single-writer stage */
```

Rules:
- Parallelize only systems with **one writer per table**.
- Keep **thread-local** output buffers; gather at stage boundaries.
- Keep per-thread counters cache-line padded; no false sharing.
- Prefer **fused parallel work** over many tiny tasks (task overhead dominates).
- Batch synchronization at stage boundaries, not per element.

---

## 6. Double buffering and the GPU edge

The GPU may still be reading last frame's vertex/instance/transform buffers while
the CPU writes this frame's. Two strategies:

```
(1) double buffer: write frame N+1's copy while the GPU reads frame N's
    buffer A <-> buffer B, swap each frame

(2) ring buffer of N slots sized to the frames-in-flight
    write slot (frame % N)
```

Never let the sim and render layouts be the same array. Production code keeps a
**GPU representation** (packed, interleaved, quantized, possibly compressed)
separate from the **simulation representation** (SoA, full precision, easy to
iterate).

```
  SIM (CPU/SoA)                 GPU (packed)
  positions_x[]  ---+
  positions_y[]  ---+--> pack -->  {px,py,pz, packed_normal} per vertex
  positions_z[]  ---+
```

This is the "one thing, many representations" rule applied to the frame.

---

## 7. Shutdown

Symmetric to init, in reverse: stop jobs (`mu_sched_exit`, `mu_ts_exit`), release
device resources, free arenas/pools, destroy platform. `mu_mmgr` can verify no
leaks in debug builds.
