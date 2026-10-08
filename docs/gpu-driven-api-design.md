# GPU-driven renderer: DOD API design (target)

Status: design target + partial implementation. The static GPU-driven core
described here is implemented (see `rendererdesign/improved-threed.md` for
what shipped); this doc is the authority for the *target* frame contract,
GPU-side layouts, sync model, and bandwidth budget. No migration phases —
either this is the right target or it isn't.

Doctrine: `AGENT.md` + `docs/mycstyle.md` + skills `dod-performance` and
`game-engine-dod`. Program = data + transformation. Layout follows the loop;
existence is state; IDs over pointers; preparation vs execution; measure
everything claimed.

Scope: specify the  public API, the GPU-side data layouts, the sync model and the bandwidth budget that make the
renderer both faster and more flexible than either.

Numbers marked *(measured)* were printed by compiling the real headers
(`sizeof`) or taken from a run log. Numbers marked *(model)* are arithmetic from
those counts and are labelled with the assumption.

---

## 0. How the three compare, in mechanisms (DOD reading)

Everything below is about *who owns visibility and command generation*, because
that is the only axis that separates a GPU-driven renderer from a CPU-driven one
with an indirect draw bolted on. DOD framing: name each pass, its input table,
its output table, and its frequency — then choose the layout from the access
pattern (skill `dod-performance` step 1–2).

### 0.1 mu_gfx (this repo) — GPU owns visibility

```
game writes SceneInstanceSource[]            (main.c, CPU)
  -> CPU compacts: SceneGpuInstance[] (80B) + SceneCandidate[] (8B)   scene3d.c
  -> upload (frame cmd buffer + staging ring, no submit/wait)
  -> cull compute: frustum (6 dot) + Hi-Z occlusion    writes draws[].instanceCount
                                                       writes visible[]
  -> one vkCmdDrawIndexedIndirect per mesh slot, index buffer bound
     (pre-Count snapshot; steady state is one Count draw per view, 9.1)
  VS: vertices pulled from a device address, rows from the instance record
```

Facts (verified against `src/three_d/scene3d.h` + `src/scene3d_shared.h`):

- `ScenePush` is 248 bytes and near the 256-byte limit; Hi-Z params are already
  bit-packed into the two former pad words (`src/scene3d_shared.h`).
- `SceneDraw` is 32 bytes of which 12 are pad; the first 20 are exactly
  `VkDrawIndexedIndirectCommand`. Stride is 32, not 20.
- Per-draw work: `vkCmdBindIndexBuffer` + a 248-byte push + one indirect draw,
  once per mesh slot (6 in the cubepets demo).
- **Skinning is designed but not wired**: `SceneSkinJob` and the
  `skin_input`/`skinned_vertices`/`palette` buffers exist,
  `skin_job_count` is always 0, and there is no skinning pipeline in
  `scene3d_init`. `SCENE_ASSET_SKINNED` is never consumed by a dispatch.
- Doc drift is a recurring bug class: `SceneGpuMaterial` comment claimed 112
  bytes, it is 32; `SceneSkinJob` comment claimed 56, it is 40. Every
  GPU-shared size claim must be a `_Static_assert(sizeof(...) == N)`,
  never prose. DOD rule: the layout *is* the contract.

Per-frame bytes for 4096 instances / 6 mesh slots *(measured)*:

| stream | bytes/frame | MB/s @60 |
|---|---:|---:|
| `SceneGpuInstance[]` upload | 327,680 | 19.7 |
| `SceneCandidate[]` upload | 196,608 | 11.8 |
| `SceneDraw[]` upload | 192 | 0.01 |
| push data (1 cull + 6 draws) | 1,736 | 0.1 |
| **total** | **526,216** | **31.6** |

### 0.2 fukuna_engine — CPU-built MDI, GPU vertex pull, blocking upload

- `GpuInstance` is 96 bytes: `mat4 model` + `vec4 bounds` + 4×u32
  (`renderer_gpu_driven.h:17`).
- The CPU builds one `VkDrawIndirectCommand` **per (instance, mesh)** with
  `instanceCount = 1` and `firstInstance = i`, then uploads the array and issues a
  single `vkCmdDrawIndirect` (`main.c:2734`, `main.c:2803`).
- Draws are **non-indexed**: `vertexCount = mesh->lods[0].index_count`, and the
  shader fetches indices by hand. `docs/GPU_DRIVEN_RENDERING_PLAN.md` §"Correct
  indirect command type" says this is a known limitation.
- The per-frame upload is a **full GPU stall**: `vk_end_one_time_cmd` =
  `vkQueueSubmit` + `vkQueueWaitIdle` + `vkFreeCommandBuffers`
  (`helpers.h:227`), called every frame from `rendering_system_upload_frame`.
- `renderer_gpu_driven.c:77` — `rendering_system_record()` is `(void)`-casts and
  a comment: *"CPU draw path remains primary until compute path implemented"*.
- No cull / compaction / LOD compute shader exists in `shaders/`.
- Their own plan document lists the endpoint (compute cull → compute command
  generation → `vkCmdDrawIndexedIndirectCount`) as **future work**.

### 0.3 threedgame — per-mesh draws, one instance each, 128-byte draw records

- `GltfIndirectDrawData` is 128 bytes **per mesh**: 4 device addresses
  (vertex/index/joints/weights) + `mat4 model` + material id + skin offset +
  flags + `bloom_color[3]`. Uploaded **every frame**
  (`main.c:35`, filled at `main.c:2253`, uploaded at `main.c:2274`).
- `instanceCount = 1`, `firstInstance = i` (the *mesh* index) — one draw per
  mesh, and the instance transform is baked into the mesh's draw record
  (`main.c:1541`). Instancing is not used at all on this path.
- Draw count is CPU-written: `uint32_t draw_count = mesh_count` is uploaded each
  frame and consumed by `vkCmdDrawIndirectCount` (`main.c:2126`, `main.c:2314`).
- Vertices are packed to 16 bytes (`PackedVertex`), normals 10-10-10 + bitangent
  sign, tangent 8+8, uv half2 — the same budget as our `ScenePackedVertex`.
- Adds real renderer features we do not have: bloom, DoF, toon outline, analytic
  fog, a glTF loader with material texturing, audio, and a filesystem layer.
- Also has **no cull compute**: `grep -rl cull */shaders/` returns nothing in
  either upstream tree.

---

## 1. Mechanism-by-mechanism table

| | mu_gfx | fukuna_engine | threedgame |
|---|---|---|---|
| instance record | 80 B (`rows[3]`, bounds, tint) | 96 B (mat4, bounds, 4 ids) | none (transform inside the per-mesh draw record) |
| bounds | stored per instance | stored per instance | absent (no culling) |
| command generation | GPU (cull compute) | CPU array, uploaded | CPU array, uploaded |
| command stride | 32 B (`VkDrawIndexedIndirectCommand` + 12 pad) | 16 B, one per (instance,mesh) | 16 B, one per mesh |
| instances per draw | N (GPU-written `instanceCount`) | 1 | 1 |
| draw count | static per mesh slot, CPU-known | CPU-computed | CPU-written to a GPU buffer |
| indexed? | yes, index buffer bound, uint16 | no, manual index fetch | yes, via device address |
| vertex fetch | device address, pulled in VS | device address, pulls indices by hand | device address, pulled in VS |
| upload sync | frame cmd buffer + staging ring | `vkQueueWaitIdle` **per frame** | frame cmd buffer + staging ring |
| occlusion culling | Hi-Z, compute, previous-frame depth | none | none |
| LOD | none | none | none |
| skinning | designed, not wired | compute kernel exists | CPU-side per-mesh transforms |
| 2D | own stage + GPU sprite cull | sprite pipeline only | sprite pipeline only |
| pass API | `begin_pass(PassDesc)` resolves layouts and stages | raw `vkCmdBeginRendering` at call sites | raw `vkCmdBeginRendering` at call sites |
| image state | tracker per mip, `rt_transition_*` | tracker per mip (upstream of ours) | same |
| buffer state | none - hand-written `cmd_buffer_barrier` | none | none |
| deferred destroy | timeline `DeleteQueue` | none | none |
| memory accounting | per-tag VMA tracker + profiler panel | none | none |
| pipeline hot reload | `pipeline_mark_dirty` / `pipeline_rebuild` | same | same |

The single structural difference that matters: **in `mu_gfx` the CPU never learns
the visible count and never builds a command**, and in both upstream snapshots it
does both, every frame.

### 1.1 What the upstream snapshots do better, and what is worth taking

1. **The `PUSH_CONSTANT(name, BODY)` macro** (`threedgame/vk.h:1237`) declares a
   struct padded to exactly 256 bytes plus an unpadded `name##_init` for the C
   initializer, so `vkCmdPushConstants` always transfers 256 bytes. We
   deliberately do the opposite (push `sizeof(struct)`, currently 248) - cheaper,
   but it is why we ran out of push space for Hi-Z. Take the *idea* (a
   compile-time size assertion per push struct) and reject the padding.
2. **Per-draw device addresses for skinning inputs** (`joints_ptr`, `weights_ptr`
   in the draw record). If skinning becomes real, those belong in the *mesh*
   table, not in a per-instance record.
3. **Feature breadth**: bloom, DoF, toon, fog, and `FSOutput` writing toon data to
   a second target (`threedgame/shaders/gltf_uber.slang`). Not architectural, but
   the API must leave room for extra render targets and per-pass parameters
   without a rewrite. See 3.7.
4. **Non-indexed indirect draws with manual index reads** (fukuna) is a trap, not
   a trick: it doubles vertex-fetch traffic for any mesh whose indices repeat. Do
   not copy it.

### 1.2 What `mu_gfx` is behind on

- **Metadata is not the bottleneck.** At 4096 instances x ~6.2 k vertices x 16 B
  the vertex stream alone is ~408 MB/frame *(model: 4096 x 6219 x 16)*, ~24 GB/s at
  60 fps, against 0.53 MB of per-frame metadata. Index reads add ~177 MB/frame at
  uint16 *(model)*. Improving occlusion culling and adding LOD is worth 10-50x
  more than any instance-record compaction. Compaction is still worth doing
  (it removes CPU work) but it is not the headline.
- **No LOD, no clusters/meshlets, no multi-view.** Shadows, reflection probes and
  cube faces all need a second cull with different constants.
- **One 248-byte push layout serves cull, VS and FS**, so the VS pays for 96 bytes
  of frustum planes it cannot use and the cull kernel pays for material fields it
  cannot use.
- **Per-draw push and per-draw index-buffer bind** instead of one push per pass
  and zero per draw.
- **The GPU-driven path is not measurable.** No counter for frustum-culled vs
  Hi-Z-culled vs drawn, and no buffer readback in the backend (`vk.h` has
  `texture_readback` only). Culling effectiveness is currently unfalsifiable from
  inside the program.
- **Two stale sizeof comments** (0.1) prove prose in headers rots; the refined
  headers must assert sizes instead.
- **`SceneInstanceSource` is 52 bytes and read every frame** (4096 x 52 = 213 KB of
  CPU-side traffic plus a pointer chase into the asset) to produce 80 bytes per
  instance for the GPU.

---

## 2. Where the bytes actually go

Model for the cubepets scene: 4096 instances, 6 mesh slots, ~6.2 k vertices and
~21.6 k indices per model, 60 fps, everything submitted before culling.

| traffic | per frame | at 60 fps | note |
|---|---:|---:|---|
| vertex stream reads (VS) | ~408 MB | 24.5 GB/s | 4096 x 6219 x 16 B, shrinks with culling and LOD |
| index stream reads | ~177 MB | 10.6 GB/s | uint16, shrinks with culling and LOD |
| instance upload | 328 KB | 20 MB/s | 80 B x 4096, every frame |
| candidate upload | 197 KB | 12 MB/s | 8 B x 24576 |
| push + commands | ~2 KB | 0.1 MB/s | |
| **total** | **~585 MB** | **~35 GB/s** | order of magnitude, not a measurement |

Ranked conclusions, which is what the design follows:

1. **Vertex and index traffic dominate by three orders of magnitude.** The API
   must make LOD/cluster selection and occlusion culling first-class, not extras.
2. **The 328 KB/frame instance upload is not a bandwidth problem, it is a CPU
   problem.** The CPU has to *author* 4096 x 80 B records per frame: transform,
   bounds, candidate mirror, memcpy. Compaction buys CPU time and cache footprint,
   and it is the prerequisite for dirty-only uploads.
3. **Anything read once per candidate is cheap** (the cull kernel reads 327 KB of
   instances + 197 KB of candidates), so ALU-vs-bytes trades inside the cull
   kernel should always resolve toward fewer bytes.
4. **DOD refinement (2026-10-08): the 197 KB candidate upload is pure waste —
   delete it, don't compact it.** A candidate is `(instance, mesh)` and the mesh
   set per instance is bounded (Q12: ≤8, typically ≤4). The GPU derives
   `iid = tid / K`, `local_m = tid % K` from the dispatch id with two ALU ops
   (Q6). The CPU upload for candidates goes from 197 KB to 0 B, the CPU
   authoring loop over 24,576 candidates disappears, and the cull stays
   candidate-parallel. The §3.3 candidate table becomes a *derived index*,
   not a stored table — "positions have wheat" applied to draw membership.

---

## 3. Final design

### 3.1 Three lifetimes, three data sets

The whole design follows from splitting state by lifetime instead of by "which
pass reads it".

| lifetime | owner | contents | upload |
|---|---|---|---|
| **Asset** (immutable) | `SceneAssets` | mesh table, vertex streams, index streams, materials, local bounds, LOD/cluster tables | once, at load |
| **Scene** (persistent) | `SceneInstances` | instance records (quantized TRS + flags), slot allocator, dirty ranges | dirty slots only |
| **View/Frame** (transient) | `SceneFrame` | per-view constants, visible lists, indirect commands, counters, Hi-Z chain | never (GPU writes; CPU only zeroes counters) |

Consequences that the current code cannot express:

- An instance that does not move costs **zero** upload bytes per frame.
- The cull kernel reads *every* instance but only the compact form (§3.2), and the
  VS reads the same compact form for visible ones — one array, two readers, no
  duplicated bounds.
- Nothing that is per-view lives in a per-instance or per-asset struct, so a
  second view (shadow map) is another element of an array, not a new code path.

### 3.2 GPU-side layouts (byte budgets, exact)

**Instance — 20 bytes, replaces 80, split into hot/cold halves.**
Transform is quantized and rebuilt in the shader; bounds are derived,
not stored. **Decision made: quantized TRS (Q1/Q2,
2026-09-27)** — half3 position + half uniform scale + 4 spare bytes +
10-10-10-2 quaternion + 4 flag bytes.

DOD refinement (2026-10-08): field order is cull-read order, but the
hot/cold split is NOT worth a second stream — correction of the earlier
note: `mesh_set`, `lod_bias`, and `flags` are all cull inputs (set lookup,
LOD bias, skin/shadow participation), and `spare` holds the chunk id Q1
needs for position reconstruction. The only VS/FS-only byte is `tint`.
19 of 20 bytes are hot, so `CullRow[16] + ShadeRow[4]` would trade one
20-B sequential read for two dependent reads to save 1 cold byte per
candidate — a loss. Keep a single 20-B AoS array (Q4 stands, strengthened):
one array, two readers, dense gather by the VS. If `tint` ever dies, the
record becomes 19 B → pad to 20 B anyway, so the palette index is free.

```c
/* 20 bytes. 4-byte aligned (storage buffer rules); no 16-byte padding games.
   Field order is cull-read order: transform + chunk first, set/flags second,
   tint last (only VS/FS-only byte). No split — 19/20 bytes are cull-hot. */
typedef struct GpuInstance {
    uint16_t pos[3];    /* 6  half  chunk-relative position (Q1) — CULL HOT */
    uint16_t scale;     /* 2  half  uniform scale                — CULL HOT */
    uint32_t spare;     /* 4  chunk id (Q1), future use          — CULL HOT */
    uint32_t quat;      /* 4  10-10-10-2 normalized quaternion   — CULL HOT */
    uint8_t  mesh_set;  /* 1  mesh-set id, 256 sets (Q12)        — CULL HOT */
    uint8_t  flags;     /* 1  bit0 dynamic, bit1 skinned, bit2 shadow,    */
                        /*    bit3 two-sided, bits4-7 spare      — CULL HOT */
    uint8_t  lod_bias;  /* 1  signed bias added to the projected LOD — CULL HOT */
    uint8_t  tint;      /* 1  palette index                      — SHADE ONLY */
} GpuInstance;          /* 20 bytes.                                       */
/* Q3 resolved 2026-09-27: uniform per-instance scale. Source assets may
   carry any node transform (TRS or matrix); the importer canonicalizes
   them before they reach this record: non-uniform scale and shear are
   baked into static mesh vertices at load, negative scale is resolved
   in the loader (it flips winding/culling), and skinned meshes keep
   their skeleton/skin space coherent with the instance transform held
   separate. By the time the renderer sees it, every instance is
   translation + rotation + uniform scale. The loader never refuses a
   mesh for non-uniform scale; it bakes it. */
```

Budget: 20 B vs 80 B is a 4x reduction, and it removes three float4s of
structural padding, world bounds, and the 4th implicit row. Reconstruction cost in
the VS and in the cull kernel is ~12 multiplies (quat to basis) plus 3 row dots;
at 20 bytes per read against a bandwidth-bound kernel, ALU is the cheaper side
(2).

Why **not** store `rows[3]` in half precision (24 B, no reconstruction): it is
only 4 bytes more but it makes every consumer pay for 24 B, and it does not
answer LOD-Hi-Z (which needs the world radius anyway). Shear is not a reason to
keep rows either: the importer bakes shear (and non-uniform scale) into static
mesh vertices at load (Q3), so no per-instance representation needs it. Rows
stay the fallback if profiling shows the quaternion reconstruction is hot -
see Q2.

**Mesh — 24 + 16 bytes, indexed by mesh slot.** Replaces the per-draw push: the VS gets
the mesh slot from the visible entry (§3.4), so no per-draw constant is needed.

```c
/* Mesh — 24 + 16 bytes, split by reader (DOD 2026-10-08). Indexed by mesh
   slot. The VS gets the mesh slot from the visible entry (§3.4), so no
   per-draw constant is needed. The cull kernel needs only the culling
   half (24 B); the VS needs only the shading half (16 B). Two L1-resident
   tables, each read sequentially by its pass — the cull never pays for
   vertex/material addresses and the VS never pays for bounds/clusters. */
typedef struct GpuMeshCull {
    uint16_t first_cluster;  /* 2  cluster table entry (meshlet path, 3.5) */
    uint16_t cluster_count;  /* 2                                            */
    uint16_t flags;          /* 2  index width, skinned, double-sided, alpha */
    uint16_t lod_group;      /* 2  LOD ladder this mesh belongs to         */
    uint32_t local_center;   /* 4  half3 local bounding sphere centre      */
    uint32_t local_radius;   /* 4  half radius + half spare                */
    uint32_t index_count;    /* 4  this LOD's index count (or 0 if non-indexed) */
    uint32_t pad;            /* 4  keep 8-wide for vectorized cull loads   */
} GpuMeshCull;               /* 24 bytes, cull pass only                    */

typedef struct GpuMeshShade {
    uint64_t vertex_stream;  /* 8  device address into the pooled vertex stream */
    uint64_t index_stream;   /* 8  device address into the pooled index stream  */
    uint32_t pack;           /* 4  base_vertex:20 | material:12             */
    uint32_t pad;            /* 4                                            */
} GpuMeshShade;              /* 16 bytes, VS only, broadcast per draw       */
```

The world sphere the cull kernel needs is `center_w = pos + basis * (scale *
local_center)`, `radius_w = local_radius * scale` — exact under uniform
scale (Q3), no max() needed. That is two extra ALU ops per candidate and
**zero** bytes per instance, versus 16 B/instance stored today.

**Material — 12 bytes, replaces 32.**

```c
typedef struct GpuMaterial {
    uint32_t base_color; /* 4  UNORM8x4                                         */
    uint32_t texture;    /* 4  bindless albedo TextureID:16 | normal/orm:16      */
    uint32_t flags;      /* 4  sampler:2 | alpha_mode:2 | uv_set:1 | ...         */
} GpuMaterial;           /* array in a device buffer; the FS reads it by id      */
```

The sampler becomes a 2-bit field because the renderer only ever wants
linear-clamp or nearest-clamp for scene materials; arbitrary samplers stay
available through the bindless table for effects.

**Draw — 32 bytes (stride 32), anchored by §9.** The 20-B stride text below
is pre-`Count` history: §9.1 anchors `MeshDrawCommand` at 32 B (20 B
`VkDrawIndexedIndirectCommand` + 12 B designated extension field),
`_Static_assert`ed. The extension field is not pad — it carries the
per-command sort key the future batching pass (§9.9) needs without
re-reading `visible[]`:

```c
/* Written by the GPU, never by the CPU. Stride 32, _Static_asserted. */
typedef struct GpuDraw {
    uint32_t index_count;    /* GPU-written: LOD selection (3.5)      */
    uint32_t instance_count; /* GPU-written: the cull kernel          */
    uint32_t first_index;    /* static: from the mesh table LOD row   */
    uint32_t vertex_offset;  /* static: from the mesh table           */
    uint32_t first_instance; /* GPU-written: base into visible[]      */
    uint32_t sort_key;       /* GPU-written: (mesh_slot:16 | lod:8 | pri:8), §9.9 batching */
    uint32_t mesh_slot;      /* GPU-written: lets the VS skip the visible[] re-read for mesh data */
    uint32_t pad;            /* reserved: batch chain head (§9.9), else zero */
} GpuDraw;
```

`first_index`, `vertex_offset` are constants for the (mesh, LOD) pair and
are filled **once** at asset load, not per frame — except under §9.1
compaction, where the whole command (including the static fields) is
written by the GPU per surviving pair, because there is no static bucket
to pre-fill. Only `instance_count` and `first_instance` change per frame
in the bucketed shape; in the compacted shape every field is GPU-written
per survivor. Stride 32 wastes 12 B per command against stride 20, but
commands scale with *survivors*, not buckets (§9.4), and 32-B alignment
keeps every record vectorizable (Q8 resolved by §9).

**View constants — 96 bytes per view, read through one push address.** This is what
replaces the 248-byte push layout.

```c
typedef struct GpuView {
    float    rows[4][4];   /* 64  view-projection, dot-able rows (as today)      */
    uint64_t draws;        /* 8   device address of this view's GpuDraw[]        */
    uint64_t visible;      /* 8   device address of this view's visible[]        */
    uint32_t draw_count;   /* 4   buckets, CPU-known at load                     */
    uint32_t caps;         /* 4   visible capacity, Hi-Z level count, flags      */
    uint32_t viewport[2];  /* 8   for LOD projection and Hi-Z LOD selection      */
} GpuView;                 /* per-view, not per-instance, not per-draw           */
```

**Push — 32 bytes, once per pass, zero per draw (DOD 2026-10-08: was 24,
grew to the push-friendly 32-B boundary because the mesh split needs two
base addresses):**

```c
typedef struct ViewPush {
    uint64_t view;       /* GpuView* for the cull dispatch / this draw set */
    uint64_t mesh_cull;  /* GpuMeshCull[] base address (cull kernel)        */
    uint64_t mesh_shade; /* GpuMeshShade[] base address (VS)                */
    uint32_t sun;        /* packed half3 direction + half ambient           */
    uint32_t misc;       /* Hi-Z base:16 | levels:8 | flags:8               */
} ViewPush;              /* 32 bytes, _Static_asserted                      */
```

Everything else the shaders need (materials, hiz size, instance base, candidate
base) is reachable from `ViewPush.view` or is a `Load` on a small, L1-resident
table. The 96 bytes of frustum planes leave the push entirely: the cull kernel
derives them from `rows[]` once per thread with 6 `length()` calls (3.5) or reads
them from a per-view extended block.

### 3.3 The cull kernel (one kernel, N views)

One dispatch per view, `numthreads(64,1,1)`, one thread per candidate. The
candidate list itself changes shape:

- **Today**: the CPU expands each instance into one candidate per mesh
  (`SceneCandidate`, 8 B, 24576 entries for the demo) and uploads them.
- **Target**: the CPU uploads nothing. The kernel thread is a *candidate*,
  and the ids are derived from the dispatch id while mesh-set size stays
  bounded (Q6): `iid = tid / MAX_MESHES_PER_SET`,
  `local_m = tid % MAX_MESHES_PER_SET`. The mesh set id lives in the
  instance's `mesh_set` byte (Q12), so the loop bound is a table lookup:

```c
/* pseudocode of the final cull thread (Q6: derived ids, bounded sets) */
uint iid     = tid.x / MAX_MESHES_PER_SET;
uint local_m = tid.x % MAX_MESHES_PER_SET;
GpuInstance inst = instances[iid];                 /* one load, shared below */
uint set   = inst.mesh_set;                        /* Q12: full byte, 256 sets */
uint first = mesh_set_first[set], count = mesh_set_count[set];
if (local_m >= count) return;                      /* tail lanes exit */
uint m = first + local_m;
float3x3 basis = quat_to_basis(inst.quat);         /* ~12 mul */
GpuMesh mesh = meshes[m];                          /* L1: 40 B, ~64 entries */
float3 c; float r;
world_sphere(inst, basis, mesh, c, r);             /* derive, don't store */
if (sphere_outside_frustum(c, r, rows)) return;    /* mirrored-plane fast path (Q5), 6-plane fallback */
if (hiz_occluded(c, r, rows, viewport, hiz)) return; /* prev-frame Hi-Z default, 9.8 */ /* §0.1 of Hi-Z work */
uint slot;
InterlockedAdd(draws[draw_index(m, lod)].instance_count, 1, slot);
visible[draws[..].first_instance + slot] = iid | (m << 16);
```

Two wins over the current shape:

1. **No candidate array, no candidate upload, no CPU expansion.** Dispatch
   `instance_count * MAX_MESHES_PER_SET` threads (Q6); each thread derives
   its (instance, mesh) pair from its id, so candidate-level parallelism
   is kept while the CPU writes nothing. Tail lanes (`local_m >= count`)
   exit early. The per-instance transform is re-read per candidate thread
   rather than shared across a loop; at 4096 instances x 6 meshes that is
   24576 tests either way and total work is identical.
2. **Draw index and LOD come from the mesh row**, so the write target is
   computed on the GPU. (Pre-`Count` history: the CPU once knew the bucket
   count statically as mesh_count x lod_count; §9 replaces buckets with
   compacted commands and the `Count` draw.)

Frustum test: mirrored-plane fast path for normal perspective cameras, generic
6-plane fallback for shadow / off-center / orthographic views (Q5, §9.2).
The VS never reads either; the kernel derives the fast path from the
projection rows with no 96-byte plane block in the push.

### 3.4 The draw path: zero per-draw state (superseded by §9; pre-`Count` history)

> §9.1/§9.4 replace this section's one-draw-per-mesh-slot loop with one
> `vkCmdDrawIndexedIndirectCount` per view over GPU-compacted commands. The
> `visible[i] = instance | mesh << 16` packing and the draw loop below are the
> initial shape (§9.1/§9.9), not a contract; the entry format stays
> implementation-private (Q28).

The problem with "one push per pass" is that the VS no longer knows which mesh it
is drawing. Solution: **pack the mesh slot into the visible entry**.

```
visible[i] = instance_slot | (mesh_slot << 16)   /* 16 bits of instance, 16 of mesh */
```

- 16 bits of instance caps a view at 65535 *visible* instances, which is the same
  order as today's `SCENE3D_MAX_INSTANCES`; if that is not enough, move to 20/12 or
  make the visible entry 8 bytes (Q7).
- The VS reads `visible[first_instance + SV_InstanceID]`, splits both halves, and
  fetches `GpuMeshShade` (16 B, L1, and *uniform across the whole draw* so it is a
  broadcast) to get `vertex_stream`, `material`, `index_count`.
- The FS receives `mesh_slot` as `nointerpolation uint` and fetches
  `GpuMaterial` (12 B, also uniform) - exactly what `gltf_uber.slang` does with
  `material_id` today, so this pattern is proven on this codebase family.

The resulting draw loop is:

```c
/* once per pass */
begin_pass(vk, cmd, &(PassDesc){ .colors = &color, .color_count = 1, .depth = &depth,
                                 .pipeline = view_pipeline,
                                 .push = BYTE_SPAN(view_push),
                                 .reads = buffers_cull_output, .read_count = N });
/* once per view, not per draw */
cmd_bind_index_buffer(cmd, view->index_stream, VK_INDEX_TYPE_UINT16);
cmd_draw_indexed_indirect(cmd, draws_slice, view->draw_count, 20, /*push*/ NULL);
```

Losing the per-draw push also removes the per-draw `vkCmdBindIndexBuffer`: today it
is bound per mesh slot because each model owns a separate index buffer. Pooling all
index data into one arena with a device-address per mesh (as `GpuMeshShade.index_stream`
does) makes it one bind per *pass*, and the hardware reads indices from wherever
the draw's `first_index` lands. That is the same trick the vertex stream already
uses, applied to indices.

Net per-frame CPU recording cost for the scene pass: **one pass begin + one index
bind + one indirect draw**, against today's 6 x (bind + push + draw). The design
goal is that the number of CPU-side Vulkan calls per frame is a function of *passes
and views*, not of geometry.

### 3.5 LOD and clusters (where the real bandwidth is; static-bucket text superseded by §9.4-§9.5)

The Hi-Z work already computes `w_clip` and the projected NDC radius for every
candidate. That is the same quantity LOD selection needs:

```
screen_px = radius_w * proj_scale / w_near + lod_bias   /* proj_scale = |row.xyz| */
lod       = lod_ladder[screen_px]                        /* precomputed bucket table */
```

So LOD selection is **free** - it is arithmetic on data the occlusion test already
produced. What it needs from the API is a draw-bucket layout that lets the cull
kernel write one instance into one of several LOD buckets (pre-Count shape; 9 replaces buckets with per-pair LOD in compacted commands):

```
draw bucket index = mesh_slot * LOD_COUNT + lod
GpuDraw[view][mesh_slot * LOD_COUNT + lod]   /* all buckets exist, most stay empty */
```

- Bucket count is `mesh_count x LOD_COUNT`, known at load, so the CPU still issues a (pre-Count; superseded by 9.4 — static buckets are gone)
  single `draw_indexed_indirect` with a static count. Empty buckets cost one
  indirect draw record read by the hardware, which is ~nothing (20 B each).
- `index_count`, `first_index`, `vertex_offset` for each (mesh, LOD) are static and
  written once at load.
- The vertex stream must be LOD-aware: either one stream per LOD ladder (each
  `GpuMesh` row points into it) or one stream with LOD ranges. The pool already
  supports either.
- **This is the 4-8x knob.** At 16 B/vertex and 4096 instances, dropping distant
  instances to LOD2 (25% of the indices) removes ~280 MB/frame on its own.

Cluster/meshlet path, which composes with the above rather than replacing it:

- `GpuMeshCull.first_cluster` / `cluster_count` point into a cluster table (64-128 B per
  cluster: bounding sphere, cone axis + cutoff, index offset/count or a 64-bit mask).
- The instance-level cull stays as designed. A **second** pass per visible instance
  tests its clusters, writing `(instance | mesh << 16, cluster)` entries into the
  same draw bucket, so the draw path does not change shape at all - the visible
  array simply holds cluster-draws instead of instance-draws.
- Cone culling kills back-facing clusters before the vertex stage; on the cubepets
  crowd this is the difference between "24 GB/s of vertex fetch" and maybe a third
  of that.
- Decision needed on whether the *asset format* grows cluster tables now (Q10), since
  it changes the loader and the .glb pipeline, not just the renderer.

### 3.6 Sync model: buffers belong in the pass description

Today the cull-to-draw dependency is hand-written at the call site
(`scene3d.c`: two `cmd_buffer_barrier` calls naming `COMPUTE`,
`SHADER_STORAGE_WRITE`, `DRAW_INDIRECT`, `VERTEX_SHADER` and access masks). That is
exactly the knowledge the pass API exists to own, and the doctrine says call sites
name intent, never stage masks.

Proposed extension (the only `vk.h` change the renderer needs):

```c
/* Coarse per-buffer state. One state covers the whole Buffer; regions are not
   tracked because the pools are few and the barrier cost is a dependency, not a
   flush. If that proves wrong, this grows a (offset,size) range per state. */
typedef struct BufferAccess {
    BufferSlice           slice;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2        access;
} BufferAccess;

typedef struct PassDesc {
    const PassAttachment *colors;   uint32_t color_count;
    const PassAttachment *depth;
    RenderTarget *const  *shader_reads;  uint32_t shader_read_count;
    RenderTarget *const  *shader_writes; uint32_t shader_write_count;
    const BufferAccess   *buf_reads;     uint32_t buf_read_count;   /* NEW */
    const BufferAccess   *buf_writes;    uint32_t buf_write_count;  /* NEW */
    ByteSpan              push;                                     /* NEW */
    PipelineID            pipeline;
} PassDesc;
```

- `buf_writes` gets `COMPUTE | SHADER_STORAGE_WRITE` (or `TRANSFER`), `buf_reads`
  gets whatever the consumer needs (`DRAW_INDIRECT`, `VERTEX_SHADER`). `begin_pass`
  flushes the resulting barriers *before* the pass begins, which is exactly the
  point in the frame where they are needed.
- The compute-only path (`color_count == 0`) already exists in `begin_pass`; a cull
  dispatch becomes a pass with `buf_reads = {instances, meshes}`, `buf_writes =
  {draws, visible, counters}` and no attachments. No `vkCmdBeginRendering`, no
  `end_pass`, one flush.
- Keep the per-mip `ImageState` tracker and the timeline `DeleteQueue`; both are
  right. Add one rule that the current code violates implicitly: **frame-lifetime
  resources (Hi-Z chain, visible arrays, draw arrays) are created at init/resize and
  destroyed only at shutdown**, never per frame, so `wait_idle` never appears inside
  the frame loop (the Hi-Z resize path currently calls `wait_idle` mid-frame, which
  is correct but is a symptom of lazily-sized resources).
- Barrier count per frame becomes: 1 for uploads, 1 per cull pass, 1 per graphics
  pass, 1 for Hi-Z level chain. Today it is uploads + 2 hand-written + 7 Hi-Z level
  flushes.

### 3.7 Room for views, targets and effects

- `GpuView[]` is an array with a count, so a shadow cascade, a reflection pass or a
  cube face is one more entry. Each view owns its own draws/visible/Hi-Z state; the
  cull dispatch runs once per view with the same pipeline.
- `PassDesc.push` means an effect pass (bloom, DoF, toon, fog) carries its own
  parameter block without a second push layout, and `shader_writes` already models
  storage-image post chains (`SINGLE_COMPUTE_POSTPROCESS.md` in the upstream tree is
  the same idea, implemented there with a hand-written push struct per pass).
- MRT: `GraphicsPipelineConfig` already takes `color_attachment_count` and a format
  array; `threedgame`'s `FSOutput` (color + toon data) maps onto it with no API
  change.
- If TAA or motion vectors ever land, they come from a sidecar/history buffer,
  never from growing the instance record — §3.2's 20-byte budget stands.
  See Q11.

### 3.8 Upload model: dirty slots, not whole scenes

```c
/* 24 bytes: a slot index and its new record. The game owns the dirty list. */
typedef struct InstanceUpdate {
    uint32_t    slot;
    GpuInstance data;   /* 20 B */
} InstanceUpdate;

/* One staging copy for the whole dirty set; no per-slot submission. */
void scene_upload_instances(Scene *s, VkCommandBuffer cmd,
                            Span(const InstanceUpdate) updates);
```

DOD refinement (2026-10-08): the `(slot, record)` pair-of-struct is a CPU
authoring convenience, not a transfer layout. On the wire it becomes two
parallel spans — `uint32_t slots[N]` + `GpuInstance rows[N]` — so the copy
loop is two sequential `memcpy`s with no per-element stride skip, and the
GPU side can `vkCmdCopyBuffer` the rows contiguously after a single
gather, or scatter by slot with one indexed loop over a dense `slots[]`.
The public signature keeps `Span(const InstanceUpdate)` (game ergonomics);
the backend splits it at the staging boundary. Never a full-table diff:
the game appends to the dirty list at move time (Q21), the renderer never
scans for changes.

Rules:

- The instance array is a **persistent device buffer**, allocated once at
  `scene_create(capacity)`; it is never re-uploaded wholesale.
- The game writes only what moved. A spinning crowd where every pet rotates means
  4096 updates (98 KB) instead of 4096 x 80 B (328 KB) - same order, but the CPU
  cost per update drops from "transform + bounds + candidate mirror" to "quantize
  TRS".
- Static geometry uploads once at spawn: 0 bytes/frame.
- **Pose dedupe for skinning**: if skinning is wired up, `SceneSkinJob` should be
  deduplicated by `(model, clip, quantized_time)` so N characters sharing one
  animation frame share one palette. 4096 palettes x 60 joints x 64 B would be
  15 MB/frame; one unique pose is 3.8 KB. The dedupe table is a CPU hash; the GPU
  side just reads `palette_addr` from the instance's skin record. This is a bigger
  win than anything else in this document *if the demo ever becomes skinned* (Q13).
- Uploads go through the frame command buffer and the existing staging ring. There
  is no `vkQueueSubmit`/`vkQueueWaitIdle` anywhere in the frame loop - that is the
  single worst thing in `fukuna_engine`'s frame (`helpers.h:227`).

### 3.9 Instrumentation as part of the API, not an afterthought

The doctrine is measure -> identify -> change -> measure, and right now the
renderer cannot be measured from inside. The final API must include:

```c
typedef struct SceneCounters {
    uint32_t submitted;      /* instances in the array                  */
    uint32_t culled_frustum;
    uint32_t culled_hiz;
    uint32_t drawn;          /* instances actually in a draw            */
    uint32_t lod[4];         /* instances per LOD bucket                */
    uint32_t clusters_cone;  /* meshlet path, when it lands             */
    uint32_t draws;          /* non-empty buckets                       */
} SceneCounters;

/* GPU-written counters, read back with a fixed lag. */
bool scene_counters_read(Scene *s, SceneCounters *out);   /* true if a fresh sample is ready */
```

Backend piece needed: `bool buffer_readback(VkBackend*, VkCommandBuffer, BufferSlice dst_host,
BufferSlice src_device, VkDeviceSize size)` plus a small ring of host-visible buffers keyed on the
submission timeline value, so a read is available 2 frames later with no stall. The existing
`texture_readback` and the capture path already establish the pattern; the timeline semaphore already
exists (`vk.h`: `timeline`, `timeline_last_submitted`).

Also wanted: named GPU timer scopes around `cull`, `hiz`, `draw`, and one per effect, so the profiler
panel can attribute time. `GpuProfiler` and `GPU_SCOPE` already exist; the new calls simply need to be
placed, and the panel needs a "show by default" flag so a screenshot is evidence.

---

## 4. The refined public API

Complete signatures. Types not shown are unchanged from today.

### 4.1 `vk.h` delta

```c
typedef struct BufferAccess { BufferSlice slice; VkPipelineStageFlags2 stage; VkAccessFlags2 access; } BufferAccess;

/* PassDesc gains: push (bytes), buffer reads, buffer writes. Nothing else changes. */
typedef struct PassDesc {
    const PassAttachment *colors;         uint32_t color_count;
    const PassAttachment *depth;
    RenderTarget *const  *shader_reads;   uint32_t shader_read_count;
    RenderTarget *const  *shader_writes;  uint32_t shader_write_count;
    const BufferAccess   *buf_reads;      uint32_t buf_read_count;
    const BufferAccess   *buf_writes;     uint32_t buf_write_count;
    ByteSpan              push;           /* bytes, <= 256, pushed once at bind */
    PipelineID            pipeline;
} PassDesc;

/* Draw helpers lose their root data when the pass already pushed it. Passing an
   empty ByteSpan means "keep the pass's constants". */
void cmd_draw_indexed_indirect(VkBackend *r, VkCommandBuffer cmd, ByteSpan root,
                               BufferSlice indirect, uint32_t draw_count, uint32_t stride);
void cmd_bind_index_buffer(VkCommandBuffer cmd, VkDeviceAddress indices, VkIndexType width);

/* Host-visible readback ring with timeline retirement. */
typedef struct ReadbackSlot { Buffer host; uint64_t retire_value; bool pending; } ReadbackSlot;
bool vk_readback_begin(VkBackend *r, VkCommandBuffer cmd, BufferSlice device_src, VkDeviceSize size,
                       uint32_t *out_slot);
bool vk_readback_consume(VkBackend *r, uint32_t slot, ByteSpan out);   /* false until retired */
```

### 4.2 `scene.h` (replaces the `scene3d_*` surface)

```c
/* ---- lifetime ---- */
Scene *scene_create(VkBackend *vk, const SceneDesc *desc);   /* capacities, pool tag */
void   scene_destroy(Scene *scene);

/* ---- assets (immutable, GPU resident) ---- */
uint32_t   scene_mesh_set_add(Scene *s, const MeshSetDesc *desc);      /* device buffers + mesh table */
uint32_t   scene_material_add(Scene *s, const MaterialDesc *desc);
VkDeviceAddress scene_index_stream(const Scene *s);

/* ---- instances (persistent, dirty-upload) ---- */
bool scene_instance_reserve(Scene *s, uint32_t count, uint32_t *out_first);
bool scene_upload_instances(Scene *s, VkCommandBuffer cmd, Span(const InstanceUpdate) updates);
void scene_instance_free(Scene *s, uint32_t first, uint32_t count);

/* ---- frame ---- */
void scene_frame_begin(Scene *s, const SceneFrameDesc *frame);   /* capacity checks only */
void scene_view_set(Scene *s, uint32_t view, const SceneViewDesc *view_desc);
/* Cull is a pass: declares its own reads/writes, no hand-written barriers.
   GPU owns visibility, LOD, compaction, command generation, and draw count. */
void scene_cull(Scene *s, VkCommandBuffer cmd, uint32_t view);
/* Issues the view's single vkCmdDrawIndexedIndirectCount over the compacted
   commands. Caller owns the pass (it may add targets). */
void scene_draw(Scene *s, VkCommandBuffer cmd, uint32_t view);
void scene_counters(const Scene *s, SceneCounters *out);

/* ---- policy switches, each one mappable to a before/after measurement ---- */
void scene_set_occlusion(Scene *s, OcclusionMode mode);   /* OFF / HIZ_PREV_FRAME (default, §9.8); no two-phase pass */
void scene_disable_occlusion(Scene *s, uint32_t frames);  /* camera-cut hatch, Q29: ignore Hi-Z for N frames */
void scene_set_lod_bias(Scene *s, float bias);
void scene_set_lod_enabled(Scene *s, bool on);
```

Design notes on that surface:

- `scene_cull(cmd, view)` is a *pass*, so the barrier between cull output and the
  draw is the backend's job (3.6). The caller does not know it exists.
- `scene_draw` issues the view's `Count` draw (§9.1) and does not open a pass:
  the caller opened one (with any extra targets
  for toon/object-id/normals), so MRT costs nothing in the API.
- `Span(const InstanceUpdate)` rather than pointer + count, per doctrine.
- Every performance switch is a named mode with an `OFF` value, so the A/B
  measurement is one line in the game, not a recompile.

### 4.3 What the game has to give up

The game (`main.c`) keeps its `SceneInstanceSource` array as CPU truth. What changes:

- It no longer passes `Span(const SceneInstanceSource)` to the renderer every frame.
  It calls `scene_upload_instances` for what moved and nothing otherwise.
- It no longer receives `last_instances` / `last_candidates` / `last_draws` from the
  stage; it asks `scene_counters` and gets GPU truth at a known lag.
- It no longer owns `SceneCamera`'s clip rows: the camera produces a `SceneViewDesc`
  (position, orientation, fov, near, far) and the stage builds rows plus anything
  else the shaders want. Camera *integration* (yaw/pitch/speed/orbit) stays in the
  game because that is gameplay.

---

## 5. Before and after, in bytes (cubepets, 4096 instances, 6 meshes)

| item | today | target | delta |
|---|---:|---:|---|
| instance record | 80 B | 20 B (19/20 cull-hot, single AoS — split rejected 2026-10-08) | -75% |
| instance array per frame | 327,680 B uploaded + read | 81,920 B read, ~19,656 B uploaded (dirty 20%) | -94% upload |
| candidate record | 8 B x 24,576 = 196,608 B | 0 B (Q6: dispatch ids derive the pair) | -100% |
| mesh record | 96 B family (mat4+bounds+ids, fukuna) | 24 B cull + 16 B shade, split by reader (2026-10-08) | cull never pays for addresses |
| draw record | 32 B | 32 B (§9.1/§9.9; extension = sort_key + mesh_slot, not pad) | stride frozen, shape not |
| material | 32 B | 12 B | -62% |
| push per frame | 248 B x 7 = 1,736 B | 32 B x (1 cull + 1 draw) = 64 B | -96% |
| dirty upload wire format | — | `slots[N] + rows[N]` parallel spans, split at staging (§3.8) | AoS API, SoA wire |
| CPU Vulkan calls in the scene pass | 6 x (bind + push + draw) + 2 barriers | 1 bind + 1 `Count` draw + 1 pass barrier per view (§9.1) | |
| per-frame GPU sync points | 0 | 0 | (upstream fukuna: 1 blocking `vkQueueWaitIdle` every frame) |
| vertex traffic | ~408 MB/frame | LOD2 for half the instances: ~250 MB, plus cone/Hi-Z savings | the actual win |

---

## 6. Rejected alternatives

1. **`vkCmdDrawIndexedIndirectCount` / GPU-written draw count.** *Reversed by §9
   (2026-09-27):* with GPU-side command compaction + per-pair LOD, the bucket count
   is the surviving-pair count - which is visible only to the GPU. The `Count`
   dependency (`vk.c:316`) is accepted and the backend prototype
   (`cmd_draw_indexed_indirect_count`, `vk.c:2629`) is kept. The earlier reason
   ("CPU knows mesh_count x LOD") no longer applies because static buckets are gone.
2. **One instance per draw (`instanceCount = 1`), as in `threedgame`.** *Superseded
   by §9.1/§9.9 (2026-09-27):* one command per surviving pair is the initial
   command-generation shape, not a contract; it makes the
   per-draw data record 128 B and destroys batching; the only thing it buys is that
   the transform can live in the draw record. Batching survivors into one
   command per (mesh, LOD) with `instanceCount > 1` stays open (§9.9).
3. **CPU-built indirect command arrays, as in `fukuna_engine`.** 16 B per
   (instance, mesh) written by the CPU, plus the CPU knowing visibility, plus a
   blocking upload. This is the thing the design exists to delete.
4. **Non-indexed draws with manual index fetches.** Doubles vertex traffic.
5. **Padding every push struct to 256 bytes** (their `PUSH_CONSTANT` macro).
6. **Storing per-instance world bounds.** Derive them from the mesh's local bounds
   and the instance basis: 16 B/instance saved, at the cost of 2 ALU ops per
   candidate in a kernel that is memory-bound.
7. **Tracking buffer state per region.** Whole-buffer state is enough at this pool
   granularity; if it ever isn't, the change is additive.
8. **A separate "render graph" layer.** The pass API plus an explicit view array is
   enough. Upstream's `high-level-rendering-using-render-graphs` doc proposes more
   machinery than the current frame shape needs, and doctrine says no abstraction
   without a concrete need.

---

## 7. Questions I need answered before this is buildable

Grouped. My recommendation follows each; the point of listing them is that several
of these change the *layout*, and layouts are expensive to change later.

**Layout and encoding**

- **Q1.** **Resolved 2026-09-27: chunk-relative f16 position, 20-B record
  kept.** `pos[3]` is relative to a per-instance chunk origin; the
  chunk id travels in the record's spare word and the chunk table lives
  GPU-side. Raw world-space f16 (ulp 1.0 near 1024) was the thing that
  made large worlds fatal; chunk-relative keeps both precision and the
  compact layout.
- **Q2.** **Resolved 2026-09-27: quantized TRS, kept.** 4 B/instance cheaper
  than 3 f16 affine rows, and shear is not a per-instance concept — the
  importer bakes it into static mesh vertices at load (Q3).
- **Q3.** **Resolved 2026-09-27: uniform per-instance scale.** `scale[3]`
  becomes one half plus 4 spare bytes (`spare`, reserved for chunk id and
  the like). The rule is: the renderer only ever sees translation +
  rotation + uniform scale. The importer canonicalizes source node
  transforms (TRS or matrix) before they reach `GpuInstance`: non-uniform
  scale and shear are baked into static mesh vertices at load, negative
  scale is resolved in the loader (it flips winding/culling), and skinned
  meshes keep their skeleton/skin space coherent with the instance
  transform held separate. The loader never refuses a mesh for
  non-uniform scale; it bakes it. A precomputed per-instance world radius
  into the spare bytes is rejected: an instance fans out to 1..4 meshes
  with different local radii, so the world radius is per (instance, mesh),
  not per instance — it stays derived as `local_radius * scale` in the
  cull thread.
- **Q4.** **Resolved 2026-09-27: keep AoS 20 B.** Cull reads every field per
  instance and the VS reads the same record for visible ones — one array,
  two readers, no duplicated bounds. SoA only wins if something reads
  positions alone in bulk (streaming, shadow bounds), and we have no such
  pass. Revisit only if such a pass appears.
- **Q5.** **Resolved 2026-09-27: mirrored-plane frustum for normal
  perspective cameras, generic 6-plane path as fallback.** Main camera
  gets the fast path (§9.2); shadow / off-center / orthographic views
  keep the 6-plane test. The VS never needs either.
- **Q6.** **Resolved 2026-09-27: prefer derived dispatch ids while mesh-set
  size stays bounded.** With `MAX_MESHES_PER_SET` of 4 or 8:
  `iid = tid / MAX_MESHES_PER_SET`, `local_m = tid % MAX_MESHES_PER_SET`.
  This removes the CPU candidate upload while keeping candidate-level
  parallelism. Do not use it if mesh-set sizes become large or highly
  variable. Note the scale of the prize: the current candidate upload is
  only ~197 KB/frame, so the motivation is CPU work and GPU parallelism,
  not bandwidth. Orthogonal to §9's `Count` move: the compacted command
  needs the same (instance, mesh) enumeration either way.
- **Q7.** **Resolved 2026-09-27: keep `visible[]` private.** Pack
  `instance:16 | mesh:16` into u32 as the initial shape (0 extra bytes,
  caps a view at 65535 visible instances); a secondary indirection only
  if it ever bites. The 16/16 packing stays the initial shape only; the
  entry format is implementation-private (Q28/§9.9).
- **Q8.** Draw record stride 20 (37% fewer bytes, unaligned records) or 32
  (aligned, matches Niagara's `MeshDrawCommand`, 12 B become a designated extension
  field)? **Resolved by §9 (2026-09-27): 32**, asserted by `_Static_assert`.
  Static buckets are gone, so the waste scales with survivors, not buckets.
- **Q9.** **Resolved 2026-09-27: require `drawIndirectFirstInstance`.**
  The visible-indexing scheme uses `firstInstance` as a base into
  `visible[]`, so the Vulkan 1.1 feature stays a hard dependency.
- **Q10.** **Resolved 2026-09-27: put LOD/cluster fields in the asset layout
  now, allow empty.** `lodCount = 1` and `clusterCount = 0` until the mesh
  pipeline can emit ladders/clusters; the shader needs no `#ifdef` and
  LOD0 is always correct (§9.5).
- **Q11.** **Resolved 2026-09-27: do not grow the 20-B instance record for
  TAA.** If motion vectors are ever wanted, they come from a separate
  sidecar (previous-transform ring or a velocity output), not from
  widening `GpuInstance`. Q2's quantized-TRS answer stands unchanged.
- **Q12.** **Resolved 2026-09-27: no 2 extra bytes — reuse the spare byte
  as an 8-bit mesh-set id.** 256 sets instead of 16, record size
  unchanged. The flags byte keeps bit0 dynamic, bit1 skinned, bit2
  shadow, bit3 two-sided; the old bits4-7 mesh-set nibble moves to the
  `reserved` byte, which becomes `mesh_set` (see §3.2).

**Capacity and policy**

- **Q13.** **Resolved 2026-09-27: skinning stays an optional capability —
  allocate only when used.** Skinning will be used (barbarian-style
  rigid/bone-part characters count), so the design keeps the path, but
  skinning buffers are allocated only when a scene actually contains
  skinned meshes. Rigid bone-part characters do not need full vertex
  skinning; the pose-dedupe rule (§3.8) still applies when the path is
  wired.
- **Q14.** **Resolved 2026-09-27: runtime capacity at `scene_create()`,
  hard maximum, allocate once.** Never resize during gameplay; capacity
  is a creation argument capped at `SCENE3D_MAX_INSTANCES` (65536,
  1.3 MB at 20 B).
- **Q15.** **Resolved 2026-09-27: runtime `view_count` with a fixed
  `MAX_VIEWS` hard cap.** Do not bake the renderer around 1/4/6 views;
  `GpuView[]` is sized by the cap, filled per frame up to `view_count`.
  Each view owns its draws/visible/Hi-Z state (§3.7).
- **Q16.** **Resolved (2026-09-27): keep previous-frame Hi-Z.** Same-frame
  two-phase (draw occluders, build Hi-Z, cull the rest) is rejected: it costs a
  mid-frame barrier and a two-tier instance classification for removing an artifact
  class that the 1-frame lag bounds to camera cuts. The accepted artifact is one
  frame of pop-in on newly-visible geometry after a cut; a
  `scene_disable_occlusion(frames)` hatch (Q29) covers teleports.
- **Q17.** **Resolved 2026-09-27: Hi-Z is a renderer service, per-system
  buffers stay separate.** The pyramid builder is shared/generic (2D,
  particles, post can all consume it), but each system owns its buffers;
  no shared mutable Hi-Z state.
- **Q18.** **Resolved 2026-09-27: keep GPU counters, debug/profiling only,
  never a sync point.** The delayed-readback ring (2-frame lag, §3.9)
  already heads the right way: counters retire on the timeline and must
  never stall the render path.

**API shape and code organisation**

- **Q19.** **Resolved 2026-09-27: split into `scene_assets.c`,
  `scene_instances.c`, `scene_pass.c`** (replacing `src/three_d/scene3d_*`)
  plus `shaders/cull.slang`, `shaders/scene.slang`, `shaders/hiz.slang`.
- **Q20.** **Resolved 2026-09-27: keep 2D and 3D separate implementations;
  share conventions, not buffers.** Same barrier model and visible-array
  convention, separate code — keeps 2D out of a 3D app's memory, which is
  the property `GameHooks.two_d` exists to guarantee.
- **Q21.** **Resolved 2026-09-27: game owns explicit dirty lists.** Keep
  `SceneInstanceSource` (52 B, CPU) as the game's truth, but never diff
  all instances per frame — the game knows what moved and uploads only
  that (§3.8).
- **Q22.** **Resolved 2026-09-27: static maximum draw capacity, GPU-written
  actual count.** The "no `Count` variant" rule was dropped by §9:
  `vkCmdDrawIndexedIndirectCount` is the draw call with a static
  `MAX_CANDIDATES` cap plus dropped-counter (Q26).
- **Q23.** **Resolved 2026-09-27: GPU tables are authoritative.** Device
  buffers written by the loader; the CPU may keep lightweight asset
  metadata for editor/debug tooling but never a second mutable rendering
  state.
- **Q24.** **Resolved 2026-09-27: `scene_create` can fail (invalid handle),
  uploads return `bool`, internal invariant violations assert.**
  `scene_upload_instances` is the one function that can legitimately fail
  on external data.
- **Q25.** **Resolved 2026-09-27: absolutely no `wait_idle` in the frame
  loop.** Frame-lifetime resources are sized once at `scene_create` and
  recreated only through an explicit resource-lifetime path outside the
  frame (resize/reallocation).

---

## 8. Summary

- Both upstream snapshots are CPU-driven for the scene: fukuna builds indirect
  commands on the CPU and stalls the queue every frame to upload them; threedgame
  issues one draw per mesh with `instanceCount = 1` and 128 bytes of per-mesh draw
  data re-uploaded every frame, with a CPU-written draw count. Neither has a cull
  shader. Our port is already past both on the axis that defines a GPU-driven
  renderer.
- The refined design keeps the two things our port got right (GPU-owned
  visibility, one indirect draw per view under 9 (per bucket in the pre-Count 3 text), the pass API and image tracker) and fixes
  the three it did not finish: the 248-byte push that mixes frame and per-draw data,
  the 80-byte instance record with stored bounds, and per-draw Vulkan calls.
- Target record sizes: instance 20 B, mesh 40 B, material 12 B, draw 32 B stride
  (§9.1/§9.9; initial `instanceCount = 1`, batchable later), push 24 B
  once per pass. Visible entries pack `instance:16 | mesh:16` initially (§9.9;
  format stays implementation-private, Q28) so the VS needs no
  per-draw constant.
- The real performance work is LOD plus occlusion (previous-frame Hi-Z, 9.8) plus clusters, because vertex and
  index traffic is ~1000x the metadata traffic. Everything else here is CPU time and
  cleanliness.
- **All of Q1-Q29 are answered (2026-09-27, `gpuans.txt`)**; the layout
  decisions (Q1 chunk-relative f16, Q2 quantized TRS, Q3 uniform scale,
  Q4 AoS, Q12 8-bit mesh-set) and the frame-contract decisions
  (Q16 previous-frame Hi-Z, Q22 static cap + `Count`, Q26 clamp +
  dropped-counter, Q27/Q28 private compaction and `visible[]`) are locked
  in §7. The design is implementable without further decisions.

---

## 9. GPU-driven `Count` (Niagara-inspired) - supersedes 3.4/3.5, §6 item 1, Q6/Q8

**Decision:** move to `vkCmdDrawIndexedIndirectCount`. The GPU owns visibility,
LOD, compaction, command generation (including the `instanceCount` /
`firstInstance` fields), and the draw count; the CPU issues **one** draw
command per view; LOD + occlusion + compaction are first-class - no task/mesh
shaders, no two-pass occlusion, no hardware mesh shading.

Taken from `niagara/` (zeux's streamed Vulkan renderer, vendored in-tree):

| Niagara mechanism | File | Adopt? |
|---|---|---|
| `MeshDraw` per-draw record (TRS + mesh/material refs) | `src/shaders/mesh.h:92` | shape only; ours is the 20 B quantized TRS |
| `MeshDrawCommand { drawId, VkDrawIndexedIndirectCommand }` at stride 32, written by `atomicAdd(commandCount,1)` | `mesh.h:104`, `drawcull.comp.glsl:143` | **yes, exactly** - GPU-compacted dense commands |
| `MeshTaskCommand` + indirect task dispatch | `mesh.h:116`, `tasksubmit.comp.glsl` | **no** - exists only to feed mesh shaders we will not compile |
| Two-pass (early/late) occlusion | `niagara.cpp:1771-1784` | **no** - we cull once against previous-frame Hi-Z |
| `drawVisibility[di]` ping-pong array | `drawcull.comp.glsl:154-155` | **no** - two-pass bookkeeping only |
| `projectSphere` + `getOcclusionMip` + mirrored-plane frustum | `shaders/math.h:2-39` | **yes**, the mip logic and the frustum form |
| Center-of-AABB single sample at chosen mip, linear fetch with min-reduction sampler | `drawcull.comp.glsl:91-94` | **no** - our 2x2 `Load` is strictly more conservative and sampler-free |
| Per-draw LOD error ladder (`lods[8].error`) | `mesh.h:53-60`, `drawcull:106-116` | **yes**, the metric, with our own ladder (9.5) |
| Cluster cone culling, int8 cone axis + cutoff | `mesh.h:11-24`, `clustercull:75-104` | **later** - needs clusterized assets + meshlet table |

### 9.1 New frame contract

One dispatch per view. The cull kernel **compacts** instead of filling buckets.
GPU owns visibility, LOD, compaction, command generation, and draw count; the
CPU issues one `vkCmdDrawIndexedIndirectCount` per view. Initial implementation
shape (not a contract):

```c
/* per-view: one command counter + one visible-slot counter, shared by the view */
uint dci = atomicAdd(commandCount, 1);                 /* cf. Niagara drawcull:143, initial strategy only, see Q27 */
drawCommands[dci].drawId        = candidate.draw_id;   /* mesh (+LOD) identity */
drawCommands[dci].indexCount    = lod.indexCount;      /* LOD-selected */
drawCommands[dci].instanceCount = 1;                   /* initial shape only, see 9.9 */
drawCommands[dci].firstIndex    = lod.indexOffset;
drawCommands[dci].vertexOffset  = mesh.vertexOffset;
drawCommands[dci].firstInstance = base;                /* base into visible[], see 9.9 */
visible[base]                   = iid | (mesh_slot << 16); /* initial packing only, see 9.9 */
```

The initial build emits one command per surviving (instance, mesh) pair.
**Anchor decision (2026-09-27):** `MeshDrawCommand` is 32 B (20 B
`VkDrawIndexedIndirectCommand` + 12 B designated extension field) and the
draw's push root is **empty** - identity comes only from `visible[]` plus the
view constants. Draw count is `commandCount`, GPU-written, consumed by
`vkCmdDrawIndexedIndirectCount(cmd, commands, 0, count, 0, MAX_CMDS, 32)`.
Backend support already exists verbatim: `cmd_draw_indexed_indirect_count`
(`vk.c:2629`); the `multi_draw_indirect_count` feature is `TRY_ENABLE`d
(`vk.c:316`).

What is frozen here is the frame contract (GPU compacts, GPU counts, one
`Count` draw per view) - not the command shape. Command generation is designed
so a later pass can batch surviving instances into one command per (mesh, LOD)
with `instanceCount > 1` and a `firstInstance` base into a contiguous
`visible[]` range, with no public-API or VS-identity change. Likewise the
`visible[]` entry format and the per-survivor `atomicAdd` above are initial
choices only; both stay implementation-private (Q27/Q28, §9.9).

`max_draw_count` is a static cap (`MAX_CANDIDATES`), so the CPU needs no
visibility knowledge. All per-mesh draws collapse into **one draw call per view**.
Cost model of the initial strategy: one global `atomicAdd` per surviving pair,
same as Niagara's `drawcull.comp.glsl:143` - contention is bounded by survivors,
not submissions.

### 9.2 Steal the mirrored-plane frustum test

Niagara tests two frustum sides at once from camera space, using only the four
symmetric planes plus explicit near/far (`drawcull.comp.glsl:79-82`):

```c
visible = visible && center.z * frustum[1] - abs(center.x) * frustum[0] > -radius;
visible = visible && center.z * frustum[3] - abs(center.y) * frustum[2] > -radius;
visible = visible && center.z + radius > znear && center.z - radius < zfar;
```

`frustum[]` is filled once per frame from the normalized projection row sums
(`niagara.cpp:1502-1514`: `(row3+row0)`, `(row3+row1)`). This is tighter than our
six-plane `Frustum`, needs 4 dot-equivalents instead of 6, no per-plane
normalization, and no 96 bytes of push data. The per-view push shrinks by exactly
the frustum block. Adopt verbatim; keep our `frustum_extract` only for the C-side
unit test.

### 9.3 Keep the 2x2 `Load`, but with their comment

Niagara samples the pyramid **once**, at the AABB center, with a linear-filtered
`textureLod` on a sampler whose reduction mode is min (`drawcull:93-94`):
*"Sampler is set up to do min reduction, so this computes the minimum depth of a
2x2 texel quad."* That needs a custom border-aware sampler with
`VK_SAMPLER_REDUCTION_MODE_MIN`, and linear filtering on depth textures is
implementation-defined on some drivers - Niagara accepts both, and its
`depthSampler` has to be wired per pyramid binding.

Our `cs_main` fetches the 2x2 explicitly with `Load` and takes the min, then
requires `m >= depth_max`. Strictly more conservative, sampler-free, no
reduction-mode sampler. **Keep it.** The one steal from Niagara is its mip-selection
correction: after `level = ceil(log2(span))`, test whether the rect straddles at
most 2 texels at `level-1` and drop down (`math.h:35-36`) - a one-level-finer fetch
is free and cuts false negatives roughly 2x. Our current `hi - lo <= 1 … else
skip` covers the correctness half; the correction covers the precision half.

### 9.4 What this deletes or supersedes from Section 3

Superseded by §9, not deleted silently — the following §3 text now reads as
pre-`Count` history:

- 3.3's "one `instanceCount` per mesh bucket" write target. Buckets are gone;
  commands are compacted.
- 3.4's "one draw per mesh slot" vocabulary and its `visible[i] =
  instance_slot | (mesh_slot << 16)` packing, its per-draw VS mesh-fetch
  walkthrough, and its `cmd_draw_indexed_indirect(cmd, draws_slice,
  view->draw_count, 20, ...)` draw loop (3.4, lines ~397-439). The `Count`
  frame contract is §9.1: one `vkCmdDrawIndexedIndirectCount` per view over
  GPU-compacted commands; the initial `visible[]` packing is §9.1/§9.9, and
  the format itself stays implementation-private (Q28).
- 3.4's per-draw LOD-bucket layout (`mesh_slot * LOD_COUNT + lod`, 3.5) is
  replaced by per-pair LOD selection written into the compacted command
  (§9.1, §9.5); the (mesh, LOD) batching of `instanceCount > 1` survivors is
  an allowed future inside command generation (§9.9), not a bucket revival.
- 3.5's "CPU still knows the bucket count statically, which keeps
  `draw_indexed_indirect` viable and keeps the «no Count variant» rule
  intact" no longer holds: static buckets are gone and the `Count` variant
  is the draw call (§9.1).
- 6.1, in full. Count *is* GPU-written; the `Count` feature dependency is accepted
  and already requested in `vk.c:316`.
- Q8 (stride): **anchored 2026-09-27 at 32**, matching `MeshDrawCommand` plus a designated
  extension field, asserted by `_Static_assert`.
- The `first_instance`-is-visible-base convention stays: each compacted command
  carries a `firstInstance` base into `visible[]` plus its `instanceCount`.
  Initially one command per surviving pair (`instanceCount = 1`, so `base`
  is the command's own output index and no slot allocator is needed); a future
  pass may batch survivors per (mesh, LOD) with `instanceCount > 1` over a
  contiguous `visible[]` range (§9.9) — at that point the visible-slot allocator
  comes back, without touching the public API or the VS identity path.

### 9.5 LOD policy (with Q10's data, without Q10's dependency)

Niagara's metric: `threshold = distance * lodTarget / scale`, walk the ladder
while `mesh.lods[i].error < threshold` (`drawcull:110-115`). Ours:

```c
float  dist   = length(center) - radius;            /* view space, > 0 */
float  metric = dist * lodTarget / inst.scale;   /* uniform scale, Q3 */
uint   lod    = 0;
while (lod + 1 < mesh.lodCount && mesh.lods[lod + 1].error < metric) ++lod;
```

- `lodTarget` is a per-view float (same units Niagara uses: "lod target error at
  z=1"), exposed via `scene_set_lod_target` - name the units in the header.
- `mesh.lods[]` is what the loader fills; until then `lodCount = 1` and the walk
  is a no-op - the shader needs no `#ifdef`, and LOD0 is always correct.
- The command's `indexCount`/`firstIndex` for the chosen LOD are the only write
  targets; there is no `(mesh x LOD)` static slot explosion.
- A pixel-size floor (`projected_diameter < 2px` => deepest LOD or cull) is a
  one-line addition later; Niagara does not do it, and we flag it as a future
  measurement before adding a branch.

### 9.6 New questions this raises

- **Q26.** **Resolved 2026-09-27: clamp at capacity, count
  `dropped_commands`, never silently overflow.** `commandCount` clamps at
  `MAX_CANDIDATES` (Niagara: *"drop draw calls on overflow; this limits us
  to ~4M"*, `drawcull:128-129`); the dropped count stays visible so
  overflows are never silent.
- **Q27.** **Resolved 2026-09-27: start with global-atomic compaction;
  keep the shader structured so subgroup/workgroup prefix can replace it
  without an API change.** Per-survivor `atomicAdd(commandCount)` first
  for simplicity (Niagara-fast); the prefix form is an
  implementation-private optimization triggered by profiling. Do not
  hard-code subgroup-size assumptions (no "group of 64" baked into the
  shader); size the prefix math off the actual workgroup/subgroup size.
  The public contract is only: survivors become a dense command array,
  `commandCount` is GPU-written. The VS never sees the strategy
  (§9.7 item 1 is the pre-approved form of that switch).
- **Q28.** **Resolved 2026-09-27: `visible[]` representation stays
  implementation-private.** The public contract is only: each command's
  `firstInstance` is a base into `visible[]`, and the VS resolves
  (instance, mesh/LOD) from the entry. Neither `instanceCount = 1` nor the
  16/16 packing is frozen: command generation may later batch survivors into one
  command per (mesh, LOD) with `instanceCount > 1` and a `visible[]` base, with
  no VS or public-API change.
- **Q29.** **Resolved 2026-09-27, renamed: `scene_disable_occlusion(frames)`.**
  With previous-frame Hi-Z locked in (Q16), newly-visible geometry after a
  camera cut pops in one frame late. A camera cut is an occlusion-history
  problem, not an LOD problem — hence the rename from
  `scene_force_full_detail`: it disables occlusion for N frames after a
  teleport. One function, zero cost when unused — see the locked
  invariants in §9.8. Include it in the `scene.h` surface.

### 9.7 Suggested further optimizations (beyond Niagara, still no mesh shaders)

1. **Subgroup ballot compaction instead of `InterlockedAdd` per pair.** Inside a
   workgroup, `ballot(visible)` + a subgroup prefix gives each surviving thread
   its group-local offset with one `atomicAdd(commandCount, group_total)` per
   group. Fewer atomics in dense views for ~6 instructions of prefix math; size
   the prefix off the actual workgroup/subgroup size, never a hard-coded 64
   (Q27). Do this only if Q27's measurement says the global atomic is hot.
2. **64-bit packed candidate keys.** Pack `(depth_metric:32 | draw_id:32)` per
   survivor and radix-sort before writing commands: front-to-back order cuts
   overdraw ~2x in dense crowds and improves Hi-Z hit rate on *this* frame's pass.
   Cost: one counting-sort pass over a dense array (already proven in the 2D
   sprite path's batch sort). Biggest pure-win after LOD; requires a scratch
   buffer of `MAX_CANDIDATES` u64.
3. **Cluster-level backface cone test in the cull thread** (Chevy's cone math is
   in `niagara/src/shaders/math.h:41`, the int8 cone data in `mesh.h:15-16`).
   Our per-mesh cull can reject a whole draw for back-facing single-cluster
   meshes (small props, heads) for free once Q10 feeds the cone table - no
   meshlet dispatch needed, one dot product per candidate.
4. **Depth pre-pass toggle for the nearest 5%.** A `COUNT`-driven pre-pass that
   draws only the front-most surviving pairs (sorted by `depth_metric` from (2))
   builds a same-frame occluder set without Niagara's two-pass machinery: occluders
   render with color writes off, then the main `Count` draw consumes fresh depth.
   Strictly better than previous-frame Hi-Z for static geometry; keep
   previous-frame as the default and the pre-pass as a per-view flag
   (`OcclusionMode`, 4.2) because it doubles vertex cost for the occluders.
5. **Persistent mapped instance buffer + GPU-side dirty merge.** Instead of the CPU
   uploading `InstanceUpdate[]` (3.8), keep instances in one persistently-mapped
   buffer and let the cull kernel read a small GPU-side "generation counter" array:
   instances whose generation != frame generation are skipped without CPU upload.
   Removes the upload path entirely for static scenes; costs one u32 read per
   instance per frame. Only matters if update traffic, not read traffic, dominates
   - measure via `scene_counters`.
6. **Shared `CullData` push across cull + Hi-Z build + debug passes.** Niagara
   pushes one `Globals`/`CullData` block (`mesh.h:26-51`) to every compute entry;
   our three entries (`cs_main`, `cs_hiz`, cull-debug) should share an identical
   128 B prefix so one push call serves the whole chain. Concretely: `rows[4]` is
   wrong as push content - push the *view matrix* (64 B) + 4 scalars (frustum,
   P00/P11, znear, lodTarget) + pyramid size, and let shaders derive symmetric
   planes; that is 96 B total and drops the 96 B `Frustum` from every push.

### 9.8 Hi-Z invariants (locked 2026-09-27)

Two rules, both free, that keep previous-frame Hi-Z (Q16) correct without a
same-frame accumulation pass:

1. **Transparent passes run with depth-write off whenever Hi-Z is on.**
   Non-monotonic depth (a later pass writing depth our pyramid read earlier) only
   exists if a transparent pass writes depth *after* the Hi-Z build. The rule
   removes the case entirely: zero GPU cost, zero branches, enforced as a
   render-state invariant. Any effect that genuinely needs depth-writing
   transparents opts out of Hi-Z per pass via the `OcclusionMode` switch (§4.2)
   instead of paying for a handling path in the cull.
2. **`scene_disable_occlusion(frames)` covers camera cuts (Q29).** Newly-visible
   geometry after a teleport pops in at most one frame late under previous-frame
   Hi-Z; the hatch disables culling for N frames after a teleport. Zero cost when
   unused, one function in the `scene.h` surface.

A same-frame scratch-pyramid update was considered and rejected: it cannot feed
the cull that already ran, so it needs a second full cull (or an occluder
pre-pass — the two-phase Q16 rejected), roughly 2.5–3x the cull stage every
frame, to fix a case that occurs on ~0% of normal frames.

### 9.9 Command generation stays behind the API (locked 2026-09-27)

The public contract is: GPU owns visibility, LOD, compaction, command
generation, and draw count; `scene_cull()` computes, `vkCmdDrawIndexedIndirectCount`
consumes. Everything below it is an implementation choice, and the following are
explicitly **not** frozen:

- **`instanceCount = 1` per command is today's shape, not a contract.** Command
  generation may later batch surviving instances into one command per
  (mesh, LOD) with `instanceCount > 1` and a `firstInstance` base into
  `visible[]`. The VS already resolves identity through `visible[]`, so batching
  changes atomics and slot assignment only.
- **`visible[]` entry format is private.** Today `instance:16 | mesh:16`; tomorrow
  a `(mesh, LOD)` key or a resolved address — the VS path reads whatever the
  cull kernel writes, through the same `firstInstance` base. No public type
  names the entry.
- **The compaction strategy is private.** Per-survivor `atomicAdd(commandCount)`
  today; subgroup/workgroup prefix (Q27) later without touching the VS, the
  public headers, or the frame contract.

The one invariant that must hold across all three: **the draw-count buffer, the
command array, and `visible[]` are a consistent triple after `scene_cull()`
returns** — `count` commands, each pointing at its own `visible[]` range.
Sub-question Q26 (overflow policy) therefore stays public: clamping at
`MAX_CANDIDATES` with a dropped counter, until a scene proves otherwise.

---

## 10. DOD conformance appendix (refinement 2026-10-08)

This section maps the design above onto the repo doctrine
(`AGENT.md` / `docs/mycstyle.md` / skills `dod-performance`,
`game-engine-dod`). It changes no layout decision in §1–§9; it names
the DOD pattern each decision already follows and lists the remaining
violations to fix in code.

### 10.1 Existence is state — what the frame already does right

| DOD pattern | Where it holds |
|---|---|
| Visibility = membership, not a flag | cull writes `visible[]` + counts; the CPU never scans `bool visible[N]` |
| Dirty = membership | §3.8: dirty ranges / game-owned dirty lists, never a full diff |
| Optional facts = side tables | bounds live in §2's `InstanceBounds[]`, materials in `GpuMaterial[]`, never nullable fields on the instance |
| LOD = collective representation | §9.5 ladder + §9.9 batching: N identical survivors → one command + count |
| State by table, not `switch` | opaque / skinned / shadow are separate batches and pipelines, never `switch (type)` in the draw loop |
| Preparation vs execution | `scene_prepare` (validate, compact, upload) vs cull/draw (operate on valid data, no branches) |

`docs/impl.txt` already sketches the same rule for the 2D path
(bitsets → AND → sparse survivor list → command stream). The 3D target
above is that sketch with GPU-side compaction.

### 10.2 Layout matches the access pattern (skill `dod-performance` step 2)

- Instance: single 20-B AoS, field order = cull-read order. The 2026-10-08
  split analysis *rejected* `CullRow + ShadeRow`: 19/20 bytes are cull-hot
  (`mesh_set`, `flags`, `lod_bias` are cull inputs, not shade-only), so a
  split trades one sequential read for two dependent reads to save 1 byte.
  Q4 stands, strengthened. The VS gathers dense survivors by slot.
- Mesh: **split** 24 B `GpuMeshCull` + 16 B `GpuMeshShade` by reader
  (2026-10-08). Unlike the instance, the split is clean: the cull never
  touches device addresses/materials, the VS never touches
  bounds/clusters. Both tables stay L1-resident; the VS mesh fetch is a
  broadcast (uniform across the draw).
- Candidates: deleted table, derived index (`tid / K`, `tid % K`).
  §2 conclusion 4 + Q6: 197 KB upload → 0 B, CPU authoring loop gone.
- Draw: 32 B stride anchored (§9); the 12-B extension is a sort key +
  mesh slot for §9.9 batching, not pad. Scales with survivors, not buckets.
- Dirty upload: AoS at the API (`InstanceUpdate`), SoA on the wire
  (`slots[N] + rows[N]`), split at the staging boundary (§3.8).
- Push: 24 → 32 B (mesh split needs two base addresses); still once per
  pass, zero per draw.
- Static data (`first_index`, `vertex_offset`, `index_count`) is written
  once at load, never per frame. Per-frame GPU writes are exactly
  `instanceCount + firstInstance` (+ LOD fields later).
- Vertex traffic dominates metadata ~1000:1 (§8); LOD + Hi-Z + clusters
  are the performance work, everything else is CPU cleanliness.

### 10.3 What still violates doctrine (fix in code, not in this doc)

1. **Doc drift without `_Static_assert`.** `SceneGpuMaterial` (112→32)
   and `SceneSkinJob` (56→40) comments lied. Rule: every GPU-shared
   size claim gets `_Static_assert(sizeof(...) == N)` at the definition
   site; prose sizes are forbidden.
2. **Unwired skinning surface.** `skin_job_count == 0` always,
   no skinning pipeline in `scene3d_init`, `SCENE_ASSET_SKINNED` never
   consumed. Either wire §3.7 or delete the surface — designed-but-dead
   code is a branch in the reader's head.
3. **Frame CPU work is still O(instances + candidates).**
   `scene3d_render` compacts + uploads full tables every frame; §3.8
   dirty ranges are designed, not implemented. Implement or re-budget.
4. **Per-mesh-slot push + bind + draw loop.** One `Count` draw per view
   (§9.1) deletes it; until then every slot pays 248 B push + barrier.
5. **`SceneInstanceSource` is 52 B of game truth with holes.**
   Split hot (moved this frame) from cold at the game boundary, or keep
   the explicit dirty list (Q21) honest — never a full-table diff.
6. **No frame/system timers on the scene path.** `MU_SCOPE_TIMER` /
   `GpuProfiler` per stage (prepare, upload, cull, record, submit);
   no optimization without a number.

### 10.4 Review checklist (must hold before calling §9 done)

```text
[ ] passes named: prepare / cull / record / submit, each with input table,
    output table, frequency written down
[ ] layout justified per pass (instance AoS kept per Q4+2026-10-08 split
    analysis, mesh split by reader, candidates derived, dirty SoA-on-wire)
[ ] hot rows hold no pointers; device addresses only in GPU tables
[ ] visibility/dirty/LOD are membership; no nullable members, no flag scans
[ ] no enum-as-flow dispatch in cull / VS / draw loop
[ ] one allocator strategy per region; no hot malloc; frame-slot reuse by
    completion, not by index coincidence (see phase-2-plan §B)
[ ] `scene_prepare` validates once; cull/VS assume valid data
[ ] before/after byte + ms numbers for every §5-row change; revert if no gain
[ ] compiles warning-clean; shared C/Slang layouts asserted, not described
```
