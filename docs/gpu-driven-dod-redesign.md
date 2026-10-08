# GPU-driven 3D renderer: fully data-oriented redesign (from scratch)

Status: design target. Supersedes `gpu-driven-api-design.md` §1–§9 as the
normative layout reference; that file remains as mechanism history.
Doctrine: `AGENT.md` §1 (non-negotiables), `dod-performance` skill,
`game-engine-dod` skill. The governing rule is AGENT.md §1.3:

> Flags that introduce `switch`es/`if`s in a hot loop are a layout bug.
> Do not store "what am I" per row and branch on it — store *eligibility*
> as table membership and run one specialized transform per table.
> A per-row field used only as a *load index* is not a flag.

Anything a shader or CPU loop ever tested with `if`/`switch`/`?:` on a
data-dependent condition is gone from the row — replaced by table
membership, a separate pipeline, or a load index. What remains per row is
only what its transform *loads*.

---

## 0. Pass inventory (the whole renderer is six transforms)

| # | transform | input table(s) | output table(s) | frequency |
|---|-----------|----------------|-----------------|-----------|
| T1 | upload (dirty instances) | `dirty_slots[]` | `gpu_inst[]` | on change |
| T2 | cull per batch+view | `gpu_inst[]`, `CullRow[]`, view consts | `vis[]`, batch draws | per frame |
| T3 | compact per view | per-batch `vis[]` + counts | `vis_all[]`, `draw_all[]` | per frame |
| T4 | Hi-Z build | depth pyramid | Hi-Z chain | per frame |
| T5 | draw (one pipeline per batch) | `vis_all[]`, `draw_all[]` | framebuffer | per frame |
| T6 | counters readback | GPU counters | `SceneCounters` (lag 2) | per frame |

Each section names one transform, its inputs, its outputs, and what it
never touches. No two sections share a row format unless the access
pattern is identical.

---

## 1. Batches, not flags: the single structural decision

Every former flag bit becomes batch membership. An instance belongs to
exactly the batches whose processing it needs; a batch is one flat
instance array + one specialized cull kernel + one specialized draw
pipeline. Nothing ever asks "what am I" — the batch *is* the answer.

```
former flag bit          batch table            cull kernel      draw pipeline
-----------------        ------------           -----------      -------------
opaque (default)         opaque_inst[]          cull_opaque      pipe_opaque
two-sided                doubleside_inst[]      cull_opaque*     pipe_doubleside
alpha-blend              blend_inst[]           cull_opaque*     pipe_blend
casts shadow             shadow_inst[]          cull_shadow      (depth only)
skinned                  skin_inst[]            cull_skin        pipe_opaque/skin VS
* same kernel entry, different output draw list — no branch, see §4.
```

Movement cost: instances change batches only on material/participation
change (spawn, material swap, shadow toggle) — rare, event-driven, off
the frame path. AGENT.md §1.3's caveat (rapidly changing state may keep
its enum) does not apply: none of these flip per tick. The `dynamic` bit
is deleted entirely — dirty upload (§6) keys off the `dirty_slots[]`
event list, never off a per-row bit.

Consequences:

- Every cull kernel loops over a homogeneous array: no `if (two_sided)`,
  no `if (alpha)`, no `if (skinned)` inside any dispatch.
- Every draw pipeline has fixed state: cull mode, blend enable, vertex
  rate are pipeline constants, never per-row data.
- Transparent sorting (§5) touches only `blend_vis[]`.
Capacity plan (cubepets model: 4096 instances): opaque 4096, doubleside
512, blend 256, shadow 2048, skin 0 (grows on demand). Each table is a
fixed-capacity device buffer at `scene_create`; counts are GPU-written
per frame.

---

## 2. Instance rows: 16 bytes, transform-only, zero branch inputs

The opaque cull kernel loads position, scale, rotation — and nothing
else. So that is the entire row:

```c
/* 16 bytes. The ONLY per-instance GPU row. Loaded by T2, written by T1.
   No tint, no flags, no bias, no set id — see the table below. */
typedef struct GpuInstance {
    uint16_t pos[3];    /* 6  half  chunk-relative position (Q1) */
    uint16_t scale;     /* 2  half  uniform scale                */
    uint32_t spare;     /* 4  chunk id (Q1 frame anchor)         */
    uint32_t quat;      /* 4  10-10-10-2 normalized quaternion   */
} GpuInstance;          /* 16 bytes.                             */
/* MU_STATIC_ASSERT(sizeof(GpuInstance) == 16); */
```

Where every deleted byte went (each names its replacement):

| old byte | replacement | why it is not a loss |
|----------|-------------|----------------------|
| `mesh_set` | `CullRow.mesh` (§3, one row per candidate at batch-insert) | cull thread loads mesh directly; set tables deleted |
| `flags` bit0 dynamic | `dirty_slots[]` event list (§6) | movement is an event, not a state |
| `flags` bit1 skinned | `skin_inst[]` membership (§1, §7) | skin kernel iterates only skinned rows |
| `flags` bit2 shadow | `shadow_inst[]` membership (§1) | shadow cull iterates only casters |
| `flags` bit3 two-sided | `doubleside_inst[]` membership (§1) | separate pipeline, fixed raster state |
| `lod_bias` | `CullRow.bias` (§3) | bias varies per (instance, mesh), was in the wrong table |
| `tint` | `ShadeRow.tint` (§3) | VS/FS-only, survivor gather |

The 20→16 B shrink is incidental. The real win: zero bytes any consumer
branches on. Every field is an arithmetic input.

---

## 3. Cull rows and shade rows: per-candidate, per-reader

A cull thread is a *candidate* (instance x mesh). The old design derived
the mesh from the thread id via set tables (two dependent loads + a tail
`if`). The new design stores the candidate at batch-insert — one flat
row per candidate, written once, read sequentially every frame:

```c
/* 8 bytes per candidate. Written at batch-insert (rare), read by T2
   sequentially. mesh is a LOAD INDEX, never branched on. */
typedef struct CullRow {
    uint32_t slot;   /* 4  index into batch's gpu_inst[] */
    uint16_t mesh;   /* 2  index into CullMesh[]         */
    int8_t   bias;   /* 1  signed LOD bias, this (instance, mesh) */
    uint8_t  pad;    /* 1                                  */
} CullRow;           /* 8 bytes.                           */

typedef struct ShadeRow {
    uint32_t tint;   /* 4  palette index + material override bits */
} ShadeRow;          /* 4 bytes, VS survivor-gather only (§5).    */
```

Cull thread (entire kernel — no `if` on row data):

```c
/* tid -> CullRow[tid] -> inst = gpu_inst[slot] -> mesh = CullMesh[mesh] */
CullRow     cr   = cull_rows[tid];
GpuInstance inst = gpu_inst[cr.slot];
CullMesh    cm   = cull_mesh[cr.mesh];
/* frustum + Hi-Z + LOD from inst + cm + cr.bias -> write vis or drop */
```

The `local_m >= count` tail branch is gone. The `mesh_set_first` /
`mesh_set_count` tables are deleted. Dispatch count is the batch's
candidate count — GPU-visible uniform, CPU-known at insert time.

---

## 4. Mesh tables: one per reader, 24 + 16 bytes

Two tables because two readers with disjoint access:

```c
/* 24 bytes. Read by T2 cull kernels only. No addresses, no materials. */
typedef struct CullMesh {
    uint16_t first_cluster;  /* 2  cluster table entry (§8)          */
    uint16_t cluster_count;  /* 2                                     */
    uint16_t lod_group;      /* 2  LOAD INDEX into LOD ladder table   */
    uint16_t pad0;           /* 2                                     */
    uint32_t local_center;   /* 4  half3 local bounding sphere centre */
    uint32_t local_radius;   /* 4  half radius + half spare           */
    uint32_t index_count;    /* 4  this LOD's index count             */
    uint32_t pad1;           /* 4  8-wide for vectorized cull loads   */
} CullMesh;

/* 16 bytes. Read by T5 vertex shader only, broadcast per draw. */
typedef struct ShadeMesh {
    uint64_t vertex_stream;  /* 8  device address, pooled vertex arena */
    uint64_t index_stream;   /* 8  device address, pooled index arena  */
    uint32_t pack;           /* 4  base_vertex:20 | material:12 (LOAD INDEX) */
    uint32_t pad;            /* 4                                       */
} ShadeMesh;
```

Deleted from the old `GpuMesh`: `flags` (index width / skinned /
double-sided / alpha). Each becomes structure, not data:

- index width → separate index arenas per width; the draw's pipeline
  knows its width statically (or one 32-bit arena — width stops varying).
- skinned → `skin` batches use the skin VS; rigid batches the rigid VS.
- double-sided / alpha → `doubleside` / `blend` batches (§1), fixed
  rasterizer/blend state per pipeline.

`GpuMaterial` (12 B) keeps `sampler:2 | alpha_mode:2` — `alpha_mode`
survives ONLY as a load-time routing key (which batch + pipeline at
insert), never read per-frame. `uv_set` routes at load which vertex
stream the `ShadeMesh` points at. Neither reaches a shader `if`.

---

## 5. Draw lists: per-batch visible arrays, one sort key

Each batch owns its output: `vis_batch[]` (32-bit slots), a batch draw
array, GPU-written count. T3 compacts per-batch outputs into the
view-global `vis_all[]`/`draw_all[]` with a prefix sum — no CPU loop over
draws, no per-draw push, no per-draw bind.

```c
/* 20-byte VkDrawIndexedIndirectCommand + 12-byte sort/route block. */
typedef struct BatchDraw {
    uint32_t index_count;     /* GPU: LOD selection (§8)              */
    uint32_t instance_count;  /* GPU: T2/T3 write                      */
    uint32_t first_index;     /* load-time: from CullMesh LOD row      */
    uint32_t vertex_offset;   /* load-time: from ShadeMesh pack        */
    uint32_t first_instance;  /* GPU: T3 prefix-sum base into vis_all  */
    uint32_t sort_key;        /* GPU: mesh:16 | lod:8 | priority:8     */
    uint32_t mesh_slot;       /* GPU: lets VS skip the vis[] re-read   */
    uint32_t pad;             /* future batch-chain head               */
} BatchDraw;                  /* 32 bytes.                             */
```

Sorting: only `blend` batches sort, by `sort_key`, GPU radix sort over
small N — opaques emit in batch order (opaque → doubleside → blend).
Per-frame CPU cost per view: one pass begin + one index bind + one
`DrawIndexedIndirectCount` per non-empty batch (typically 2–3).

---

## 6. Uploads: event list in, two sequential spans out

Movement is an event. The game appends to `dirty_slots[]` whenever it
writes an instance transform; T1 drains the list. No per-row dirty bit,
no `if (dirty)` scan, no `dynamic` flag:

```c
/* CPU-side staging: AoS at the API (ergonomics), SoA on the wire. */
void batch_upload(Batch *b, VkCommandBuffer cmd,
                  const uint32_t *slots, const GpuInstance *rows, uint32_t n);
/* impl: one memcpy of slots[n], one memcpy of rows[n], one staging copy. */
```

Static geometry: inserted once, never dirtied — 0 bytes/frame by
construction, not by flag test. A fully-spinning 4096-crowd uploads
4096 × 16 B = 64 KB (was 328 KB authoring + 197 KB candidates).

---

## 7. Skinning: a batch with its own kernel

`skin_inst[]` holds only skinned instances: 16-B `GpuInstance` plus a
`skin_ref` sidecar (`palette slot : 20 | joint_count : 12`) written at
insert. The skin kernel iterates `skin_inst[]` densely — rigid instances
are not in the table, so no `if (skinned)` exists anywhere:

- Pose dedupe key `(model, clip, quantized_time)` → one palette per
  unique pose per frame (CPU hash, 3.8 KB per pose).
- The skin VS is a separate pipeline bound only for the skin batch's
  draw; the rigid VS has no skin inputs at all.

---

## 8. LOD and clusters: derived per candidate, bucketed per batch

LOD selection is arithmetic on data T2 already produced
(`screen_px = radius_w * proj_scale / w_near + bias`); the result is a
load index into the ladder, never a branch:

- Per (mesh, LOD): static `first_index`/`index_count`/`vertex_offset`
  rows at load. T2 writes the selected LOD's constants into the
  `BatchDraw` — the draw reads constants, never selects.
- Cluster path: `CullMesh.first_cluster/count` → cluster table (sphere +
  cone). A second dense dispatch over survivors appends `(slot, cluster)`
  to the same batch `vis[]`. Cone test is a predication, not a mode flag.
- Collective LOD: N identical distant instances → one `GpuInstance` +
  count in a `crowd` batch; the VS expands by `SV_InstanceID`.

---

## 9. Sync and passes: intent in, barriers out

Call sites name intent; the pass owns stage masks:

```c
typedef struct BufferAccess {
    BufferSlice           slice;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2        access;
} BufferAccess;

typedef struct PassDesc {
    const PassAttachment *colors;   uint32_t color_count;
    const PassAttachment *depth;
    const BufferAccess   *buf_reads;     uint32_t buf_read_count;
    const BufferAccess   *buf_writes;    uint32_t buf_write_count;
    ByteSpan              push;   /* 32 B ViewPush: view + bases + sun/misc */
    PipelineID            pipeline;      /* one per batch — fixed state, §1 */
} PassDesc;
```

`ViewPush` (32 B, once per pass): `view` + `cull_mesh_base` +
`shade_mesh_base` + `sun/misc`. No frustum planes in the push (derived
from VP rows in-kernel); no per-draw push (mesh via `BatchDraw.mesh_slot`).

---

## 10. Counters and review gate

```c
typedef struct SceneCounters {
    uint32_t submitted;      /* candidates per batch                   */
    uint32_t culled_frustum;
    uint32_t culled_hiz;
    uint32_t drawn;
    uint32_t lod[4];
    uint32_t draws;          /* non-empty batch draws                  */
} SceneCounters;
bool scene_counters_read(Scene *s, SceneCounters *out); /* lag-2, no stall */
```

A layout change lands only with: named passes + per-pass access pattern
(§0 table), byte budget with `MU_STATIC_ASSERT`, no per-row field tested
by `if`/`switch`/`?:` in any T1–T6 loop, and a before/after number from
`scene_counters_read` plus GPU timers around T2/T4/T5.

---

## 11. Flag-to-table map (complete — every old flag accounted for)

| old flag / enum / switch | where it died | replacement |
|--------------------------|---------------|-------------|
| `flags` bit0 dynamic | §1, §6 | batch membership + `dirty_slots[]` event list |
| `flags` bit1 skinned | §1, §7 | `skin_inst[]` batch + skin VS pipeline |
| `flags` bit2 shadow | §1 | `shadow_inst[]` batch + depth-only cull |
| `flags` bit3 two-sided | §1 | `doubleside_inst[]` batch + fixed raster state |
| `mesh_set` + set tables | §3 | `CullRow.mesh` load index at insert |
| `lod_bias` | §3 | `CullRow.bias` (per-candidate table) |
| `tint` | §3 | `ShadeRow.tint` (survivor-gather only) |
| `GpuMesh.flags` (width/skin/ds/alpha) | §4 | arenas per width, skin VS, §1 batches |
| `GpuMaterial.alpha_mode` | §4 | load-time batch router; never read per-frame |
| `GpuMaterial.uv_set` | §4 | load-time stream router; never read per-frame |
| `GpuView.caps/misc` flag fields | §9 | `ViewPush` fixed layout; Hi-Z levels per-view const |
| `OcclusionMode` switch | §1, T2 | opaque batches run Hi-Z; shadow batch skips it by kernel choice |
| blend sort order | §5 | `sort_key` in `BatchDraw`; only blend batch sorts |



