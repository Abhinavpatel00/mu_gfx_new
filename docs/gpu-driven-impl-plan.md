# GPU-driven DOD renderer: implementation plan

Status: plan. Normative target: `gpu-driven-dod-redesign.md` (this plan is
derived from it and must not contradict it). Frame flow: `three-d-port.md` is
**history only** — the 3D path it describes was removed and is not a base.
Doctrine: `AGENT.md` §1, skills `dod-performance` and `game-engine-dod`.
Cadence precedent: `phase-2-plan.md` (small steps, no behavior change per step,
screenshot-diff verification).

This is a **greenfield** build. There is no `scene3d`/`renderer3d` code to
migrate; the milestones create the new files named in each step. File names
below are proposed, not installed.

## Ordering rationale

1. **Measure first** (M0). Every later milestone shows a before/after number
   from the same counters. No optimization without a number.
2. **A visible baseline before any GPU work** (M1). Bring up one mesh and a
   direct indexed instanced draw first, so every subsequent milestone has a
   screenshot to diff against instead of a black frame.
3. **Residency before layouts** (M1). Slices + baked addresses are the
   foundation every table assumes. No new row format lands in a per-asset
   `VkBuffer`.
4. **Layouts before transforms** (M2). All shared rows and their
   `_Static_assert`s exist before a shader reads them; C and Slang cannot drift.
5. **T1 before T2** (M3 before M4). Instances must be resident and dirty-uploaded
   before the cull can read them.
6. **T2 alone before T3** (M4 before M5). Cull effectiveness is falsifiable with
   counters before compaction exists; the survivor list is directly observable.
7. **T3 before T5** (M5 before M6). T5 consumes the compacted command triple; do
   not draw from a half-built one.
8. **Classes one at a time** (M7). Opaque first, proved out with the M1 baseline
   still available; each new class is a new table + kernel + pipeline, never an
   `if` in an old one.
9. **Views before Hi-Z** (M8 before M9). Multi-view must be shaped before a
   per-view Hi-Z chain is added, or the chain is written for one view.
10. **Hi-Z + LOD last of the core** (M9). The 4–8× knob; it needs M0 counters to
    prove itself and M6 draws to produce the depth it reads.
11. **Clusters and amortization after** (M10, M11) — measured, independent wins
    on top of a working frame.

Every milestone keeps the application green: `make` clean, one live run (~90 s,
resize, hot reload, zero validation errors), screenshot diff against the
previous milestone unless the milestone declares a visual change.

---

## M0. Instrumentation (measure-first foundation)

**Goal.** Culling effectiveness falsifiable from inside the program. Today it
is not: there is no counter and no buffer readback.

**Files.** `vk.h`/`vk.c` (buffer readback + timer scopes), `src/three_d/scene.h`
(`SceneCounters`), later consumers in `scene.c` and the cull shader.

**Steps.**
1. `buffer_readback` mirroring `texture_readback`: a ring of host-visible
   `cpu_pool` lanes keyed on `timeline_last_submitted`, lag-2 consume, `true`
   when a fresh sample is ready. Never a stall, never a sync point.
2. `SceneCounters` GPU-written atomics (redesign §16): `submitted`,
   `culled_frustum`, `culled_hiz`, `drawn`, `lod[4]`, `draws`, `dropped`.
   Atomic adds only — no branches.
3. `GPU_SCOPE` timers placed around the (future) cull/compact/hiz/draw entries;
   profiler panel shows them by default so a screenshot is evidence.
4. HUD lines: `submitted / frustum-culled / drawn / dropped`.

**Acceptance.** HUD numbers move with the camera (looking at the sky ⇒ `drawn`
≈ 0). Screenshot of the panel is the evidence. No rendering change.

---

## M1. Residency + direct-draw baseline

**Goal.** Exactly three `VkBuffer`s; every table a `BufferSlice` from the right
pool. A visible baseline to diff against: one mesh, N instances, drawn with a
direct indexed instanced draw (no cull, no compaction). This baseline is deleted
in M6 and exists only to keep the frame non-black while the GPU path is built.

**Files.** `vk.h`/`vk.c` (pool handle + slice helpers), new
`src/three_d/scene.c` (`scene_create`/`scene_destroy`, capacities),
`shaders/scene.slang` (bring-up VS/FS, later reused), `renderer.c` frame wiring.

**Steps.**
1. Confirm the three pools exist with the right policies (redesign §14):
   `cpu_pool` LINEAR reset per frame, `staging_pool` RING freed by timeline,
   `gpu_pool` TLSF `GPU_ONLY`.
2. `scene_create(vk, SceneDesc)` carves every table in redesign §14 as one
   slice: instance table, per-class `cull_row[]`, mesh/material/lod tables,
   vertex/index arenas, per-view `survivors[]`/`vis_all[]`/`draw_all[]`/counts
   (per frame-in-flight lane). Capacity is an argument; over-capacity fails loud.
3. Bake device addresses at insert/load (`gpu_base_addr + slice.offset`). No
   per-frame base+offset adds in shaders.
4. Bring up a minimal pipeline: one mesh, hardcoded instances, direct indexed
   instanced draw (a temporary CPU path). Verify pixels.
5. Wire `scene_create`/`scene_destroy` into `renderer_resources_create` /
   shutdown; destroy after `wait_idle` + queue drain.

**Acceptance.** Zero `vkCreateBuffer` per frame (count in a debug build);
baseline screenshot captured; pool high-water HUD line added. No `wait_idle` in
the frame loop.

---

## M2. Shared layouts + asset/candidate tables

**Goal.** Every shared GPU row exists, asserted, and is filled at load. This is
the layout contract the shaders and the renderer both compile against.

**Files.** new `src/three_d/scene_shared.h` (one authoritative C/Slang
definition), `src/three_d/scene_asset.c` (loader), `scene3d`-era asset code is
not reused.

**Steps.**
1. Define and `_Static_assert` every row (redesign §2, §4, §5, §7, §11):
   `GpuInstance` (20 B), `CullMesh` (32 B), `ShadeMesh` (16 B), `LodRow` (12 B),
   `CullRow` (8 B), `GpuDraw` (32 B), `GpuMaterial` (12 B), `GpuView`,
   `ViewPush` (32 B).
2. **No chunk table, no half position** (redesign §2): `GpuInstance.pos` is
   `f32[3]`. There is nothing to chunk-size or verify.
3. Importer canonicalization, required by the 20 B row: non-uniform scale and
   shear baked into static vertices at load, negative scale resolved in the
   loader (flipping winding/culling), skinned meshes kept in coherent skin
   space. The loader never refuses a mesh for non-uniform scale; it bakes it.
4. Load-time routing: material `alpha_mode`/`uv_set` → class + vertex stream;
   index width → class split (`opaque` vs `opaque32`). Resolved once, never a
   per-frame read (redesign §4, §21).
5. `CullRow[]` per class: one row per `(instance, mesh)` that needs culling in
   that class. Written at insert (rare), read sequentially every frame.
6. `lod_row[]`, `cluster[]` may be empty (`lod_count = 1`, `cluster_count = 0`);
   the shader needs no `#ifdef`.

**Acceptance.** C and Slang read the same header and all `_Static_assert`s pass;
loader logs mesh/material/lod/candidate counts for the demo scene; baseline
M1 draw still correct (no visual change).

---

## M3. T1 — instance upload (dirty event list)

**Goal.** Quantized-TRS instance table resident and updated only on movement.
Static instances cost **0 B/frame**; the per-frame authoring loop disappears.

**Files.** `scene.c` (`scene_upload_instances`, quantize), game side
(`main.c` authoring → dirty list), `scene_shared.h` (`InstanceUpdate`).

**Steps.**
1. `scene_upload_instances(Scene*, cmd, ByteSpan updates)`: AoS at the API,
   split into `slots[N] + rows[N]` at the staging boundary (two contiguous
   copies), one staging→device copy per frame, no per-slot submission.
2. Game appends `(slot, row)` only when it writes a transform. No full-table
   diff, no per-row dirty bit, no `if (dirty)` scan.
3. Barrier: transfer-write → cull/VS storage-read, declared through `PassDesc`
   (M6), hand-written only while the pass API lacks buffer access (redesign
   §15).

**Acceptance.** Static scene ⇒ 0 B/frame instance upload (HUD). Rotating crowd
uploads only moved rows. Screenshot identical to M1.

---

## M4. T2 — cull (opaque class, frustum only)

**Goal.** The GPU owns visibility for the opaque class. The cull writes a dense
survivor list; no CPU visibility knowledge.

**Files.** `shaders/cull.slang` (`cs_main`), `scene.c` (dispatch + survivor
buffers), `scene_shared.h` (survivor key), `scene.h` (`scene_cull`).

**Steps.**
1. Cull kernel per batch, `numthreads(64,1,1)`, one thread per candidate:
   `cull_row[tid]` → `gpu_inst[slot]` → `cull_mesh[mesh]`; derive the world
   sphere; mirrored-plane frustum test (generic six-plane fallback for
   off-centre/ortho); append `key = (lod<<30)|(mesh<<16)|slot` with one
   `InterlockedAdd(survivor_count, 1)`. Both exits are the bounds guard and the
   visibility test (redesign §6).
2. Fully derived basis: `quat_to_basis`; normals are the quaternion (uniform
   scale) — no inverse-transpose anywhere.
3. Clamp `survivor_count` at `MAX_SURVIVORS`, increment `dropped` on overflow.
4. `culled_frustum` counter written by the rejection path; `submitted` from the
   candidate count.
5. Standalone cull-only debug view: draw survivors as points or a counter HUD,
   to validate before T3/T5 exist.

**Acceptance.** `submitted / culled_frustum / drawn` in HUD are correct against
an analytic camera test; `drawn ≈ 0` looking at sky; screenshot of the HUD.
The M1 direct draw is still the visible output.

---

## M5. T3 — compact (histogram → scan → emit/scatter)

**Goal.** Turn the unordered survivor list into a dense `draw_all[]` +
`vis_all[]` + GPU `draw_count`, so T5 needs no CPU knowledge.

**Files.** `shaders/compact.slang` (histogram, scan, emit, scatter entries),
`scene.c` (dispatch sizes, `G` sizing, `group_count`/`cursor` scratch),
`scene_shared.h` (`GpuDraw`).

**Steps.**
1. `G = mesh_count × lod_count` for the class. Size `group_count[G]`,
   `vis_base[G]`, `cmd_index[G]`, `cursor[G]`.
2. Histogram: clear `group_count`, then one atomic increment per survivor's
   group.
3. Scan: exclusive prefix over `group_count` → `vis_base`; over `(count > 0)` →
   `cmd_index` and `draw_count`. Single-workgroup when `G` fits the group size,
   two-level otherwise. Never hard-code the group size (redesign §7).
4. Emit: one thread per group writes `GpuDraw` from the static template + T3
   results (`instance_count`, `first_instance`) at `cmd_index[g]`.
5. Scatter: one thread per survivor writes `vis_all[vis_base[g] + local] = slot`
   with `local = InterlockedAdd(cursor[g], 1)`.
6. Barrier: T2 write → T3 read; T3 write → T5 indirect/vertex read (redesign
   §15). Verify `draw_count == command triple` consistency with a debug readback.

**Acceptance.** `draw_count`, `draw_all[].instance_count`, and `vis_all[]`
contents match a CPU reference for the demo scene (debug-only readback);
`draws` (non-empty groups) in HUD; no CPU visibility readback in the frame.
The M1 direct draw is still the visible output.

---

## M6. T5 — draw (IndirectCount, `gl_DrawID` identity)

**Goal.** One `vkCmdDrawIndexedIndirectCount` per batch, no per-draw push, no
per-draw bind. The direct M1 baseline is deleted here.

**Files.** `vk.h`/`vk.c` (enable `drawIndirectFirstInstance`; index-bind helper),
`src/three_d/scene_render.c` (pass + draw wiring), `shaders/scene.slang` (VS
reads `gl_DrawID` + `vis_all`), `scene.c` (`scene_draw`).

**Steps.**
1. **Enable `drawIndirectFirstInstance`** in device setup — it is a hard
   requirement because `firstInstance` is a base into `vis_all[]` (redesign §8).
2. Bind the class index arena once per pass (width is a class property).
3. `cmd_draw_indexed_indirect_count(cmd, draw_all, draw_count, MAX_GROUP_COUNT,
   sizeof(GpuDraw))`.
4. VS: `gl_DrawID` → `draw_all[view]` → `ShadeMesh` (mesh/material/
   `vertex_stream`/`base_vertex`), a broadcast load; `slot =
   vis_all[first_instance + SV_InstanceID]` → `gpu_inst[slot]`; vertices pulled
   by device address with `first_index`/`vertex_offset` from the command.
5. Delete the M1 direct path and the temporary CPU command arrays.

**Acceptance.** Draw-call count = batches × views in the profiler; image matches
the M6 section of the redesign (renders identically to M1 for the opaque demo);
`draws` in HUD; validation clean with synchronization validation on.

---

## M7. Remaining classes (one at a time)

**Goal.** `doubleside`, `blend`, `skin`, `opaque32` as tables + kernels +
pipelines. No `if` on membership in any existing kernel.

**Files.** `scene_shared.h` (class ids), `scene_asset.c` (router), `scene.c`
(per-class candidate tables), `shaders/cull.slang` (shared entry for
opaque/doubleside as marked; separate entries for blend/skin/32), pipelines.

**Common per-class steps.**
1. New `cull_row[]` slice sized at `scene_create`.
2. Reuse the opaque cull entry with a different output list where the kernel is
   identical (doubleside); a new entry where it is not (skin, 32).
3. New draw pipeline with fixed state (cull mode / blend / index type).
4. Load-time router fills the new class's candidates.
5. Old path stays until M0 counters prove the new class.

**Acceptance (each).** Per-class counters in HUD; screenshot clean; shader has
zero data-dependent branch on membership (audit via redesign §18); geometry
routed correctly (a two-sided decal renders with culling off, a glass prop
blends, etc.).

---

## M8. Views + shadow

**Goal.** `GpuView[]` with a runtime count and a fixed cap; shadow/reflection/
cube is one more element, not a new path. Shadow is a **view** over the caster
classes, not a class.

**Files.** `scene.h`/`scene.c` (`scene_view_set`), `scene_render.c` (per-view
passes), `shaders/cull.slang` (frustum-only entry, no Hi-Z).

**Steps.**
1. Per-view `survivors[]`/`vis_all[]`/`draw_all[]`/counts already exist as lane
   arrays (M1); fill the view constants (VP rows, viewport, near/far, LOD
   target).
2. `scene_view_set` validates and stores constants; no per-view pipeline.
3. Shadow view: same caster candidates, frustum-only cull kernel, depth-only
   draw pipeline per class.
4. Ensure the main camera's frustum fast path and the shadow view's generic
   six-plane path are both selected from the projection rows, not a flag.

**Acceptance.** One scene with a colour view + a shadow view renders; counters
split per view; screenshot diff clean for the colour view and a plausible
depth-only shadow map. No `if (is_shadow)` in the cull kernel.

---

## M9. Hi-Z (T4) + LOD (the 4–8× knob)

**Goal.** Redesign §9, §10. Occlusion and LOD select, both first-class, from
previous-frame depth. Everything before this was prerequisite.

**Files.** `shaders/hiz.slang` (pyramid build), `scene_shared.h`
(`LodRow` already there), `shaders/cull.slang` (Hi-Z test + LOD walk),
`scene.c` (per-view Hi-Z chain lifetime), `scene.h`
(`scene_set_occlusion`, `scene_disable_occlusion`, `scene_set_lod_*`).

**Steps.**
1. T4 per view: build the depth pyramid from this frame's depth after T5;
   frame-persistent chain sized at `scene_create`/resize.
2. Cull: 2×2 min-`Load` occlusion test with the one-level-finer mip correction;
   `culled_hiz` counter.
3. LOD walk using the world sphere the occlusion test already produced; select
   `lod`, pack it into the survivor key; `LodRow` supplies `first_index` /
   `index_count` per rung; `lod[]` counters.
4. `scene_disable_occlusion(frames)` for camera cuts.
5. Lock the two Hi-Z invariants (redesign §9): transparent passes never write
   depth while Hi-Z is on; no same-frame two-phase pass.

**Acceptance.** `culled_hiz` and `lod[]` move with the camera; vertex-fetch
GB/s in the profiler down materially on the demo at distance; screenshot
**changes** (that is the point) and is approved by eye.

---

## M10. Clusters (deferred, measured)

**Goal.** Cluster/cone culling without mesh shaders (redesign §10).

**Steps.**
1. Loader emits `cluster[]` tables (sphere + cone) and `CullMesh.cluster_*`.
2. Second dense dispatch over T2 survivors appends `(slot, cluster)` into the
   same batch survivor stream; T3/T5 do not change shape.
3. Cone test before the vertex stage; `clusters_cone` counter.

**Acceptance.** Cluster path improves the target scene over M9 alone; asset
format decision is made here, not earlier. Revert if the second dispatch +
sync cost exceeds the saved vertex work.

---

## M11. Static-view amortization

**Goal.** Redesign §17. Skip T2/T3 when the view and scene are unchanged.

**Steps.**
1. Track a per-lane `(view_epoch, scene_epoch)` pair.
2. If unchanged, skip T2/T3 and let T5 reuse the lane's still-valid
   `draw_all[]`/`vis_all[]`.
3. Named policy switch so the A/B is one line, not a recompile.

**Acceptance.** Still camera ⇒ cull/compact GPU time ≈ 0 in the profiler, image
unchanged; a camera move or a dirty instance forces a re-cull.

---

## M12. Cleanup + doctrine lock

1. Delete every superseded path (direct baseline, temporary CPU command arrays,
   any legacy 3D scaffolding).
2. `gpu-driven-api-design.md` and `three-d-port.md` marked history;
   `gpu-driven-dod-redesign.md` is the sole normative reference.
3. Enforce the redesign §18 checks in CI: every shared size `_Static_assert`ed;
   the no-branch audit over cull/VS/FS with the whitelist; one shared header
   read by C and Slang.
4. Run the redesign §19 review checklist end to end.

---

## What stays untouched (deliberately)

- `CameraMode` enum — CPU-side mode integrator, two call sites, never in a hot
  loop. Doctrine targets hot-loop branches, not boot config.
- `GameHooks` + `render()` boundary — a system boundary; validate there, do not
  flatten it.
- Pass API shape (`begin_pass(PassDesc)`) — **extended** with `BufferAccess`
  (redesign §15), never replaced.
- 2D/sprite stage — separate stage, separate doc (`two-d-renderer.md`).
- Sampler/descriptor defaults — independent, land whenever.

## Done means

`make` green, live run + resize + hot reload with zero validation errors,
screenshot diff clean except where a milestone declares a visual change, every
milestone with a before/after number from M0 counters, `wait_idle` absent from
the frame loop, and no per-row membership flag left in any hot struct (audit
every struct against redesign §21: each former flag must point at its
replacement).
