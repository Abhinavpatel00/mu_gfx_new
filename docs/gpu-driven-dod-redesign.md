# GPU-driven 3D renderer: fully data-oriented design

Status: **normative spec.** This document is self-contained. It replaces the
earlier `gpu-driven-api-design.md` and does not depend on the removed
`scene3d` / `hiz` implementation; those are history, not a base.

Doctrine: `AGENT.md` §1, skills `dod-performance` and `game-engine-dod`.
Program = data + transformation. Layout follows the loop. State is existence
(table membership, not flags). IDs over pointers. Preparation is separate from
execution. Measure everything claimed. **No mesh shaders, no task shaders, no
`VK_EXT_mesh_shader`:** everything below is vertex pull + indexed
`vkCmdDrawIndexedIndirectCount`.

The one governing rule (AGENT.md §1.3):

> Flags that introduce `switch`es/`if`s in a hot loop are a layout bug. Store
> *eligibility* as table membership and run one specialized transform per
> table. A per-row field used only as a *load index* is not a flag.

Every shader loop below is written so that no byte it reads is ever tested by
`if`/`switch`/`?:` on a data-dependent condition. What remains in a row is
exactly what its transform loads. Membership is answered by *which table the row
lives in*, never by a field.

---

## 0. The model in one picture

```
game owns CPU truth (instance transforms, dirty events)
        │
        ▼
 T1  upload dirty instances ───────────────► gpu_inst[]      (20 B/row, global)
        │
        ▼   per (render_class, view)
 T2  cull   candidates ──┐
                         ├─ frustum + Hi-Z (prev frame) + LOD (arithmetic)
                         └─► survivors[] + survivor_count   (one atomic/survivor)
        │
        ▼
 T3  compact survivors ─────────────────────► vis_all[] + draw_all[] + draw_count
        │
        ▼
 T5  vkCmdDrawIndexedIndirectCount ─────────► framebuffer
        │
        ▼
 T4  Hi-Z build from this frame's depth ────► next frame's T2

 T6  counters readback (GPU truth, lag 2, never a stall)
```

A **render class** is a pipeline variant (fixed vertex stage, raster state,
blend state, index width). A **view** is a camera (VP + viewport + Hi-Z chain).
A **batch** is one `(class, view)` pair. Classes and views are declared at
frame setup; nothing branches on "what am I" afterward.

T4 runs *after* T5 and feeds the *next* frame's T2. There is no same-frame
two-phase occlusion: it costs a mid-frame barrier and a second classification
to remove an artifact that a one-frame lag bounds to camera cuts (a
`scene_disable_occlusion(frames)` hatch covers teleports).

---

## 1. Pass inventory (the whole renderer is six transforms)

| # | transform | input table(s) | output table(s) | frequency |
|---|-----------|----------------|-----------------|-----------|
| T1 | upload dirty | `dirty_slots[]`, `dirty_rows[]` (CPU) | `gpu_inst[]` | on change |
| T2 | cull per (class,view) | `cull_row[]`, `gpu_inst[]`, `cull_mesh[]`, view consts, Hi-Z | per-batch `survivors[]` + `survivor_count` | per frame |
| T3 | compact per (class,view) | `survivors[]`, `survivor_count` | `vis_all[]`, `draw_all[]`, `draw_count` | per frame |
| T4 | Hi-Z build per view | depth image | Hi-Z chain | per frame |
| T5 | draw per (class,view) | `draw_all[]`, `draw_count`, `vis_all[]` | framebuffer | per frame |
| T6 | counters readback | GPU counters | `SceneCounters` (lag 2) | per frame |

Every section names one transform, its inputs, its outputs, and what it never
touches. No two tables share a row format unless the access pattern is
identical.

---

## 2. `gpu_inst[]` — one global instance transform table

There is **one** instance transform table for the whole scene, indexed by
instance slot. It is shared by every class and every view. There are no
per-batch copies: a shadow caster and its visible colour draw read the same row.
Copying it per batch (as earlier drafts did) duplicates storage, duplicates the
dirty-upload path, and creates a desync class for no benefit.

```c
/* 20 bytes, 4-byte aligned (no 16-byte padding games). One table, all
   readers. Every field is an arithmetic input; none is ever tested. */
typedef struct GpuInstance {
    float    pos[3];    /* 12  world-space position                  */
    uint32_t quat;      /*  4  10-10-10-2 normalized quaternion      */
    uint16_t scale;     /*  2  half uniform scale                    */
    int16_t  lod_bias;  /*  2  signed LOD bias, per instance         */
} GpuInstance;          /* 20 bytes.                                 */
_Static_assert(sizeof(GpuInstance) == 20, "GpuInstance is 20 bytes");
```

Decisions this locks in:

- **World-space `f32` position, no chunk table.** Half positions need a
  chunk-relative scheme, which forces a *dependent* chunk-origin load in the
  cull thread — pointer chasing inside a 20k-thread dispatch, forbidden by
  AGENT.md §6.2. The +4 bytes/instance over an f16 layout is 16 KB/frame at
  4096 instances, against a vertex stream the design already budgets at
  hundreds of MB/frame. Record size is not optimized in isolation
  (`improved-threed.md` §5 says the same); the cull stays one sequential load.
- **Uniform scale (per instance).** The importer canonicalizes every node
  transform before it reaches this row: non-uniform scale and shear are baked
  into static vertices at load, negative scale is resolved in the loader
  (flipping winding/culling), skinned meshes stay in coherent skin space. As a
  consequence the normal transform is *just the quaternion* — the cull and the
  VS never need an inverse-transpose or the cofactor 3-cross-product path.
- **No `flags` byte.** `dynamic` is not a field (T1 keys off the dirty event
  list, §13); `skinned`, `two-sided`, `alpha` are table membership (§4, §12).
- **No stored bounds.** The world sphere is derived, not stored:

```c
/* exact under uniform scale; no max(), no approximation */
float3 center_w = inst.pos + quat_rotate(inst.quat, mesh.local_center * hi(inst.scale));
float  radius_w = hi(inst.scale) * mesh.local_radius;
```

Two extra ALU ops per candidate and zero bytes per instance, versus 16 B per
instance stored today.

---

## 3. Render classes and views

### 3.1 Render class

A class is the set of pipeline state that cannot vary within one draw: vertex
stage, raster state (cull mode), blend state, and index width. Membership is
resolved **once at load/insert** from mesh material + skin flag; it is never
re-read per frame. A mesh never changes class; an instance never changes class
(its meshes do not).

```
class            vertex stage    raster      blend        index width
---------------  --------------  ----------  -----------  -----------
opaque           rigid           back-cull   off          16
doubleside       rigid           no-cull     off          16
blend            rigid           back-cull   alpha-blend  16
skin             skin            back-cull   off          16
opaque32         rigid           back-cull   off          32   (mesh > 64k verts)
```

The set is content-driven and small (typically ≤5). Each class owns:

- `cull_row[]` — the candidate table (§5), written at insert;
- `survivors[]`, `survivor_count` — per view (§6);
- `vis_all[]`, `draw_all[]`, `draw_count` — per view (§7);
- one cull kernel entry and one draw pipeline (§6, §8).

Adding a class is adding a table + a pipeline, never an `if` in an existing
kernel.

### 3.2 View

A view is a camera: VP rows, viewport, near/far, LOD target, and its own Hi-Z
chain. A shadow cascade, a reflection probe or a cube face is one more element
of `GpuView[]`, not a new code path. The cull dispatch is identical across
views; only the constants differ. Each view owns its own survivor/vis/command
arrays (per frame-in-flight lane).

Shadow rendering is a **view**, not a class: the caster candidates are culled
with the frustum-only kernel (no Hi-Z), and the draw pipeline is the class's
depth-only variant. The caster set is therefore not a separate class entry for
every combination — it is "the classes this view draws, depth-only".

### 3.3 Batch

`batch = (class, view)`. A batch is a flat candidate array, one cull kernel
entry, one compaction, one draw. Nothing ever asks "what am I" — the batch *is*
the answer. The CPU issues one `vkCmdDrawIndexedIndirectCount` per batch.

---

## 4. Mesh tables — one per reader

Two device tables, split by reader. The cull kernel reads only `CullMesh`; the
vertex shader reads only `ShadeMesh` (broadcast per draw). Neither pays for the
other half.

```c
/* 32 bytes, read by T2. No device addresses, no materials. 8-wide so a
   candidate load is one cache line covering two rows. */
typedef struct CullMesh {
    uint16_t lod_first;     /*  2  base into LodRow[]                    */
    uint16_t lod_count;     /*  2  LOD rungs for this mesh              */
    uint32_t local_center;  /*  4  half3 local bounding-sphere centre   */
    uint32_t local_radius;  /*  4  half radius + half spare             */
    uint32_t cluster_first; /*  4  cluster table entry (§10)            */
    uint32_t cluster_count; /*  4  cluster count                        */
    uint32_t pad[2];        /*  8  8-wide cull loads                    */
} CullMesh;                 /* 32 bytes.                                 */
_Static_assert(sizeof(CullMesh) == 32, "CullMesh is 32 bytes");

/* 16 bytes, read by the VS via gl_DrawID (broadcast). */
typedef struct ShadeMesh {
    uint64_t vertex_stream; /*  8  device address, pooled vertex arena   */
    uint32_t pack;          /*  4  base_vertex:20 | material:12          */
    uint32_t pad;           /*  4                                        */
} ShadeMesh;                /* 16 bytes.                                 */
_Static_assert(sizeof(ShadeMesh) == 16, "ShadeMesh is 16 bytes");
```

LOD rungs live in a side table indexed by `lod_first + lod`, read only when a
candidate is visible enough to need a ladder:

```c
typedef struct LodRow {
    float    error;        /*  4  screen-space error threshold          */
    uint32_t first_index;  /*  4  into the index arena (this LOD)       */
    uint32_t index_count;  /*  4                                         */
} LodRow;                  /* 12 bytes.                                 */
```

`alpha_mode` and `uv_set` survive **only as load-time routers** (which class,
which vertex stream), never as per-frame reads. There is no
`CullMesh.flags` byte: index width, skin, two-sided and alpha are class
membership (§3.1).

---

## 5. Candidates — per-class `cull_row[]`

A candidate is one `(instance, mesh)` pair that needs culling in one class. The
table is written once at insert (rare, off the frame path) and read
sequentially every frame. `mesh` is a **load index**, never branched on.

```c
/* 8 bytes. slot is a load index into gpu_inst[]; mesh into CullMesh[].
   No bias (it lives on the instance), no flags (class is table membership). */
typedef struct CullRow {
    uint32_t slot;  /*  4  instance slot                     */
    uint16_t mesh;  /*  2  CullMesh index                    */
    uint16_t pad;   /*  2                                     */
} CullRow;          /* 8 bytes.                                 */
_Static_assert(sizeof(CullRow) == 8, "CullRow is 8 bytes");
```

A candidate belongs to exactly one class; one instance appears in every class
whose meshes it uses (multi-membership, like ECS). Dispatch count for a batch is
its candidate count — a uniform, CPU-known at insert time. There is no
per-candidate tail `if` and no `mesh_set_first`/`mesh_set_count` indirection.

---

## 6. T2 — cull (per batch)

One compute dispatch per batch, `numthreads(64,1,1)`, one thread per candidate.
The entire kernel:

```c
/* tid -> cull_row[tid] -> gpu_inst[slot] -> cull_mesh[mesh]. No branch on
   row data; the only exits are bounds (tid < count) and the visibility test. */
CullRow     cr   = cull_rows[class][tid];   /* candidates are per class */
GpuInstance inst = gpu_inst[cr.slot];
CullMesh    cm   = cull_mesh[cr.mesh];

float3x3 basis = quat_to_basis(inst.quat);             /* ~12 mul            */
float  s       = hi(inst.scale);
float3 c       = inst.pos + mul(basis, half3(cm.local_center) * s);
float  r       = s * hi(cm.local_radius);

uint lod = lod_select(cm, c, view, inst.lod_bias);     /* arithmetic, §10    */

if (!frustum_visible(c, r, view)) return;              /* mirrored-plane test */
if (occlusion_on && hiz_occluded(c, r, view)) return;  /* prev-frame Hi-Z, §9 */

/* one atomic per survivor; group id is derived, not stored */
uint key = (lod << 30) | (cr.mesh << 16) | cr.slot;    /* 2 | 14 | 16        */
uint idx;
InterlockedAdd(batch.survivor_count, 1, idx);
batch.survivors[idx] = key;                  /* batch = (class,view) */
```

Properties:

- **One atomic per survivor**, the same cost model Niagara's `drawcull`
  documents. Contention is bounded by survivors, not by candidates.
- **Group id is derived** from `mesh` + selected `lod`; the compaction pass
  (§7) buckets it. Nothing stores "which group am I".
- The dispatch is **candidate-parallel**, so tail lanes are handled by the
  `tid < count` guard, not by a data-dependent mesh-set loop.
- The visibility test for a class that skips occlusion (shadow view) is a
  *different kernel entry*, not a flag tested inside this one.

Frustum test: the mirrored-plane fast path for symmetric perspective cameras
(`center.z·f1 − |center.x|·f0 > −r`, `center.z·f3 − |center.y|·f2 > −r`, plus
near/far), generic six-plane fallback for off-centre / orthographic / cube
views. Both are derived from the projection rows in-kernel; no 96-byte plane
block is pushed. This is strictly tighter and cheaper than six normalized
planes.

---

## 7. T3 — compact (per batch)

T2 produces a dense, unordered survivor list. T3 turns it into a dense command
array and a dense visible array. It is a **counting sort over group ids**
(group = `(mesh, lod)` within the class):

1. **Histogram.** Clear `group_count[G]` (G = `mesh_count × lod_count` for the
   class), then each thread reads its survivor's group and atomically
   increments `group_count[g]`.
2. **Scan.** Exclusive prefix sum over `group_count[G]` produces
   `vis_base[g]` (survivor offsets) and, over `(count > 0)`, `cmd_index[g]`
   plus the batch's `draw_count` (number of non-empty groups). Implementation
   is private: a single-workgroup scan when `G` fits the group size, two-level
   otherwise. There is no hard-coded group size.
3. **Emit + scatter.** One thread per group writes its command; one thread per
   survivor writes `vis_all[vis_base[g] + local] = slot` with
   `local = InterlockedAdd(cursor[g], 1)`.

```c
/* 32 bytes: 20-byte VkDrawIndexedIndirectCommand + 12-byte extension.
   Written by T3, read by the hardware and by the VS through gl_DrawID. */
typedef struct GpuDraw {
    uint32_t index_count;    /* static per (mesh,lod)                  */
    uint32_t instance_count; /* T3: group survivor count               */
    uint32_t first_index;    /* static: LodRow.first_index             */
    uint32_t vertex_offset;  /* static: ShadeMesh base_vertex          */
    uint32_t first_instance; /* T3: vis_base[group]                    */
    uint32_t mesh_lod;       /* static: mesh:16 | lod:8 | class:8      */
    uint32_t pad[2];         /*  8  reserved, zero                     */
} GpuDraw;                   /* 32 bytes, stride for the Count draw.   */
_Static_assert(sizeof(GpuDraw) == 32, "GpuDraw is 32 bytes");
```

The invariant T5 relies on: **`draw_count`, `draw_all[]`, and `vis_all[]` are a
consistent triple when `scene_cull()` returns** — `draw_count` dense commands,
each pointing at its own contiguous `vis_all[]` range.

Why this is a real pass and not ceremony: T2 is candidate-parallel and rejects
most candidates, so it cannot write grouped ranges without worst-case reserved
capacity per group (which scales memory with `mesh × lod`) or a size-class
allocator. T3 pays a small fixed `G`-sized scan and one scatter atomic per
survivor to get a **dense** command array and a dense visible array — that is
what makes `vkCmdDrawIndexedIndirectCount` possible with no CPU knowledge and
no wasted command records.

Body of a `GpuDraw` written by T3:

```c
GpuDraw d = template[g];                 /* LOD0 constants + mesh_lod    */
d.index_count = lod[g].index_count;      /* already selected by T2's key */
d.first_index = lod[g].first_index;
d.instance_count = group_count[g];
d.first_instance = vis_base[g];
draw_all[view][cmd_index[g]] = d;
```

---

## 8. T5 — draw (per batch)

One `vkCmdDrawIndexedIndirectCount` per batch per view over the compacted
commands:

```c
cmd_bind_index_buffer(cmd, class_index_arena, class_index_type); /* once/pass */
cmd_draw_indexed_indirect_count(cmd, draw_all_slice,              /* offset 0  */
                                draw_count_slice,                 /* offset 0  */
                                MAX_GROUP_COUNT, sizeof(GpuDraw));
```

The VS resolves identity with **no per-draw push**:

- `gl_DrawID` indexes `draw_all[view]` → `mesh_lod` → `ShadeMesh` (mesh,
  material, `vertex_stream`, `base_vertex`). This is uniform across the draw, so
  it is a broadcast load, not a per-lane gather. `shaderDrawParameters` is
  already enabled in the backend.
- `slot = vis_all[first_instance + SV_InstanceID]` → `gpu_inst[slot]`.
- Vertices come from `shade_mesh.vertex_stream` (device address) with the index
  buffer bound for real (`first_index` / `vertex_offset` from the command).

Consequences:

- `firstInstance` is a **base into `vis_all[]`**, not a draw index.
  `drawIndirectFirstInstance` is therefore a **hard requirement** and must be
  enabled in device setup.
- The vertex shader may read `gl_DrawID` and `SV_StartInstanceLocation`; it
  never re-reads a mesh id from a per-draw constant or from the visible entry.
- The visible entry is a **plain `uint32_t` instance slot**. Mesh and LOD are
  uniform per command, so they do not belong in the entry. This shrinks `vis[]`
  and removes a decode from the VS.

CPU Vulkan calls per frame in the scene are a function of **batches and views**,
never of geometry: one pass begin + one index bind + one Count draw per batch.

---

## 9. T4 — Hi-Z (per view)

Build the depth pyramid from **this** frame's depth after T5; T2 next frame
consumes it. Per view, frame-persistent, sized at `scene_create`/resize — never
allocated per frame (that is why there is no `wait_idle` in the loop).

- Test with an explicit 2×2 min-`Load` (conservative, sampler-free) rather than
  a min-reduction filtered sampler; no custom sampler, prediction-safe.
- Mip selection with the one-level-finer correction (test whether the bounding
  rect spans ≤2 texels one level down, else step down), which roughly halves
  false occlusion.
- Only a class's no-Hi-Z kernel (shadow) skips the test; the switch is kernel
  choice, not a flag in the row.

Locked invariants that keep previous-frame Hi-Z correct for free:

1. **Transparent passes never write depth while Hi-Z is on.** Non-monotonic
   depth only exists if a later pass writes depth an earlier pyramid read. The
   rule removes the case with zero GPU cost; an effect that genuinely needs it
   opts out of Hi-Z per view instead of paying for a handling path in the cull.
2. **`scene_disable_occlusion(frames)`** disables occlusion for N frames after
   a teleport/camera cut. Zero cost when unused.

A same-frame scratch pyramid is rejected: it cannot feed a cull that already
ran, so it needs a second full cull (~2.5–3× the cull stage) to fix a case that
occurs on ~0% of frames.

---

## 10. LOD and clusters

LOD selection is arithmetic on data the cull already produced — the world
centre, radius and `w_clip` are in hand for the occlusion test:

```c
float dist   = length(center_view) - radius_w;
float metric = dist * view.lod_target / scale;
uint  lod    = 0;
while (lod + 1 < cm.lod_count && lod_row[cm.lod_first + lod + 1].error < metric)
    ++lod;
lod = clamp(lod + inst.lod_bias, 0, cm.lod_count - 1);
```

- The selected `lod` is packed into the survivor key (§6), so T3 reads it as a
  load index and the command carries `first_index`/`index_count` for that rung.
- Until the asset carries ladders, `lod_count = 1` and the walk is a no-op; the
  shader needs no `#ifdef`.
- This is the dominant win: at 16 B/vertex and 4096 instances, dropping distant
  instances to LOD2 removes most of the vertex stream.

Clusters (meshlets), still without mesh shaders:

- `CullMesh.cluster_first/count` → a `Cluster[]` table (sphere + cone).
- A **second dense dispatch over the survivors of T2** appends `(slot, cluster)`
  into the same batch's survivor stream, so T3 and T5 do not change shape — the
  visible array simply holds cluster draws instead of instance draws.
- Cone culling kills back-facing clusters before the vertex stage. This is a
  deferred, measured step: it requires the cluster tables in the asset format
  and a second sync point (the second dispatch needs the survivor count from
  T2, i.e. an indirect dispatch).

Collective LOD: N identical distant instances become one `GpuInstance` plus a
count in a crowd batch; the VS expands by `SV_InstanceID`. This reduces instance
count *before* optimizing instances (`game-engine-dod` §11.4).

---

## 11. Materials

```c
typedef struct GpuMaterial {
    uint32_t base_color; /*  4  UNORM8x4                          */
    uint32_t texture;    /*  4  bindless albedo:16 | normal/orm:16 */
    uint32_t flags;      /*  4  sampler:2 | ...                   */
} GpuMaterial;           /* 12 bytes.                             */
_Static_assert(sizeof(GpuMaterial) == 12, "GpuMaterial is 12 bytes");
```

The FS reads one material per draw (uniform). `alpha_mode` and `uv_set` are
resolved to class/stream at load and never reach a shader test. The sampler is a
2-bit field because scene materials only ever want linear/nearest-clamp.

---

## 12. Skinning — a class with its own kernel

`skin` is a class. Its candidate rows reference skinned instances; rigid
instances are not in it. There is no `if (skinned)` anywhere: the skin VS is a
separate pipeline bound only for the skin batch.

- `skin_ref[]` is a side table (`palette:20 | joint_count:12`) written at
  insert; only the skin VS reads it.
- **Pose dedupe** by `(model, clip, quantized_time)` → one palette per unique
  pose per frame (CPU hash, ~3.8 KB/pose). This matters: 4096 palettes × 60
  joints × 64 B would be ~15 MB/frame; a shared pose is 3.8 KB.
- Skinning runs as a dispatch before the cull (a separate transform over a
  separate table). It is not fused into the cull — job-parallel and
  candidate-parallel grids cannot share one without a branch in every thread.

---

## 13. T1 — upload: event list in, two sequential spans out

Movement is an event, not a state. The game appends `(slot, row)` to its dirty
list when it writes a transform; T1 drains it. No per-row dirty bit, no
`if (dirty)` scan.

```c
typedef struct InstanceUpdate { uint32_t slot; GpuInstance data; } InstanceUpdate;

/* AoS at the API (ergonomics); SoA on the wire (two sequential memcpys). */
void scene_upload_instances(Scene *s, VkCommandBuffer cmd,
                            ByteSpan updates /* InstanceUpdate[N] */);
```

Rules:

- `gpu_inst[]` is a persistent device buffer allocated at `scene_create(cap)`; it
  is never re-uploaded wholesale.
- Static instances upload once at spawn → **0 bytes/frame by construction**.
- Uploads go staging→device through the frame command buffer only. No
  `vkQueueSubmit`, no `vkQueueWaitIdle` anywhere in the frame loop.
- The backend splits the AoS span into `slots[N]` + `rows[N]` at the staging
  boundary (two contiguous copies, no per-element stride skip).

---

## 14. Residency, capacity, overflow

Every table is a `BufferSlice` suballocated from one of three pools. There are
**exactly three `VkBuffer`s**:

- `cpu_pool` — LINEAR, host-visible, reset each frame (push staging, transient).
- `staging_pool` — RING, host-visible, freed by timeline fence (uploads).
- `gpu_pool` — TLSF, device-local, long-lived (all GPU tables, arenas, Hi-Z).

One slice per table, indexed by slot — never one allocation per row (that would
be pointer-chasing the TLSF on the frame path).

| table | pool | lifetime | align |
|---|---|---|---|
| `gpu_inst[]` | `gpu_pool` | `scene_create` capacity; grow = new slice + copy + deferred free | 16 |
| `cull_row[]` per class | `gpu_pool` | `scene_create`; grow on insert | 16 |
| `cull_mesh[]`, `shade_mesh[]`, `lod_row[]`, `cluster[]`, materials | `gpu_pool` | load-time | 16 |
| vertex / index arenas | `gpu_pool` | load-time grow-only; one slice per stream | 16 / 2 or 4 |
| `survivors[]`, `vis_all[]`, `draw_all[]`, counts | `gpu_pool` | `scene_create`, **per view per frame-in-flight lane** | 16 (draw stride 32) |
| `dirty_slots[]` / staging, push staging, `PassDesc[]` | `cpu_pool` / `staging_pool` | frame-transient | 16 |
| Hi-Z chain | `gpu_pool` | per view, frame-persistent | image, 256 |
| counters readback | `cpu_pool` | per frame-in-flight lane, lag 2 | 4 |

Rules:

1. Capacity is fixed at `scene_create`/load. Insert past capacity **fails loud**;
   a slice never grows mid-frame. Growth = new slice + copy + deferred free past
   last timeline use.
2. Device addresses are baked at insert/load (`gpu_base_addr + slice.offset`)
   because the pool buffer base never moves. Shaders never add base+offset per
   thread.
3. Every GPU-written count (`survivor_count`, `draw_count`, `group_count`)
   **clamps at capacity** and increments a `dropped` counter. Overflow is never
   silent. T2's per-survivor atomic clamps at `MAX_SURVIVORS`; T3's histogram is
   sized `G`.
4. Frame-lifetime GPU tables are created at init/resize and destroyed only at
   shutdown, so `wait_idle` never appears in the frame loop.

---

## 15. Sync — intent in, barriers out

Call sites name intent; `begin_pass` owns stage masks. Buffers join the pass
description:

```c
typedef struct BufferAccess {
    BufferSlice           slice;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2        access;
} BufferAccess;

typedef struct PassDesc {
    const PassAttachment *colors;         uint32_t color_count;
    const PassAttachment *depth;
    const BufferAccess   *buf_reads;      uint32_t buf_read_count;
    const BufferAccess   *buf_writes;     uint32_t buf_write_count;
    ByteSpan              push;           /* one push per pass, never per draw */
    PipelineID            pipeline;
} PassDesc;
```

- T2 writes `survivors`, `survivor_count` → T3 reads them (one barrier).
- T3 writes `vis_all`, `draw_all`, `draw_count` → T5 reads them as
  `DRAW_INDIRECT` + `VERTEX_SHADER` (one barrier).
- T4 reads depth (`COLOR_ATTACHMENT_OUTPUT`/`DEPTH_STENCIL_ATTACHMENT` write) →
  next frame's T2 reads Hi-Z.
- All buffers are per frame-in-flight lane, so lanes never alias.

Barrier count per frame: 1 upload + 1 per cull + 1 per compact + 1 per draw +
1 Hi-Z level chain. No hand-written `VkMemoryBarrier2` at call sites.

---

## 16. Counters and profiling

```c
typedef struct SceneCounters {
    uint32_t submitted;      /* candidates per class                */
    uint32_t culled_frustum;
    uint32_t culled_hiz;
    uint32_t drawn;          /* survivors                           */
    uint32_t lod[4];
    uint32_t draws;          /* non-empty groups                    */
    uint32_t dropped;        /* capacity overflows                  */
} SceneCounters;

bool scene_counters_read(Scene *s, SceneCounters *out); /* lag 2, no stall */
```

GPU-written atomics in the cull (no branches); read back through a host-visible
ring keyed on `timeline_last_submitted`, 2 frames late, never a sync point.
`GPU_SCOPE` timers around T2/T3/T4/T5, shown by default in the panel.

A layout change lands only with: named passes + per-pass access pattern (§1),
byte budget with a `_Static_assert`, no per-row field tested by
`if`/`switch`/`?:` in any T1–T6 loop, and a before/after number from
`scene_counters_read` plus GPU timers.

---

## 17. Static-view amortization (the cozy-builder win)

A builder scene is often still. The doctrine's own question applies at frame
granularity: *is avoiding the work cheaper than doing the work?* When a view's
VP and viewport are unchanged and no instance in any class was dirtied since the
last time that frame-in-flight lane ran this view, T2/T3 are skipped and T5
reuses the lane's still-valid `draw_all[]`/`vis_all[]`.

Correctness requirement: the skip is valid **only per frame-in-flight lane**,
because each lane owns its own command/visible arrays and the skip never writes
them. Track a per-lane `(view_epoch, scene_epoch)` pair; a bump on either forces
a re-cull. This is a named policy switch so the A/B is a one-line change, not a
recompile.

---

## 18. Shader invariants (make the no-branch rule mechanical)

The whole design rests on "zero bytes any consumer branches on", so it is
enforced, not asserted in prose:

1. **Size.** Every shared GPU struct carries `_Static_assert(sizeof == N)` at
   its definition. Prose sizes are forbidden (the 112→32 material drift is the
   precedent).
2. **Branch audit.** A check over the cull/VS/FS sources and their SPIR-V fails
   on data-dependent `if`/`switch`/`?:`, whitelisting only: the dispatch bounds
   guard, the visibility early-outs, the LOD ladder walk, and tail-lane exits.
   No membership test may appear.
3. **Shared layout.** C and Slang read one authoritative definition
   (`scene_shared.h`), with the stride/offset/member-order checks above.

---

## 19. Review checklist (before calling this done)

```
LAYOUT
[ ] one gpu_inst[] table; no per-batch instance copies
[ ] hot rows hold no pointers; device addresses only in GPU tables
[ ] SoA vs AoS chosen from the access pattern, justified per table
[ ] one atomic value per cell; no nullable members
[ ] no per-row membership flag anywhere (audit every struct against §3/§4)

CONTROL FLOW
[ ] no data-dependent branch in cull/compact/VS beyond whitelisted (§18)
[ ] class/view is table membership + pipeline choice, never a runtime switch
[ ] visibility = survivor list membership, not a scanned bool array

MEMORY
[ ] every table is a BufferSlice from the right pool; three VkBuffers total
[ ] capacity fixed at scene_create; overflow clamps + counts, never grows
[ ] index width is a load-time class split; index buffer binds once per pass
[ ] wait_idle never appears in the frame loop

CONCURRENCY / SYNC
[ ] one writer per buffer; barriers derived from PassDesc, not hand-written
[ ] per-view per-lane arrays; no cross-lane aliasing

PROCESS
[ ] named passes with input/output/frequency written down (§1)
[ ] before/after number per layout change; revert if no gain
[ ] compiles warning-clean; every shared size asserted
```

---

## 20. Byte budget (cubepets model: 4096 instances, 6 meshes)

| item | bytes | note |
|---|---:|---|
| instance row | 20 | §2 |
| cull_mesh / shade_mesh | 32 / 16 | §4, split by reader |
| candidate row | 8 | §5 |
| draw command | 32 (stride) | §7, Count draw |
| material | 12 | §11 |
| push | 32 | once per pass, zero per draw |
| instance upload | 20 × dirty | static → 0 B/frame (§13) |

Order of magnitude per frame at 4096 instances: instance read ~80 KB, candidates
~24 KB, commands ≤ survivors × 32 B; the vertex+index stream dominates by
~1000× (hundreds of MB). **The performance work is LOD + Hi-Z + clusters, not
instance compaction.** Everything else in this document is CPU time and
cleanliness.

---

## 21. Flag → table map (every former flag accounted for)

| former flag / enum / switch | replacement | where |
|---|---|---|
| `flags` bit0 dynamic | `dirty_slots[]` event list | §13 |
| `flags` bit1 skinned | `skin` class membership | §3.1, §12 |
| `flags` bit2 shadow | shadow view over caster classes | §3.2 |
| `flags` bit3 two-sided | `doubleside` class + fixed raster state | §3.1 |
| `lod_bias` | `GpuInstance.lod_bias` (arithmetic input) | §2 |
| `mesh_set` + set tables | `CullRow.mesh` load index | §5 |
| `GpuMesh.flags` (width/skin/ds/alpha) | class membership + index-width class split | §3.1, §4 |
| `GpuMaterial.alpha_mode` | load-time class router | §4, §11 |
| `GpuMaterial.uv_set` | load-time stream router | §4 |
| `OcclusionMode` switch | kernel choice: opaque runs Hi-Z, shadow does not | §6, §9 |
| blend sort order | class order (opaque → doubleside → blend); blend class sorts its groups by depth | §3.1, §7 |

---

## 22. Non-goals (explicit, not deferred phases)

No mesh/task shaders. No same-frame two-phase occlusion. No render graph layer
(the pass API plus an explicit view array is enough). No per-draw push
constants. No CPU-built indirect command arrays. No non-indexed draws with manual
index fetches (they double vertex traffic). No per-instance stored bounds. No
`int` in serialized/GPU structs.
