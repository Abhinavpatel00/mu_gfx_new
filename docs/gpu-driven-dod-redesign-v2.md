# GPU-driven DOD renderer: v2 (target-game rewrite)

Status: **v2 spec.** Companion to `gpu-driven-dod-redesign.md` (v1), which stays
on disk intact. Where the two disagree, **v2 wins**; §0 lists every divergence
so the delta is auditable rather than discovered. Doctrine: `AGENT.md` §1,
skills `dod-performance` and `game-engine-dod`, `mu_gfx/AGENTS.md`.

Target: a renderer good enough to ship a Slime Rancher-shaped game — thousands
of individually-simulated creatures that all move every frame, terrain, foliage,
jelly-like vertex deformation, shadowing, water/goo later.

**Prime directive, unchanged:** lay data out for the loop you will run, remove
every branch the layout makes unnecessary, own memory explicitly, and measure
everything you claim. No mesh shaders, no task shaders, no render-graph layer.
Vertex pull + indexed `vkCmdDrawIndexedIndirectCount`.

---

## 0. What changed from v1, and why

| # | v1 | v2 | why |
|---|---|---|---|
| 1 | one `gpu_inst[]`, static instances cost 0 B/frame | one `gpu_inst[]` **partitioned** into a dynamic prefix and a static suffix (§3) | v1's headline claim is false for the target game: *every* creature moves every frame. Keeping one address space is worth far more than the upload cost. |
| 2 | `CullRow` 8 B, one row type for all classes | unchanged, but the rule is now stated generally: **a row carries only what its transform loads** (§5.1) | wobble and terrain need different load sets; making that explicit is what keeps the no-branch rule mechanical |
| 3 | vertex deformation unaddressed | `wobble` class + `inst_wobble[]` side table (§6) | creatures jiggle. Cheaper than skinning and sufficient for jelly motion |
| 4 | terrain absent | terrain as **chunked heightfield**, its own class (§7) | the game needs ground; "the positions have wheat" |
| 5 | dispatch counts CPU-known | cull and both compaction dispatches are **indirect** (§9) | creature counts change; dispatching `max_survivors` threads when 30 survive is pure waste |
| 6 | `cs_prefix` implied a parallel scan | **explicit two-level scan**, dual-output (§8.2) | the shipped code is `[numthreads(1,1,1)]` — one thread scanning all G groups, serialized between cull and scatter. This is the largest single defect in the current build |
| 7 | `vis[]` holds a plain slot (§8) | unchanged as the **norm**, but flagged: shipped `vs_main` unpacks `(slot, mesh)` out of the survivor key (§10) | mesh is uniform per draw; decoding it per lane is a gather the hardware never needed |
| 8 | barriers hand-written over the whole `gpu_pool` | `BufferAccess` in `PassDesc`; barriers derived (§11) | a full-pool barrier per transition, ×4 per view, is both slow and fragile |
| 9 | water/goo | **deferred** to its own doc | no renderer, no water |
| 10 | static-view amortization (§17) | **deferred** | nice, but it is an optimization on top of a working frame |

Unchanged from v1 and not restated here: the six-transform model, render
classes, views, batches, the three-buffer residency rule, capacity/overflow
rules, counters, shader invariants, and the flag→table map. See
`gpu-driven-dod-redesign.md` §§1–5, 14, 16–18, 21, 22.

---

## 1. Pass inventory

| # | transform | input | output | freq |
|---|-----------|-------|--------|------|
| T1a | upload dynamic instances | `inst_dynamic[]` (CPU mirror) | `gpu_inst[0..dyn_count)` | per frame |
| T1b | upload static instances | `inst_static[]` (CPU) | `gpu_inst[dyn_cap..)` | on change |
| T2 | cull per (class, view) | `cull_row[]`, `gpu_inst[]`, `cull_mesh[]`, `inst_wobble[]`, view consts, Hi-Z | `survivors[]`, `survivor_count`, `cull_args` | per frame |
| T3 | compact per (class, view) | `survivors[]`, `survivor_count` | `vis_all[]`, `draw_all[]`, `draw_count` | per frame |
| T4 | Hi-Z build per view | depth image | Hi-Z chain | per frame |
| T5 | draw per (class, view) | `draw_all[]`, `draw_count`, `vis_all[]` | framebuffer | per frame |
| T6 | counters readback | GPU counters | `SceneCounters` (lag 2) | per frame |

Nine transforms counting the T1 split. T4 runs after T5 and feeds the next
frame's T2 — no same-frame two-phase occlusion, for v1 §9's reason.

---

## 2. The model in one picture

```
game owns CPU truth
  ├─ dynamic instance array   (creatures, goo, player — move every frame)
  └─ static instance array    (terrain props, buildings, decor — written once)
                    │
                    ▼  T1a/T1b  →  gpu_inst[]  (ONE address space)
                              [0, dyn_cap)          = dynamic, rewritten per frame
                              [dyn_cap, capacity)   = static,  uploaded once
                    │
                    ▼  per (render_class, view)
  T2  cull ──► survivors[]  +  survivor_count  +  cull_args
                    │
                    ▼
  T3  count ─► scan ─► vis_all[] + draw_all[] + draw_count
                    │
                    ▼
  T5  vkCmdDrawIndexedIndirectCount ──► framebuffer
                    │
                    ▼
  T4  Hi-Z from this frame's depth ──► next frame's T2

  T6  counters readback (lag 2, never a stall)
```

---

## 3. `gpu_inst[]` — one table, partitioned, not duplicated

The instance row is unchanged from v1 §2. What changes is **where the write
traffic goes**.

```c
/* 20 bytes. Unchanged. Every field is an arithmetic input; none is ever tested. */
typedef struct SceneInstance {
    float       pos[3];    /* 12 world-space position                      */
    SCENE_U32   quat;      /*  4 10-10-10-2 normalized quaternion           */
    SCENE_U16   scale;     /*  2 half uniform scale                        */
    SCENE_I16   lod_bias;  /*  2 signed LOD bias                           */
} SceneInstance;           /* 20 bytes */
```

### 3.1 The partition

```c
enum { SCENE_DYNAMIC_CAP = 8192 };   /* fixed at scene_create; instances ≤ this */

/* slot ∈ [0, capacity). The split point is a scene constant, not a per-row bit:

    slot <  dyn_cap   →  dynamic   rewritten every frame by T1a
    slot >= dyn_cap   →  static    uploaded once by T1b, never again          */

BufferSlice gpu_inst;    /* one buffer, one baked device address */
uint32_t    dyn_count;   /* CPU live count of dynamic instances */
uint32_t    static_count;
```

Why one table and not two buffers:

- **One address.** The shader loads `gpu_inst[slot]` with no base select and no
  branch. Two tables would mean two baked addresses and a per-row choice — which
  is a flag wearing a disguise, and v1's own rule forbids it.
- **One candidate row type.** `CullRow.slot` indexes the same space regardless of
  whether the creature moved this frame or was placed at load.
- **One dirty/upload story.** The only thing that changes is *which prefix of the
  array gets written*, and that is arithmetic on two CPU integers.

The cost is that the cull's sequential candidate read now gathers from two
regions of one array. On a working set of 8 K × 20 B = 160 KB this is L2-resident
on any modern GPU; the gather was already random before the partition (§6.2 of
the root `AGENT.md`: the problem is never "pointers", it is dependent addresses,
and a fixed-stride gather over 160 KB is not dependent).

### 3.2 The upload

The dynamic prefix is written every frame. There is no way around those bytes —
v1's "0 B/frame" was only ever true for a scene of statues. The question is
whether they cost a staging copy.

```c
/* Preferred: gpu_inst is allocated DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT.
   On the target hardware (integrated GPU, unified memory) this is free — it is
   the same memory. T1a is then a plain CPU store loop; no staging, no copy. */

for (uint32_t i = 0; i < s->dyn_count; ++i)
    ((SceneInstance *)gpu_inst.mapped)[i] = s->cpu_dynamic[i];

/* Fallback for a discrete GPU without a suitable memory type: one contiguous
   staging→device copy of dyn_count * 20 B. Never a per-slot copy. */
```

Sizing the budget honestly, at 8192 dynamic instances:

| path | bytes/frame | note |
|---|---:|---|
| mapped DEVICE_LOCAL (iGPU) | 164 KB | 164 KB of CPU stores, ~10 µs. No PCIe traffic. |
| staging fallback (dGPU) | 164 KB | one `vkCmdCopyBuffer`, ~2 µs at 12 GB/s |

Compare to the vertex+index stream for 8192 creatures at 500 verts each: ~82 MB.
**The instance upload is 0.2% of the frame's memory traffic.** Optimizing it
below this is waste; §20 of v1 makes the same argument for compaction.

Decision: use the mapped path when available, fall back otherwise. Measure which
one you got and put it in the HUD — a silent fallback that costs 164 KB/frame is
exactly the kind of thing that gets discovered a year later.

### 3.3 Instance lifecycle

```c
uint32_t scene_instance_create(Scene *s, const SceneInstanceDesc *d);   /* dynamic */
uint32_t scene_instance_create_static(Scene *s, const SceneInstanceDesc *d);
```

Dynamic instances occupy `[0, dyn_count)` densely and are swap-removed. Static
instances are append-only into `[dyn_cap, dyn_cap + static_count)`: a level's
buildings and props do not churn, and a dense array with no removal path is the
simplest thing that can be correct.

Both return a slot in the same space, so a caller cannot tell them apart, which
is the point: **whether something moves is a placement decision made once, not a
property carried per frame.**

---

## 4. Render classes

A class is fixed pipeline state — vertex stage, raster state, blend state, index
width. Membership is resolved once at insert. Unchanged from v1 §3.1, extended
with the two the target game needs:

```
class            vertex stage    raster      blend        depth       index
---------------  --------------  ----------  -----------  ----------  ------
opaque           rigid           back-cull   off          write       16
doubleside       rigid           no-cull     off          write       16
wobble           wobble          back-cull   off          write       16
terrain          terrain         back-cull   off          write       16
blend            rigid           back-cull   alpha-blend  no-write    16
skin             skin            back-cull   off          write       16
opaque32         rigid           back-cull   off          write       32
```

`wobble` (§6) and `terrain` (§7) are new. Both are **a table + a cull entry + a
pipeline**, never an `if` added to an existing kernel.

Shadow rendering is a **view** over the caster classes, not a class — unchanged
from v1 §3.2.

---

## 5. Candidates

### 5.1 The general rule

> A candidate row carries exactly the fields its transform loads. Nothing more.
> If a transform needs a field no sibling transform needs, that transform's
> class gets its own row type — not a wider shared row with an unused field.

This is v1 §5's rule made general. It is what lets `wobble` and `terrain` exist
without a single byte of waste in the classes that do not need them.

```c
/* 8 bytes. The only row type used by opaque/doubleside/blend/skin/opaque32. */
typedef struct SceneCullRow {
    SCENE_U32 slot;   /* 4 index into gpu_inst[]        */
    SCENE_U16 mesh;   /* 2 index into cull_mesh[]       */
    SCENE_U16 pad;    /* 2                              */
} SceneCullRow;
_Static_assert(sizeof(SceneCullRow) == 8, "SceneCullRow is 8 bytes");
```

### 5.2 Wobble candidates

`wobble` needs the instance plus three per-instance parameters. Following §5.1
the parameters live in a **side table indexed by the same slot**, not in a wider
candidate row:

```c
/* 8 bytes. Sized dyn_cap, not capacity: static props do not wobble, so paying
   for them is exactly the waste §5.1 forbids. */
typedef struct SceneWobble {
    SCENE_U32 phase_amp;   /* 4  phase:16 | amplitude:16 (unorm)  */
    SCENE_U32 freq_sincos; /* 4  sin(w*t):16 | cos(w*t):16         */
} SceneWobble;
```

`freq_sincos` is computed on the **CPU** while it is already writing the
instance row for the frame, so the GPU does one multiply instead of a `sin`.
Preparation over execution, per `mu_gfx/AGENTS.md`.

The candidate row for `wobble` stays 8 bytes. The wobble *cull* kernel reads
`inst_wobble[slot]`; the wobble *draw* kernel reads it too. Two readers, one
row, no branch.

### 5.3 Terrain candidates

A terrain chunk is a candidate like anything else: `slot` = chunk slot,
`mesh` = the shared patch template id. §7.

---

## 6. `wobble` — vertex deformation as a class

Creatures jiggle. There are two ways to deform vertices per instance: skinning
and a cheap analytic wobble. Wobble first, for three reasons:

1. It is 8 bytes per instance instead of a palette of `joint_count × 64 B`.
2. Pose dedupe (§12 of v1) does not apply — there are no poses.
3. It is a separate pipeline; when skinning lands it is a *seventh* class, and
   the wobble path stays for goo, water, foliage wind, and impact ripples.

The deformation itself is arithmetic on data already in hand:

```slang
/* wobble vertex stage. Params come from inst_wobble[slot], transform from
   gpu_inst[slot]. Both are per-lane loads; nothing is tested. */
float  phase = f16tof32(w.phase_amp & 0xFFFFu) * 6.2831853f;
float  amp   = f16tof32(w.phase_amp >> 16);
float  w     = unpack_half(w.freq_sincos);          /* cos/sin precomputed */

float3 local = unpack_vertex(sm.vertex_stream[vid]).pos;
float  t     = f16tof32(inst.scale);                /* height above the instance origin */
float  k     = phase + t * 2.0f;                    /* travelling wave up the body   */
float2 j     = float2(cos(k), sin(k)) * amp * w;    /* lateral displacement           */
float3 world = quat_rotate(inst.quat, local * f16tof32(inst.scale)) + j
             + float3(0.0f, (1.0f - cos(k)) * amp * w.y, 0.0f);
```

The `cos`/`sin` pair is the only transcendental, and it is per-vertex rather than
per-instance. If it ever shows up in a profile, the fix is a polynomial
approximation over `[0, 2π)`, not a restructure — note it and move on.

**Amplitude and frequency are authored per instance, not per class.** The class
carries the *capability*; the row carries the *amount*. That is the difference
between a table and a flag, and it is why `wobble` needs no per-row flag to
disable itself.

---

## 7. Terrain — a chunked heightfield is a class

> "The wheat doesn't have positions. The positions have wheat."

Terrain is the case where the world already provides the locations. There are no
terrain objects; there is a grid, and each chunk of it is one candidate.

### 7.1 The data

```c
/* One global heightfield. Only the cells the terrain actually covers are
   stored; chunk N covers cells [N*CHUNK_CELLS, (N+1)*CHUNK_CELLS). */
typedef struct SceneHeightfield {
    SCENE_U32 addr;     /* 4 device address of float height[]      */
    SCENE_U32 origin_x; /* 4 world x of cell (0,0), in half-float  */
    SCENE_U32 origin_z; /* 4 world z of cell (0,0), in half-float  */
    SCENE_U32 nx;       /* 4 cells along x                          */
    SCENE_U32 nz;       /* 4 cells along z                          */
    SCENE_U32 cell_size;/* 4 world size of one cell, float          */
    SCENE_U32 chunk_n;  /* 4 cells per chunk side                   */
} SceneHeightfield;      /* 28 bytes. One per terrain, read by every chunk. */
```

Height is `f32`, not `f16`. This is the one place the compression is wrong:
terrain height differences *are* the silhouette, a half-float at 128 m has ~6 cm
of error, and 6 cm of stair-stepping on a horizon is visible. Height is also the
smallest table in the renderer (a 256×256 field is 256 KB, read once per frame
per chunk-vertex), so there is nothing to win by compressing it.

```c
/* 16 bytes. One per terrain chunk. Read by T2 and by the terrain VS. */
typedef struct SceneChunk {
    SCENE_U32 cell_first; /* 4 first cell of this chunk in height[]   */
    SCENE_U16 chunk_x;    /* 2 chunk grid coordinate                  */
    SCENE_U16 chunk_z;    /* 2                                       */
    float      min_y;     /* 4 lowest cell (bounds; not a flag)      */
    float      max_y;     /* 4 highest cell                          */
} SceneChunk;
_Static_assert(sizeof(SceneChunk) == 16, "SceneChunk is 16 bytes");
```

`min_y`/`max_y` are recomputed when the field is edited (level load, or a
terrain tool at edit time) and are **bounds, not eligibility**. The cull derives
the chunk's bounding sphere from them arithmetically, exactly as it derives an
instance's world sphere in v1 §2.

### 7.2 Why chunks and not a whole-terrain draw

A single draw of the entire terrain has no culling, no LOD, and one enormous
vertex fetch. Chunking makes the existing machinery do the work for free:

- **Cull** — a chunk is a candidate; frustum and Hi-Z reject it whole.
- **LOD** — a distant chunk draws a 1-in-4 subsampled grid instead of the full
  one. Same `LodRow` mechanism as every other mesh: a rung is a different
  `(first_index, index_count)` pair, so terrain LOD needs no new code.
- **Shadow** — a chunk is a candidate in the shadow view like anything else.

A 256×256 field at `CHUNK_CELLS = 32` is an 8×8 chunk grid = 64 candidates.
Chunk size is the LOD granularity and the cull granularity at once; 32 is a
reasonable default and the only number that needs tuning.

### 7.3 The vertex stage

Terrain has no vertex arena and no index buffer. The VS pulls cell coordinates
from `SV_VertexID` and reads height directly:

```slang
/* terrain VS. Index -> cell -> height. The patch template (mesh) supplies the
   grid topology: vertex id -> (x, z) within the chunk. */
uint2 cell = patch_cell[vid];             /* mesh supplies this, uniform */
uint   g   = chunk.cell_first + cell.y * chunk_n + cell.x;
float  y   = height[g];
```

The patch template is a tiny index-buffer-only mesh shared by every chunk
(`cell_first` differs per chunk). It is an ordinary mesh in the v1 sense, so it
lands in `cull_mesh`/`shade_mesh` and needs no special case.

---

## 8. T2/T3 — cull and compact, corrected

### 8.1 T2 cull

Unchanged in shape from v1 §6: one thread per candidate, derive the world
sphere, two visibility exits, one atomic per survivor.

Two corrections:

```c
/* (a) indirect dispatch count, written by the kernel itself */
if (group_index_x == 0u) InterlockedMax(pc.cull_args[0], group_index_x + 1u);
/* pc.cull_args is a VkDispatchIndirectCommand triple; y/z are written once by
   the CPU at scene_create as 1. The cull writes only .x. T3 then dispatches
   from survivor_count with no CPU involvement and no upper-bound guesswork. */

/* (b) the dynamic/static partition costs the cull nothing */
SceneInstance inst = pc.gpu_inst[cr.slot];   /* one load, either region */
```

### 8.2 T3 compact — the real fix

The shipped `cs_prefix` is `[numthreads(1, 1, 1)]`: **one GPU thread** looping
over all `G` groups, serializing between cull and scatter. For a target-game
scene (`G` ≈ mesh_count × lod_count ≈ 800) that is a few microseconds of
single-threaded latency on every view, every frame, for no reason.

The scan produces **two** exclusive scans from one pass:

| output | from |
|---|---|
| `vis_base[g]` | exclusive scan of `group_count[g]` |
| `cmd_index[g]`, `draw_count` | exclusive scan of `(group_count[g] > 0)` |

```c
/* Stage 1: per-block scan. ELEMS_PER_THREAD is a compile-time constant chosen
   against maxComputeWorkGroupInvocations; nothing hard-codes a group size. */
[shader("compute")] [numthreads(256,1,1)]
void cs_scan_block(uint3 gid, uint3 lid)
{
    uint  g   = gid.x * 256u * ELEMS + lid.x;
    uint  cnt = (g < G) ? group_count[g] : 0u;        /* pad, not branch out */
    /* Hillis-Steele inclusive scan in workgroup shared memory over two lanes
       (cnt, emit). Standard, no workgroup-scope prefix assumption. */
    ...
    if (lid.x == 0u) block_sum[gid.x] = total;       /* both lanes */
}

/* Stage 2: scan the block sums. One workgroup; blocks ≤ 1024 by construction
   because G ≤ 1024 * 256 * ELEMS and ELEMS is a compile-time constant. */
[shader("compute")] [numthreads(256,1,1)]
void cs_scan_blocks(...) { ... block_offset[gid.x] = ...; }

/* Stage 3: apply offsets, zero cursors, emit commands. One thread per group. */
[shader("compute")] [numthreads(256,1,1)]
void cs_emit(uint3 gid, uint3 lid)
{
    uint g = ...;
    vis_base[g] += block_offset[gid.x];
    cursor[g]    = 0u;
    if (group_count[g] > 0u) { draws[cmd_index[g]] = template[g]; }  /* uniform branch on a derived count, not a row flag */
}
```

Three dispatches, `O(G/256)` wide, replacing one serial thread. When
`G ≤ 256 * ELEMS` the block dispatch is 1 workgroup and stages 2–3 are the same
launches with a trivially-zero offset — the code path is identical, so there is
no "small scene" special case to get wrong.

### 8.3 T3 scatter

Unchanged, and now dispatched **indirectly** from `survivor_count`:

```slang
[shader("compute")] [numthreads(64,1,1)]
void cs_scatter(uint3 tid)
{
    if (tid.x >= pc.survivor_count[0]) return;      /* the only exit */
    uint key = pc.survivors[tid.x];
    uint g   = group_of(key);
    uint local;
    InterlockedAdd(pc.cursor[g], 1u, local);
    pc.vis[pc.vis_base[g] + local] = key & 0xFFFFu;  /* slot only — see §10 */
}
```

The shipped code dispatches this `(max_survivors + 63)/64` times regardless of
how many survived. With `max_survivors = 8192` that is 128 workgroups launched
to service, typically, 30 survivors. Every one of them loads
`survivor_count`, evaluates the guard, and retires.

---

## 9. Indirect dispatch, everywhere

| dispatch | count source | written by |
|---|---|---|
| T2 cull | `cull_args` (GPU) | cull kernel's `InterlockedMax` |
| T3 count | `survivor_count` (GPU) | T2 |
| T3 scan blocks | `G`, CPU | scene_create (known, CPU-owned) |
| T3 scan block-sums | `ceil(blocks/256)`, CPU | scene_create |
| T3 emit | `G`, CPU | scene_create |
| T3 scatter | `survivor_count` (GPU) | T2 |
| skinning (v1 §12) | `survivor_count` (GPU) | T2 |

Result: **CPU work per frame is proportional to (classes × views), never to
entity count.** Creatures can spawn, die and change class without the CPU
touching a dispatch size. This is the single largest CPU-side win in v2 and it
is what makes the "thousands of moving creatures" case unremarkable.

The dispatch-args buffers are `VkDispatchIndirectCommand` triples allocated per
view per lane, zeroed and given `y = z = 1` at scene_create.

---

## 10. Identity in the draw — the shipped divergence

v1 §8 specifies, correctly:

- `vis_all[]` holds **a plain `uint32_t` instance slot**.
- The VS gets mesh and LOD from `gl_DrawID → draw_all[view].mesh_lod`.

The shipped `vs_main` instead unpacks both out of the survivor key:

```slang
/* WRONG — shipped. A per-lane gather + decode of a value that is uniform
   across the whole draw. */
SceneU32 entry = pc.vis[iid];
SceneU32 slot  = entry & 0xFFFFu;
SceneU32 mesh  = (entry >> 16) & 0x3FFFu;
```

Both are 4 bytes, so this is not a bandwidth argument. The argument is:

1. **The broadcast is already free.** `gl_DrawID` is uniform across the draw, so
   `draw_all[draw_id].mesh_lod` is one load hoisted out of the per-lane path,
   while the key version is a load every lane performs and then agrees on.
2. **`draw_all[]` is already resident and already cache-hot** — it was read by
   the hardware a few hundred nanoseconds ago to launch the draw. `vis_all[]` is
   cold by comparison.
3. **It keeps the compaction pass honest.** T3 writes `vis_all[]`; if `vis_all[]`
   must also carry mesh, T3 has to re-derive mesh from the group it is already
   writing — an encode that exists only to be decoded again.

v2 keeps the v1 §8 rule. Change `vs_main` to the broadcast form and have `cs_scatter`
store `key & 0xFFFFu`, which §8.3 above already does.

---

## 11. Sync — `BufferAccess` in `PassDesc`

The shipped `scene_frame` writes four barriers over the **entire** `gpu_pool`:

```c
/* WRONG — shipped. Whole-pool, four times, per view. */
cmd_buffer_barrier(cmd, vk->gpu_pool.buffer, 0, vk->gpu_pool.size_bytes, ...);
```

A whole-pool barrier is the most expensive legal way to say "these 32 KB are
ready". It also makes every unrelated buffer in the pool participate in the
memory-dependency graph, which is precisely the kind of accidental coupling that
turns into a mystery 2 ms six months later.

v1 §15 already specifies the fix; it was never implemented. Extend the existing
pass API — **do not replace it**:

```c
typedef struct BufferAccess {
    BufferSlice          slice;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2        access;
} BufferAccess;

typedef struct PassDesc {
    /* ... existing: colors, color_count, depth, shader_reads, shader_writes ... */
    const BufferAccess *buf_reads;       /* new */
    uint32_t            buf_read_count;
    const BufferAccess *buf_writes;      /* new */
    uint32_t            buf_write_count;
};
```

Call sites name intent; `begin_pass` converts each `BufferAccess` to a
`VkBufferMemoryBarrier2` against exactly that slice's range and batches them with
the image barriers it already batches. No hand-written `VkMemoryBarrier2` at any
scene call site.

Per-frame barrier inventory after this change — **7 buffer barriers total**,
independent of view count, down from 4 × views over the whole pool:

```
1  upload writes          -> cull reads
1  cull writes            -> scan/scatter read
1  scan writes            -> emit/scatter read
1  emit writes            -> scatter + draw read
1  draw                   -> Hi-Z build reads depth
1  Hi-Z build             -> next frame's cull
1  counters               -> copy-to-host
```

The image barriers for attachments and Hi-Z continue to be derived from
`ImageState` as today, unchanged.

---

## 12. Hi-Z and LOD

Unchanged from v1 §9 and §10 — previous-frame depth pyramid, explicit 2×2
min-`Load`, the one-level-finer mip correction, `scene_disable_occlusion(frames)`
for camera cuts. The two locked invariants stay:

1. Transparent passes never write depth while Hi-Z is on.
2. No same-frame two-phase occlusion.

v2 additions:

- **LOD for terrain is free** (§7.2) — a coarser rung is a different index range.
- **LOD for wobble is free** — dropping a distant creature to LOD2 means fewer
  vertices running the same wobble math, which is where the cost is.
- The LOD walk uses the world sphere the occlusion test already produced, and
  needs no `#ifdef` while assets carry a single rung.

---

## 13. Deferred

| item | why deferred |
|---|---|
| water / goo | no renderer yet. When it lands it is a heightfield-style grid pass (§7 is the template) reading a CPU-simulated field, or a compute pass writing that field — its own doc. |
| static-view amortization | v1 §17, kept valid. It is an optimization on top of a frame that works. |
| skinning class | v1 §12. `wobble` covers jelly motion and foliage wind; skeletal characters need the class and the pose dedupe. |
| clusters / meshlets | v1 §10. Requires cluster tables in the asset format and a second sync point. Measure at 100 K+ triangles before paying for it. |
| collective LOD | v1 §11.4. Same. |

---

## 14. Byte budget (target game: 8192 creatures, 6 meshes, 64 terrain chunks)

| table | bytes | count | total | per frame |
|---|---:|---:|---:|---:|
| `gpu_inst` row | 20 | 8192 | 164 KB | **164 KB rewritten** (§3.2) |
| `cull_row` | 8 | 8192 + 64 | 66 KB | 66 KB read |
| `cull_mesh` / `shade_mesh` | 32 / 16 | 6 | 0.3 KB | 0.3 KB read |
| `chunk` | 16 | 64 | 1 KB | 1 KB read |
| `heightfield` f32 | 4 | 65 536 | 256 KB | partial read |
| `draw_all` stride | 32 | ~200 groups | 6 KB | 6 KB write + read |
| `vis_all` | 4 | survivors | ~30 KB | ~30 KB |
| `inst_wobble` | 8 | dyn_cap | 64 KB | 8 B/moved instance |

CPU-visible per frame: **~330 KB**. GPU-visible per frame including the vertex
stream: **~80 MB** (creature geometry dominates at ~1000× everything else).

The conclusion of v1 §20 holds and gets sharper: **the performance work is LOD,
Hi-Z, and vertex format.** Instance residency is correctness and CPU cost, not
the bottleneck. Optimizing §3.2 below a single contiguous write would be
optimizing 0.2% of the frame.

---

## 15. Implementation order

Ordered by *risk × dependency*, not by feature appeal. Each step leaves a
visible, correct frame.

| step | change | risk | why here |
|---|---|---|---|
| 1 | `BufferAccess` in `PassDesc`; `scene_frame` declares its reads/writes | low | §11. Pure win, no visual change, removes the whole-pool barriers every later step would inherit. |
| 2 | parallel T3 scan (§8.2) | low | Biggest existing defect. Pure GPU time, no visual change (modulo a real bug being uncovered). |
| 3 | indirect dispatches (§9) | low | Removes `max_survivors`-sized launches. No visual change. |
| 4 | `vis[]` = slot, `gl_DrawID` identity (§10) | low | No visual change; makes the doc and the code agree. |
| 5 | `gpu_inst` partition + mapped dynamic upload (§3) | medium | Touches instance lifecycle. Verify static props still draw and moved creatures still update. |
| 6 | `inst_wobble` + wobble class (§6) | medium | New pipeline, new VS. First visual change; screenshot it. |
| 7 | Hi-Z (§12) | medium | Needs the T4 depth read wired to the pass API from step 1. Biggest perf win. Screenshot *changes* — that is the point. |
| 8 | LOD ladders (§12) | medium | Needs meshes with rungs in the asset format. Terrain LOD lands first (cheapest to author). |
| 9 | terrain class + chunks (§7) | high | New geometry path, new bounds derivation. Do it after everything else is solid. |
| 10 | shadow view (v1 §3.2) | high | Second view stresses the per-view lane arrays for the first time. |

Deliberately **not** in this order: skinning, clusters, water, static-view
amortization. Each is a real feature with a real cost, and none of them is on
the critical path to "a creature walks around".

---

## 16. Review checklist

```
LAYOUT
[ ] gpu_inst is ONE table, partitioned by a scene constant — not two buffers
[ ] a candidate row carries only what its transform loads; wobble/terrain get
    side tables rather than a wider shared row
[ ] hot rows hold no pointers; device addresses only in GPU tables
[ ] every shared size _Static_assert'ed in scene_shared.h

CONTROL FLOW
[ ] no data-dependent branch on a row field in any T1–T6 loop
[ ] class and view are table membership + pipeline choice, never a runtime switch
[ ] the partition boundary is two CPU integers, never a per-row bit

MEMORY / DISPATCH
[ ] CPU per-frame work is proportional to (classes x views), never entity count
[ ] every data-dependent dispatch is indirect
[ ] capacity fixed at scene_create; overflow clamps + counts, never grows
[ ] gpu_inst is mapped DEVICE_LOCAL when the memory type allows; the fallback
    is one contiguous copy and is reported in the HUD

SYNC
[ ] no hand-written VkMemoryBarrier2 at any scene call site
[ ] no barrier spans more than the slices it names
[ ] wait_idle absent from the frame loop; per-view per-lane arrays never alias

PROCESS
[ ] before/after GPU timer per pass per step
[ ] a screenshot after every step that changes the image, approved by eye
[ ] compiles warning-clean with -Wall -Wextra -Werror
```

---

## 17. What v1 got right and v2 keeps without restating

The class/view/batch decomposition, the 20-byte instance row with derived
bounds, the two mesh tables split by reader, the three-buffer residency rule,
`drawIndirectFirstInstance` as a hard requirement, previous-frame Hi-Z over
same-frame two-phase occlusion, counters at lag 2, and the flag→table map. If
you are looking for the doctrine rather than the delta, v1 §§2–5, 14, 16–18,
21, 22 still stand.
