# 3D port — one pass, GPU-driven

Source: `fukuna_engine/main.c` (3012 lines) + `fukuna_engine/shaders/gltf_minimal.slang`
+ `fukuna_engine/shaders/compute_skinning.slang`.
Short overview: `fukuna_engine/docs/3D_RENDERER_MAIN_C.md` (accurate for data
shapes, stale on flow — it describes CPU-built indirect commands uploaded per
frame; that part is not ported).
Destination: this repo (`vk.h`/`vk.c`, `renderer.c`/`renderer.h`, `main.c`,
`src/scene3d_shared.h`, `shaders/scene3d.slang`, `src/packed_vertex.c`).

## 0. One-pass rules

One landing, no phases, no compat flags, no `USE_GPU_CULL` ifdefs.

1. Copy `fukuna_engine/main.c` asset/scene/animation code verbatim where it is
   load-time or scene-authoring truth, adapt only where the backend API
   differs (buffer alloc, upload, push constants — table in section 1).
2. Delete what the GPU now owns: `build_indirect_commands`
   (`fukuna_engine/main.c:2703-2743`),
   `rendering_system_prepare_indirect_draws` (`:2745-2772`), per-frame
   `gpu_scene_release` + re-alloc (`:2515-2526`, `:2536`),
   `frame_build_gpu_instances` memcpy pass (`:2462-2489`). None of these land
   in the new tree, not even temporarily.
3. What lands on day one is the steady state in section 2: persistent asset
   buffers, triple-buffered frame buffers, skinning dispatch, cull dispatch
   that writes `instanceCount` + `visible[]` directly, single
   `vkCmdDrawIndexedIndirect` per batch group. No `Count` variant, no
   per-instance commands (rationale in section 2).
4. Scope is cut by content, not by architecture: one model format (glTF/GLB,
   triangles only), LOD0 only, CPU palette eval, no shadows, no occlusion, no
   texture streaming. The pipeline is final; only content coverage grows later.


## 1. Source-to-destination delta (the only adaptation work)

### 1.1 Backend API map — mechanical renames

| fukuna idiom | mu_gfx equivalent | Notes |
|---|---|---|
| `buffer_pool_alloc(gpu_pool)` + `renderer_upload_buffer_to_slice(r, cmd, dst, data, bytes, align)` (`fukuna_engine/vk.h:526-540`, used `main.c:1174`, `:2567-2575`) | `renderer_upload_buffer_to_slice(VkBackend *r, VkCommandBuffer cmd, BufferSlice dst, ByteSpan data)` (`vk.h:584`) for persistent buffers; `renderer_upload_buffer(r, cmd, BYTE_SPAN(x), align)` (`vk.c:1412`) for one-shot frame uploads | mu_gfx takes `ByteSpan` and drops both alignment args; staging alignment is internal (256). |
| `renderer_upload_draws` wrapping `renderer_upload_buffer(r, cmd, draws, bytes, 16, 16)` (`main.c:1257-1272`) | `renderer_upload_buffer(&vk, cmd, BYTE_SPAN, 16)` | Same, fewer args. Draw buffer becomes persistent (section 5), not per-frame. |
| `slice_device_address(renderer, slice)` (`fukuna_engine/renderer.h:31`) | `gpu_base_addr + slice.offset`, or `Buffer.address + offset` for `create_device_buffer` (`vk.c:1462-1483`) | mu_gfx `BufferSlice` has no address helper; `Buffer` does. Asset buffers use `Buffer` + `.address`. Frame slices from `gpu_pool` use `r->vk.gpu_base_addr + slice.offset` (pool base fixed, `vk.c:3256-3258`). Compute once per frame. |
| `vk_begin_one_time_cmd / vk_end_one_time_cmd` (`main.c:1222`, `:2577`) | same names, first arg is `devc.device` in mu_gfx (`src/two_d/picture.c:225`) | Load-time uploads only. Frame work records into the frame `cmd`. |
| `PUSH_CONSTANT(MeshPC, ...)` + raw `vkCmdPushConstants(..., VK_SHADER_STAGE_ALL, ...)` (`main.c:2594-2606`, `:2801`) | `PUSH_CONSTANT(name, BODY)` (`common.h:37-52`, asserts `sizeof == 256`) + `push_constants / dispatch_push / cmd_draw_*` (`vk.c:2434-2504`) taking `BYTE_SPAN(payload)` | mu_gfx payloads are always 256 B padded; fukuna `MeshPC` (136 B) and `SkinningPC` (24 B) must be redeclared via the macro so CPU/Slang layouts cannot drift. Never call raw `vkCmd*` for root payloads. |
| `pipeline_get(pipelines.skinning)` / `pipelines.gltf_minimal` (`main.c:2658`, `:2797`) | `PipelineID` + `vk->render_pipelines.pipelines[id-1]` (`src/two_d/sprite_pass.inl:63`) | 1-based IDs. Create with `pipeline_create_compute / pipeline_create_graphics` (`vk.h:598-599`). |
| `vkCmdDrawIndirect` over CPU commands (`main.c:2803`) | `cmd_draw_indexed_indirect(&vk, cmd, BYTE_SPAN(push), indirect, count, stride)` (`vk.h:628`) | Indexed from day one (fukuna shader fetches `indices[]` manually but issues non-indexed draws — that mismatch is a port bug if copied; section 4). |
| inline `VkMemoryBarrier2` (`main.c:2624-2638`, `:2676-2690`) | `cmd_buffer_barrier(cmd, buffer, offset, size, src_stage, src_access, dst_stage, dst_access)` (`vk.h:643`) | Same sync2 underneath; call sites name intent, not structs. |
| `frame_start(&renderer, &cam)` + raw `VkRenderingInfo` (`main.c:2919`, `:2983`) | `frame_start(r)` (`renderer.c:990`) + `begin_pass(&vk, cmd, &PassDesc{...})` (`vk.h:635`) | mu_gfx owns transitions via `ImageState`; never write raw `VkRenderingInfo`. |
| `mu_bulk_storage / mu_sparse_set / mu_hash32` | `external/mu/mu.h` (same library, vendored at `external/mu`) | Copy scene/asset container usage verbatim. No reimplementation. |

### 1.2 Feature caps — verified present, one verified absent

mu_gfx `vk.c:236-260` requests and `:293-314` enables: `multiDrawIndirect`,
`drawIndirectCount`, `bufferDeviceAddress`, `shaderDrawParameters`
(unconditional, v1.1). `drawIndirectFirstInstance` is not requested anywhere —
assume absent, same as the 2D renderer. Consequence: every indirect command
carries `firstInstance = 0`; the instance base travels in the push payload and
the vertex shader indexes `visible[base + iid]`. This is already the shape of
`shaders/scene3d.slang:29-31` (`pc.visible[base_instance + instance]`); keep it.
Do not port fukuna's `firstInstance = i` (`main.c:2738`) — invalid here, and
the reason fukuna's draw path needs rework, not copying.

## 2. Steady-state pipeline (lands whole)

Per frame, on the frame `cmd`:

```text
game writes RenderInstance TRS (dirty-flagged)
  -> upload compact instance records (mat3x4 rows + bounds + draw/material)
  -> skinning dispatch (per skin job x chunks of 64 verts)
  -> barrier: CS_WRITE -> CS_READ + VS_READ
  -> cull dispatch (1 thread per candidate, frustum vs world bounds)
       writes commands[batch].instanceCount (zeroed on upload)
       writes visible[commands[batch].firstInstance + slot] (atomic slot)
  -> barrier: CS_WRITE -> DRAW_INDIRECT_READ + VS_READ
  -> begin_pass (depth + hdr_color[current_image], LOAD_CLEAR)
  -> one cmd_draw_indexed_indirect per batch group, root = ScenePush
  -> end_pass -> existing post_pass / smaa / ldr_to_swapchain (untouched)
```

Why this shape and not fukuna's:

- One command per batch group (static group, skinned group — a handful per
  model), not one per instance. Fukuna emits one `VkDrawIndirectCommand` per
  frame instance (`main.c:2722-2742`) and draws them all (`:2803-2804`); at
  10k instances that is 10k commands rebuilt and uploaded on the CPU every
  frame, plus `firstInstance = i` per command, which this backend cannot
  honor (section 1.2). A batch-group command has `firstInstance = 0`; the
  vertex shader resolves `visible[push.base + iid]` from GPU-written
  `visible[]` — the pattern `shaders/scene3d.slang:18-31` already
  implements. The cull kernel owns `instanceCount`, so the CPU never knows
  the visible count after upload-time zeroing.
- No `IndirectCount` variant. The count lives in `commands[b].instanceCount`,
  written by the same dispatch that writes `visible[]`; the draw consumes it
  directly. A separate count buffer adds a buffer, a barrier, and a second
  thing to get wrong for zero benefit here. Keep `drawIndirectCount` enabled
  in caps for later; do not use it now.
- Skinning stays a separate dispatch before culling, not fused: skin jobs
  are `(job_count, max_chunks, 1)` over vertices, cull is 1D over
  candidates. One grid cannot serve both without a branch in every thread.


## 3. Data: copied, changed, deleted

Copied verbatim (load-time truth, no backend coupling):

- `PackedVertex` / `PackedSkinVertex` 16 B / 32 B + packers
  (`fukuna_engine/main.c:184-283`) -> `src/packed_vertex.c` (exists; diff
  and overwrite). `shaders/scene3d.slang` unpack must match these bit
  layouts exactly; if the diff shows drift, the shader is wrong.
- glTF importer `asset_build_from_gltf` (`:1275-2102`): passes 1-2 (count,
  fill), materials, skins/joints, nodes, clips/samplers/channels. Only the
  sink changes: `geometry_gpu_upload` targets `Buffer`, not `BufferSlice`
  (section 5).
- Animation eval `model_asset_evaluate_animation` (`:988-1047`): CPU node
  local -> global -> palette, upload palette before skinning. Keep on CPU:
  palettes are joints (tens), not instances (thousands).
- Scene API: `scene_spawn / scene_destroy / scene_write / scene_read`
  (`:709-830`), `build_model_matrix` (`:833-869`). Gameplay contract
  unchanged.
- Camera `camera_defaults_3d` (`fukuna_engine/vk.h:888-915`): port the
  header block; mu_gfx `GlobalData` currently writes identity view/proj
  (`renderer.c:1043-1080`) and must be fed real matrices.

Changed (same role, backend-shaped Buffers):

- `GpuDraw` (`:323-332`), `Mesh` bounds (`:346-358`), `Material`
  (`:360-373`), `GeometryGpu` (`:450-460`): identical fields, every
  `BufferSlice` becomes `Buffer` + `.address` (known at create,
  `vk.c:1462-1483`). Created once with `STORAGE | INDEX | INDIRECT |
  TRANSFER_DST | DEVICE_ADDRESS`, never reallocated.
- `FrameInstance` (`:557-567`, 96+ B with full `mat4`) never lands.
  Replaced by a compact GPU record: 3 matrix rows (48 B) + bounds (16 B) +
  draw/material/flags (12 B). `frame_build_gpu_instances` (`:2462-2489`)
  is deleted with it — the game writes compact records in the dirty walk.
- `SkinJob` (`:569-587`): same fields, addresses from
  `Buffer.address + offset`, resolved once, not per job.
- `GpuScene` (`:598-606`): `instance_buffer / skin_job_buffer` become
  triple-buffered `Buffer`s (one per `MAX_FRAMES_IN_FLIGHT`, worst-case
  sized, reused — the 2D `stream`/`out` pattern,
  `src/two_d/sprite_init.inl:74-85`); `indirect_cmd_buffer` becomes one
  persistent `Buffer` of batch-group commands; `indirect_count_buffer` is
  deleted; add `visible_buffer` (`uint` per instance slot, GPU-written).

Deleted (CPU owns nothing per-frame but dirty uploads):

- `frame_emit_draws` (`:2387-2409`), `build_indirect_commands`,
  `rendering_system_prepare_indirect_draws`, per-frame `gpu_scene_release`,
  `render_frame`'s CPU-command draw (`:2775-2805`). Per-frame CPU walk is:
  dirty transforms -> compact records + skin jobs -> two uploads -> two
  dispatches -> draws. No frame packet arrays, no command array, no
  `array_free` in the frame.


## 4. Shaders: three files, two already exist

- `shaders/scene3d.slang` exists. Its cull entry (`cs_main`, `:4-21`) is the
  port target with one fix, not a rewrite: the bounds test must use
  world-space center, but fukuna's CPU path never transforms `mesh->center`
  (`main.c:2397-2400` stores local) and the shader reads
  `pc.bounds[candidate.instance]` raw (`:10`). Upload world bounds (one
  `mat4 * vec4` per instance on CPU, cheap) and keep the shader as-is.
  `commands[].firstInstance` / `instanceCount` layout must match `SceneDraw`
  in `src/scene3d_shared.h:37-43` exactly. Vertex entry (`:28-52`) already
  does `visible[base + iid] -> rows -> world -> clip` with cofactor normals;
  keep. Fragment (`:53-57`) is lambert placeholder — keep for bring-up; PBR
  material fetch (`gltf_minimal.slang`) ports later as content, not now.
- `fukuna_engine/shaders/compute_skinning.slang` ports to
  `shaders/skinning.slang` with renames only: `Push` becomes
  `PUSH_CONSTANT(SkinningPush, ...)` 256 B, addresses from section 1.1. Keep
  the grid: dispatch `(job_count, max_chunks, 1)` with 64-thread groups
  (`main.c:2674`); `groupID.x = job`, `groupID.y = chunk`. The
  past-`vertex_count` early-out (`:169`) stays.
- `fukuna_engine/shaders/gltf_minimal.slang` does NOT port. Its vertex
  shader manually fetches `indices[]` then issues non-indexed
  `vkCmdDrawIndirect` — only works by accident of `firstInstance = i`
  routing. The new draw is indexed (`cmd_draw_indexed_indirect`), index
  buffer bound for real, shader reads `vertices[indices[vertex]]`.
  Deliberate break, not a port.
- `compileslang.sh`: the loop already handles `cs_main` plus extra entries
  (`:60-66`); a second compute entry in one file needs one `.spv` per entry,
  same treatment. `scene3d.comp.spv` + `.vert.spv` + `.frag.spv` and
  `skinning.comp.spv` must all build.


## 5. Buffers and frame wiring (where things live)

- `Renderer` (`renderer.c:66-114`) gains a `Scene3d` struct next to
  `SpriteSystem sprites` (`renderer.c:98`): asset table (models, one `Buffer`
  set each), triple-buffered frame `Buffer`s (instances, skin jobs,
  visible), one persistent indirect `Buffer`, `PipelineID`s (`scene3d`
  graphics + `cull`/`skinning` compute). Same lifetime as sprites: create
  once in `renderer_resources_create`, destroy after `wait_idle` + queue
  drain at shutdown; frame buffers reused by slot (`current_frame`), never
  freed per frame.
- Sizes are worst-case constants: instances = `MAX * 64 B`, visible =
  `MAX * 4 B`, skin jobs = `MAX_SKIN_JOBS * sizeof(SkinJob)`, indirect =
  `MAX_BATCH_GROUPS * sizeof(VkDrawIndexedIndirectCommand)`. Starting
  point: 65536 instances / 256 skin jobs / 64 batch groups as `#define` in
  the new `scene3d.h`; raise on measurement. (262144 is the 2D cap; too
  many for skinned 3D.)
- `ScenePush` (`src/scene3d_shared.h:44-55`) must fit 256 B: 6 addresses +
  counts + 4 clip rows + sun is near the limit; verify with `_Static_assert`
  after adding `visible_base` / `batch_count`. Clip rows come from the real
  camera: `update_global_data` (`renderer.c:1043`) currently writes identity
  view/proj and must write real matrices.
- `begin_pass` (`vk.h:635`) with depth (`COMPARE_OP_GREATER`,
  `pipeline_config_default`, `vk.h:513-524`) + `hdr_color[current_image]`,
  `LOAD_CLEAR` both. Draws sit between `pass_sprites` and `post_pass` in
  `renderer_frame` (`renderer.c:1530-1531`) or behind a flag while bringing
  up — order vs. sprites is content, not pipeline.
- External deps: `cgltf` (`fukuna_engine/external/cgltf/cgltf.h`, impl in
  `fukuna_engine/ext.c:15-17` — commented out in this repo's `ext.c:16-18`,
  restore it), `meshoptimizer` + `stb_image` + `cglm` already present.
  `Makefile` `SRC_C` gains the new `src/three_d/*.c`; no new libraries.

## 6. Landing checklist (single review, in order)

1. `cgltf` restored in `ext.c`, `src/three_d/` + `shaders/skinning.slang`
   added, `compileslang.sh` emits 4 spvs, `scene3d_shared.h` asserts pass.
2. One glTF model loads: mesh/material/draw counts logged, asset `Buffer`s
   filled, `draw_buffer` persistent. Size-check before any scene or frame
   loop.
3. One instance draws: hardcode one compact record + one batch-group command
   (`instanceCount = 1`), indexed draw, lambert fragment. Pixels beat
   architecture.
4. Skinning dispatch moves verts (checksum or visual). Palette upload
   barrier present (`TRANSFER -> COMPUTE`, cf. `main.c:2615-2639`).
5. Cull dispatch: park the camera so half the instances are behind;
   GPU-written `instanceCount` drops, CPU draw count untouched. Read back a
   `last_visible` counter for HUD only, never stall.
6. Full frame: dirty-walk uploads, both dispatches, batch-group draws,
   `begin_pass`/`end_pass`, post chain untouched. Validation clean
   (`VALIDATION true`, `src/constant.h:37`); no `vkCmdCopyBuffer` inside a
   pass (the bug `GPU_DRIVEN_RENDERING_PLAN.md` documents — do not
   reintroduce it).
7. After landing, this file shrinks to "current design" (same treatment as
   `docs/two-d-renderer.md`); `3D_RENDERER_MAIN_C.md` is not referenced
   again.

## 7. Explicit non-goals (out of scope, not deferred phases)

No shadow pass, no occlusion culling, no LOD selection, no texture streaming
or eviction (load-time `mip_count = 1` like sprites), no PBR beyond lambert
bring-up, no multi-model batching beyond per-model batch groups, no animation
compression, no `drawIndirectFirstInstance` reliance ever. Each would be a new
design doc, not a continuation of this port.
