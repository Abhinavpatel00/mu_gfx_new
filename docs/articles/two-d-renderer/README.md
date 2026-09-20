# Building a 2D renderer on mu_gfx — article series

An incremental, measured walkthrough of a high-throughput 2D sprite renderer built
on the API that already exists in this repository. Every API name, signature and
file reference in this series was read from the tree at
`57ea3c13c62b1bac87bd37f4daf7fbb2ef1fb239`; nothing here is aspirational API.

Each article leaves the renderer in a runnable state. You are expected to build and
run after every step; if a step does not show up in the GPU profiler UI, stop and fix
it before reading on.

---

## Who this is for

You can read C99, you know what a cache line is, and you have seen Vulkan once.
The series assumes you accept these rules, because violating any of them is what
makes 2D renderers slow:

1. **Fill rate dominates. Vertex data is cheap.** 100 000 sprites cost ~1.6 MB of
   instance traffic. Eight layers of overdraw at 1080p cost ~66 MB of framebuffer
   traffic. Both are per frame. Do the arithmetic before optimizing anything.
2. **A frame is three frames.** `MAX_FRAMES_IN_FLIGHT` is 3. Anything you write for
   frame N is still being read while you prepare frame N+1.
3. **Preparation and execution are separate phases.** Preparation (cull, transform,
   sort, batch, write byte ranges) touches N sprites once, linearly. Execution
   (recording draws) is a short, straight-line loop over a small batch array.
4. **No allocation in the frame.** Per-frame memory comes from the 32 MB
   `cpu_pool` (linear bump allocator, reset every frame) or a per-slot `Buffer`.
5. **Measure, then change, then measure.** The repo already profiles both sides:
   `TracyCZoneNC` on the CPU, `GPU_SCOPE` + `GpuProfiler` on the GPU.

---

## Ground truth: what the backend hands you

| Facility | Where | Notes |
|---|---|---|
| `begin_pass` / `end_pass` | `vk.c:2455`, `vk.c:2434` | one call declares a pass; transitions derive from `ImageState` |
| `PassDesc`, `PassAttachment`, `LoadOp`, `StoreOp` | `vk.h:397-429` | `LOAD_CLEAR`/`LOAD_DISCARD`/`STORE_DISCARD` are the bandwidth knobs |
| `cmd_draw(r, cmd, root, vertex_count, instance_count)` | `vk.c:2422` | root payload rides with the draw (push constants, ≤256 B) |
| `dispatch_push(r, cmd, root, gx, gy, gz)` | `vk.c:2428` | same root mechanism for compute |
| `PUSH_CONSTANT(Name, body)` | `common.h:37` | 256-byte, 16-byte-aligned, `_Static_assert`ed |
| `BYTE_SPAN(value)` / `ByteSpan` | `external/mu/mu/mu_span.h:342` | by-value const view, two fields |
| `MU_SPAN_OF` / `MU_SPAN_IMPL` | `external/mu/mu/mu_span.h:135` | the sanctioned way to declare a typed span over structs |
| Bindless set 0: sampled images (0), samplers (1), storage images (2), `global_data` UBO (3) | `vk.c:2955-2983` | one `vkCmdBindDescriptorSets` per frame in `renderer_frame` |
| `RenderTarget`, `rt_create`/`rt_resize`/`rt_destroy` | `vk.h:248`, `vk.c:1525` | `bindless_index` is the id your shader samples with |
| `create_texture` / `destroy_texture` | `vk.c:899`, `vk.c:3307` | `TextureID` **is** the bindless slot; ids come from `mu_id_pool` |
| `sampler_create` / `default_samplers.samplers[SAMPLER_*]` | `vk.h:281`, `vk.c:3122` | zero-value `SamplerDesc` = linear + repeat |
| `BufferPool` (LINEAR / RING / TLSF) + `buffer_pool_alloc` | `vk.h:296`, `vk.c:1312` | `BufferSlice` = `{pool, buffer, offset, size, mapped}` |
| `cpu_pool` / `gpu_pool` / `staging_pool` | `vk.c:3153-3167` | 32 MB / 512 MB / 128 MB |
| `vk_frame_acquire` / `vk_frame_submit` | `vk.c:3176`, `vk.c:3208` | slot wait, pool resets, timeline retirement |
| `delete_queue_defer` / `tick` / `drain` | `vk.c:2543-2580` | retire GPU resources on the submission timeline |
| `GPU_SCOPE(prof, cmd, name, stage)` | `src/helpers.h:624` | per-pass GPU timings + pipeline statistics |
| `pipeline_create_graphics` / `GraphicsPipelineConfig` / `blend_alpha` | `vk.c:2130`, `vk.h:333`, `vk.h:517` | returns 1-based `PipelineID` for `PassDesc.pipeline` |
| `pipeline_rebuild` + `pipeline_mark_dirty` | `vk.c:2151` | shader hot reload via `dmon` + `compileslang.sh` |

Two caveats worth knowing before you write shader code:

* `src/constant.h:9` defines `GAME_STATE_BINDING 4` and `shaders/common.slang:18`
  declares `StructuredBuffer<GameEntity> game_entities` at binding 4, but the
  descriptor set layout actually created in `vk.c:2955` has bindings 0–3 only.
  **Do not pass per-frame arrays through SSBO bindings.** Pass device addresses in
  the root payload — that is exactly what `pass_nuklear` does with
  `UiPush { UiVertex* vertices; ... }` and what this series does for sprite data.
* `texture_upload` / `texture_readback` are declared in `vk.h:619-638` but are not
  implemented in the tree. The working upload path is the one `nuklear_init`
  (`src/nuklear_renderer.inl:35-66`) uses: `create_buffer` → `memcpy` →
  `vkCmdCopyBufferToImage` → `rt_transition_all` → submit. Article 05 wraps that
  pattern into the picture system.

---

## Reference implementation layout

The repo's own convention is `renderer.c` including `.inl` passes
(`#include "src/nuklear_pass.inl"`). Stay in that convention — private state
(`Renderer`, `VkBackend`) is visible there and nowhere else.

```
src/two_d/
  sprite_data.h      CPU/GPU shared structs, included by C and Slang (like src/slangtypes.h)
  sprite.h           public API: create/destroy, begin/end, draw calls
  sprite.c           instance packing, sorting, batching, picture table
  sprite_pass.inl    recording (included by renderer.c, like nuklear_pass.inl)
  sprite_init.inl    pipelines, buffers, picture uploads (included by renderer.c)
shaders/
  sprite.slang       vertex + fragment, compiled by compileslang.sh
```

`Makefile`: add `src/two_d/sprite.c` to `SRC_C` and add a `compiledshaders/sprite.*.spv`
rule next to the `scene3d` ones (`SLANGC ?= /opt/shader-slang-bin/bin/slangc`).

---

## The budget ledger

Put these numbers in a comment at the top of `sprite.c` and keep them true.

| Quantity | Value | Arithmetic |
|---|---|---|
| Default window | 1362 × 749 | `renderer.c:826-827` |
| 1080p pixels | 2 073 600 | 1920 · 1080 |
| One full-screen RGBA8 layer | 8.29 MB written | 2 073 600 · 4 |
| One full-screen RGBA16F layer | 16.59 MB written | 2 073 600 · 8 |
| `hdr_color` set (3 images) | 49.8 MB | 16.59 · 3 |
| Sprite instance, compact | 16 B | see article 02 |
| 100 000 sprites | 1.6 MB/frame | 100 000 · 16 |
| 100 000 sprites, 3 frames in flight | 4.8 MB | 1.6 · 3 |
| 8× overdraw at 1080p RGBA8 | 66.4 MB/frame | 8.29 · 8 |
| 60 fps frame budget | 16.67 ms | 1/60 |
| Target 2D pass GPU time | ≤ 3 ms | leaves room for tonemap + SMAA |
| Target CPU preparation | ≤ 1.5 ms | sorting + packing 100 000 sprites |
| Target draw calls | ≤ 64 | one per (blend, texture) batch |

Derived bandwidth sanity check: 66.4 MB of fill at 60 fps is ~4 GB/s of framebuffer
traffic. That is the number to attack first — not the 1.6 MB of instance data.

---

## Build and run

```bash
make -j$(nproc)          # debug build, shaders compiled as a dependency
./build/app              # x11 by default; ./build/app wayland to force wayland
make release -j$(nproc)  # -O3 -march=native, the only build you benchmark
make run_asan            # address + UB sanitizer, for CPU-side packing code
```

The app opens with the GPU profiler window available (`gpu_profiler_ui_update` in
`renderer.c`). It also has a Capture window: screenshot button and a 60 fps video
recorder that reads the swapchain through the capture slots — free regression
tooling for every article in this series.

---

## Article index

| # | Article | You end with |
|---|---|---|
| 1 | [Frame ownership and the first quad](01-frame-ownership-and-the-first-quad.md) | A profiled 2D pass drawing one procedural quad into `hdr_color` |
| 2 | [The instance record format](02-instance-record-format.md) | A 24 B/sprite layout, `_Static_assert`ed, packed branch-free |
| 3 | [Ordering and batching](03-ordering-and-batching.md) | Counting sort + batch array, ≤64 draw calls for 100 000 sprites |
| 4 | [GPU-driven culling and indirect draws](04-gpu-driven-culling-and-indirect.md) | Compute visibility pass and indirect commands per batch |
| 5 | [Pictures: atlas, residency, uploads](05-pictures-atlas-residency.md) | Bindless picture table with streaming upload and safe eviction |
| 6 | [CPU-side locality](06-cpu-side-locality.md) | Preparation phase with no branches, no allocation, measured |
| 7 | [GPU-side locality and overdraw](07-gpu-side-locality-and-overdraw.md) | Quad topology, blend, LOD and fill-rate decisions that survived measurement |
| 8 | [Measurement and regression gates](08-measurement-and-regression-gates.md) | Counters, a fixed benchmark scene, thresholds that fail loudly |

---

## Where the 2D pass sits in the existing frame

`renderer_frame` (`renderer.c:1388`) records, in order:

```
poll events → pipeline_rebuild → delete_queue_tick → frame_start (acquire + slot wait)
→ update_global_data → vk_cmd_begin → bind descriptor sets (graphics + compute)
→ post_pass → pass_smaa → pass_ldr_to_swapchain → profiler/capture UI → pass_nuklear
→ capture_record → transition to PRESENT_SRC → vk_cmd_end → vk_frame_submit
```

The 2D pass belongs between `update_global_data` and `post_pass`: it draws into
`hdr_color[current_image]`, and the existing compute tonemapper, SMAA, blit,
screenshot and video-capture paths then treat your sprites like any other scene
content. Nothing in this series replaces the post chain. Everything is additive.

Note that `pass_fire` (`renderer.c:1125`) is currently unused — nothing in
`renderer_frame` writes `hdr_color` today, and `post_pass` reads it. So your sprite
pass *is* the scene pass in article 01, and it must `LOAD_CLEAR` the HDR target.

---

## Verified preconditions (checked before writing this series)

These are the facts the code in this series depends on. All were checked against the
tree, and the shader claims were checked by compiling with the repo's own
`/opt/shader-slang-bin/bin/slangc` and validating with `spirv-val`:

| Claim | Evidence |
|---|---|
| `VkPhysicalDeviceVulkan12Features.bufferDeviceAddress` is enabled | `vk.c:314` |
| `VkPhysicalDeviceVulkan11Features.shaderDrawParameters` is enabled | `vk.c:293-295` |
| `multiDrawIndirect` and `drawIndirectCount` are enabled | `vk.c:307`, `vk.c:313` |
| **`drawIndirectFirstInstance` is NOT enabled** | `apply_caps`, `vk.c:283-315` — not in the list |
| `gpu_pool` has `SHADER_DEVICE_ADDRESS_BIT` + `INDIRECT_BUFFER_BIT`, is `GPU_ONLY` | `vk.c:3159-3163` |
| `cpu_pool` is `LINEAR`, host-visible, `DEVICE_ADDRESS` | `vk.c:3153-3158` |
| Push-constant range is a single `VK_SHADER_STAGE_ALL` block of 256 bytes at offset 0 | `vk.c:3033-3048` |
| A raw device-address pointer in a push constant compiles to `PhysicalStorageBuffer` in compute **and** vertex stages and validates for Vulkan 1.3 | `slangc` + `spirv-val --target-env vulkan1.3` on a test kernel, see article 02 |
| A buffer slice's device address is `r->vk.gpu_base_addr + slice.offset` | `vk.c:3169-3171` sets `gpu_base_addr` from `gpu_pool.buffer` |

The `drawIndirectFirstInstance` gap matters and is used deliberately: every indirect
draw in article 04 carries `firstInstance = 0`, and the batch base index travels in
the root payload instead. This is not a workaround — it is also the cheaper shader
form, because the base index lives in a register instead of being added to
`SV_InstanceID` per vertex.