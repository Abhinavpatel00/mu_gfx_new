# `.muasset`: cooked GPU-ready asset format

Status: design target. Companion to `gpu-driven-dod-redesign.md` (the runtime
tables) and `gpu-driven-impl-plan.md` (the migration milestones).
Doctrine: `AGENT.md` §1, `dod-performance`, `game-engine-dod` §12 residency.

## 0. Why a format, not a faster parser

Today `scene3d_asset.c` runs `cgltf` at launch: JSON parse, accessor walk,
per-vertex float deinterleave, quantize, bounds, per-model `Buffer` upload.
That work is identical every launch — it is a load-time constant pretending
to be runtime work. A cooked format moves it offline once:

| cost today (per launch) | cooked (per launch) |
|---|---|
| JSON parse + base64 + accessor fixup | 0 — gone |
| per-vertex float math (deinterleave, normalize, pack) | 0 — bytes stored packed |
| per-model `Buffer` alloc + N small uploads | one staging copy into `gpu_pool` slices |
| bounds/spheres/LOD/clusters recomputed | precomputed, checksummed |
| batch routing by material flags at load | precomputed membership (`BATCH_ROUTE`) |

Target: asset load is `open + validate + 1 copy + address bake`, ≥10x faster
than the `cgltf` path on the same model, measured (§7).

## 1. Two files, two jobs

```
models/pet.mua        # binary blob: GPU-ready tables, offsets only, no pointers
models/pet.mua.json   # sidecar: provenance + budgets + timings, human-readable
```

- The `.mua` is what ships and what the runtime reads. Field order inside
  every row matches the shader's read order (redesign §2–§4). Loading never
  transforms a byte except the device-address bake (§5), which touches only
  the ~2.5 KB mesh tables — never the vertex mass.
- The `.mua.json` is what humans read: source path + content hash, cook argv,
  per-section budgets, cook timings, texture URIs. It is also the debug
  oracle: `muasset dump pet.mua` must print the same tables (§6).
- Rule: **anything debuggable in JSON must be derivable from the blob.**
  The sidecar is a cache of the dump, not a second source of truth. Tests
  enforce this by round-trip (§6).

Non-goals: no textures inside the blob (URIs + content hashes only, §8 —
images keep their own loaders and atlases), no scripting, no scene graph
(the runtime has no scene graph; the file has none either — nodes exist
only as the flat animation tables the skin kernel reads).

## 2. Binary layout (`.mua`)

Little-endian. Offsets only, no pointers. Every section starts 16-aligned;
every row's field order matches the shader's read order (redesign §2–§4),
so the loader never swizzles vertex mass.

```c
#define MUASSET_MAGIC   0x2E41554DU   /* ".MUA" little-endian */
#define MUASSET_MAJOR   1u
#define MUASSET_MINOR   0u

typedef struct MuassetHeader {
    uint32_t magic;          /* MUASSET_MAGIC */
    uint32_t version;        /* major << 16 | minor */
    uint32_t section_count;
    uint32_t total_size;     /* whole blob, header included */
    uint8_t  content_hash[32]; /* sha256 of source file(s) cooked */
    uint32_t flags;          /* bit0: has_skin, bit1: has_clusters, rest 0 */
} MuassetHeader;             /* 48 bytes. _Static_assert(sizeof == 48). */

typedef struct MuassetSection {
    uint32_t tag;            /* MUASEC_* below */
    uint32_t row_size;       /* bytes per row; 1 for raw arenas */
    uint32_t row_count;
    uint32_t offset;         /* from blob start, 16-aligned */
    uint32_t size;           /* row_size * row_count, padded to 16 */
    uint32_t checksum;       /* crc32 of section bytes */
} MuassetSection;            /* 24 bytes. */
```

Section tags (stable ABI — never renumber, only append):

```c
#define MUASEC_VERT_ARENA  1u  /* f32 pos/norm/uv rows, one stream per LOD */
#define MUASEC_IDX_ARENA   2u  /* u16 rows (u32 promoted mesh gets its own) */
#define MUASEC_CULLMESH    3u  /* 24 B rows, redesign §3.2, shader order    */
#define MUASEC_SHADEMESH   4u  /* 16 B rows; addrs stored as ARENA OFFSETS  */
#define MUASEC_LOD         5u  /* per (mesh,lod): first_index, count, error */
#define MUASEC_CLUSTER     6u  /* 64 B rows: sphere + cone + index range    */
#define MUASEC_SKIN        7u  /* inv-bind f32x12 rows + joint remap u16    */
#define MUASEC_MATERIAL    8u  /* 12 B rows, redesign §3.2                  */
#define MUASEC_BATCH_ROUTE 9u  /* u8 per mesh: batch id, cooked at cook     */
#define MUASEC_ANIM       10u  /* packed curves: times f32 + TRS keys       */
#define MUASEC_NAMES      11u  /* mesh/material/node names, debug only      */
```

Notes:

- `SHADEMESH` stores `vert_off` / `idx_off` (u64 arena-relative offsets),
  **not** device addresses. The loader bakes `gpu_base + off` once into the
  resident copy (§5) — the file stays relocatable, the GPU copy stays fast.
- `VERT_ARENA` packs one LOD stream after another; `LOD` rows point at
  `(stream_base, first_vertex)`. Interleaved `pos f32x3 + norm f32x3 + uv
  f32x2 + joints u8x4 + weights f32x4` = 48 B/vertex skinned, 32 B static
  (joints/weights omitted — separate streams per batch, never a flag
  tested per vertex; redesign §7 batch rule).
- `BATCH_ROUTE` is the cooked answer to "which batch does this mesh join":
  material alpha/skinned/double-sided bits are read **once at cook**, never
  branched on at runtime. Adding a batch = new id, never a wider flag.
- `NAMES` is debug-only and never uploaded to `gpu_pool`.

## 3. Sidecar (`.mua.json`)

Cooker-written, human-readable, never read by the runtime hot path:

```json
{
  "source": "models/pet.glb",
  "source_hash": "sha256:…",
  "cook_argv": "muasset_cook models/pet.glb -o models/pet.mua --lods 3 --clusters",
  "format": "1.0",
  "budgets": {
    "vertices": 6219, "indices": 21600, "meshes": 6,
    "vert_bytes": 199008, "idx_bytes": 43200,
    "cullmesh_rows": 18, "clusters": 0
  },
  "batch_route": [0, 0, 0, 0, 0, 0],
  "textures": [{"uri": "pet_albedo.ktx2", "hash": "sha256:…"}],
  "cook_ms": 41.2
}
```

Rule (§1): the sidecar is a cache. `muasset dump --json` regenerates it
from the blob; CI round-trips blob → dump → diff against the sidecar.

## 4. Cook pipeline (offline, slow path allowed)

`tools/muasset_cook.c` — host-only, links `cgltf`, never ships in the engine:

1. Parse glTF once. Canonicalize nodes (non-uniform scale/shear baked to
   vertices, negative scale flips winding — redesign Q3 rule, enforced here).
2. Bucket meshes by batch key `(alpha, skinned, doubleside)` → `BATCH_ROUTE`.
   New appearance = new batch id, never a runtime flag test.
3. Emit LOD ladders (decimate 100/50/25%), cluster tables (64–128 B rows:
   sphere + cone + index range) when `--clusters` is set.
4. Quantize nothing at cook except vertex packing (f32 stays f32 — instance
   TRS quantization is per-frame runtime work, not asset work).
5. Write sections 16-aligned, crc32 per section, sha256 of source in header.
   Cook prints per-section bytes + ms (feeds the sidecar `budgets`).

Failure policy: cooker fails loud on unknown extensions, u32 indices it
cannot split, or >256 mesh sets. No silent downgrade — a bad asset never
becomes a slow runtime branch.

## 5. Load path (runtime, fast path only)

```c
bool muasset_load(Renderer *r, const char *path, Muasset *out);
/* open + mmap (or read) -> validate -> 1 staging copy -> bake -> register */
```

Steps, in order, no work skipped and none added:

1. **Validate** (CPU, touches header + section table only): magic, version
   major match, `total_size == file size`, section offsets inside blob,
   no overlap, crc32 per section. Any failure → `false` + log with
   `(tag, offset, expected, got)`. Vertex mass is never touched on failure.
2. **Resident copy**: reserve `vert_bytes + idx_bytes` in the vertex/index
   arenas (`gpu_pool` slices, grow-only, one slice per LOD stream) and
   `N * 24 + N * 16` for the mesh tables; single `staging_pool` copy via
   `renderer_upload_buffer_to_slice` on the frame cmd buffer. One submit,
   no `WaitIdle`, no per-model upload loop (kills today's N-small-uploads).
3. **Address bake** (only ~2.5 KB): for each `SHADEMESH` row,
   `vert_addr = gpu_base + vert_arena_off + vert_off`,
   `idx_addr  = gpu_base + idx_arena_off + idx_off`, written into the
   resident `ShadeMesh[]` copy. Vertex/index bytes are never rewritten.
4. **Register**: copy `CULLMESH/SHADEMESH/LOD/CLUSTER/SKIN/MATERIAL` rows
   into the live tables, append `BATCH_ROUTE` to the batch registrar
   (game-engine-dod: renderables registered on add, never walked per frame),
   store `NAMES` in CPU debug table only.

Slow-path fallback: if `.mua` is missing or version-mismatched, the loader
invokes the cooker path (`scene3d_asset.c` today) once and writes the `.mua`
beside the source — developers never hand-edit blobs, they re-cook.

## 6. Debug tooling (debuggable is a requirement, not a wish)

```sh
muasset dump models/pet.mua              # human tables: header, sections,
                                         # cullmesh rows, batch route, budgets
muasset dump models/pet.mua --json       # regenerates the sidecar; CI diffs it
muasset validate models/pet.mua          # re-checks crc32 + offsets, nonzero exit
muasset diff a.mua b.mua                 # per-section byte/row diff after a re-cook
```

- `dump` prints row structs in shader field order with units
  (`radius_m`, `off_bytes`), never raw hex unless `--raw`.
- Every loader error names `(file, section tag, row, field, expected, got)` —
  a corrupt blob is diagnosable without a GPU capture.
- `NAMES` section exists so dumps show `mesh[3]=pet_body` instead of indices.

## 7. Measurement + acceptance

| claim | how measured | gate |
|---|---|---|
| ≥10x faster load | `cook_ms` (sidecar) vs `load_ms` (loader timer, `MU_SCOPE_TIMER`) on cubepets scene | impl-plan M1 exit |
| 1 staging copy | `GPU_SCOPE` + profiler panel: one transfer per `.mua`, zero per-model uploads | M1 exit |
| dump == sidecar | CI `muasset dump --json | diff sidecar` | every cook change |
| zero runtime branches added | batch routing is `BATCH_ROUTE` lookup; no new material-flag `if` in shaders | review checklist |

## 8. Textures stay out of the blob

Images keep their own loaders, compressed formats (`KTX2/BC7`), and atlases.
The blob stores URIs + hashes (§3); the loader resolves them through the
existing `texture_load_async` SPSC path. Rationale: textures stream and mip
independently of geometry — packing them together would force re-cooking
megabytes when a material tweaks one scalar.

## 9. Versioning

- `major` bump = breaking row layout; loader rejects older blobs with
  `need re-cook (have 1.x, want 2.0)` — never a compat shim in shaders.
- `minor` bump = additive section (new tag) or new batch id; loader ignores
  unknown tags it does not need.
- Tags never renumbered; row sizes `_Static_assert`ed in both cooker and
  loader against the same `muasset.h`.

## 10. Build order (maps to `gpu-driven-impl-plan.md`)

1. `muasset.h` (structs + tags + asserts) + `validate` — no renderer change.
2. `muasset_cook` for static meshes (VERT/IDX/CULL/SHADE/LOD/MATERIAL/ROUTE/
   NAMES) — output boots today's path via slow-path fallback.
3. Loader fast path + 1-copy upload + address bake — M1's measurable payoff.
4. Skin (`SKIN/ANIM`) + clusters (`CLUSTER`) — unlock M6/M7.
5. CI round-trip (`dump --json` diff) + ≥10x load measurement — exit gate.
