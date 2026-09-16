# Learnings from NoGraphicsAPI → mu_gfx API Redesign Plan

A study of the `NoGraphicsAPI` prototype (Vulkan 1.4 implementation of Sebastian Aaltonen's
*No Graphics API*) and a concrete plan for what mu_gfx adopts, adapts, and rejects.

---

## Part 1 — Why the current pass API hurts

Writing one pass in mu_gfx today takes ~40 lines of ceremony, repeated for every pass
(`pass_fire`, `post_pass`, `pass_smaa` ×3, `pass_ldr_to_swapchain`):

```c
rt_transition_all(r, cmd, &r->hdr_color[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
flush_barriers(r, cmd);

VkRenderingAttachmentInfo color = { .sType = ..., .imageView = ..., .loadOp = ..., ... };
VkRenderingInfo rendering = { .sType = ..., .renderArea.extent = ..., ... };

vkCmdBeginRendering(cmd, &rendering);
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->render_pipelines.pipelines[r->EnginePipelines.fire]);
vk_cmd_set_viewport_scissor(cmd, r->swapchain.extent);
vkCmdPushConstants(cmd, r->bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
vkCmdDraw(cmd, 3, 1, 0, 0);
vkCmdEndRendering(cmd);
```

Six sources of boilerplate and of bugs:

1. **Manual transitions.** Every pass author must know each target's previous layout and both
   stage masks — the exact knowledge the `ImageState` tracker already holds, duplicated by hand
   per call site. A wrong stage mask compiles fine and corrupts silently.
2. **Vulkan structs at call sites.** `VkRenderingAttachmentInfo` + `VkRenderingInfo` leak the
   backend into application code.
3. **Pipeline handle plumbing.** `r->render_pipelines.pipelines[r->EnginePipelines.fire]` — three
   nested levels to name one pipeline.
4. **Push constants via raw pointers.** `sizeof(push)` + stage mask + layout repeated everywhere;
   size/stage mismatches are possible.
5. **Viewport/scissor set by hand** after every `BeginRendering`, with the negative-height
   convention hidden in a helper.
6. **Profiler scope + pass code interleaved**, so the actual draw is buried inside plumbing.

The same story applies to declaring a pipeline: `GraphicsPipelineConfig` defaults +
`blend_alpha()` helpers + explicit format arrays for what is usually "render into this target
with default state".

## Part 2 — What NoGraphicsAPI does differently

Relevant points from `NoGraphicsAPI/include/NoGraphicsAPI/NoGraphicsAPI.hpp` and `docs/`:

- **One-call render pass.** `begin_render_pass(commands, RenderingDesc)` takes
  `Span<const ColorAttachment>` + depth/stencil attachments with load/store/clear, derives the
  render area from the first attachment, sets viewport/scissor, resets depth/stencil state, and
  pairs with a bare `end_render_pass`. No `VkRenderingInfo` ever appears outside the backend.
- **Root payload per draw.** `draw(commands, ByteSpan root, vertex_count, ...)` copies the
  per-draw argument block (`vkCmdPushDataEXT`) right before the native command. Root structs are
  declared once in a shared C++/Slang header — one source of truth for CPU and GPU layouts.
- **Layout-free PSOs + command-state split.** Rasterization/blend/format compatibility are baked
  into the PSO; viewport, scissor, and depth/stencil state are *commands*, so begin-pass resets
  them to safe defaults instead of forcing PSO permutations.
- **Global stage/access barriers.** `barrier(cmd, Stage before, Access before, Stage after, Access after)`
  — no resource lists, no layouts; normal textures live in `GENERAL` permanently
  (`VK_KHR_unified_image_layouts` makes that cheap).
- **Designated-initializer descs with real defaults.** Nearly every field has a useful default;
  call sites name only the fields that differ. `Span`/`ByteSpan`/`GpuRange` are passed by value so
  the compiler ships pointer+size in registers.
- **Timeline submission + DeleteQueue.** Every submit returns/advances a monotonic timeline
  value; the optional `DeleteQueue` is a fixed-capacity ring of `{retire_value, callback}`
  entries — `defer(value, cb)` at record time, `tick()` each frame, `drain()` after
  `wait_idle`. Deferred destruction stops being a hand-rolled per-frame ceremony, and reuse
  waits become precise (`wait_timeline(point)`) instead of device-wide.
- **GpuRange for addresses.** Index buffers, indirect args, and copies consume
  `GpuRange {gpu, size}` — the same non-owning view everywhere, passed by value.

Things mu_gfx deliberately skips (agreed scope):

- **Descriptor heaps (`VK_EXT_descriptor_heap`).** mu_gfx keeps its existing Set-0 bindless
  model (64k textures / 256 samplers / push-constant IDs). It already achieves the same
  *feel* — no per-draw binds, shaders index by uint32 — without requiring Vulkan 1.4-era
  extensions.
- **Meshlets / task+mesh shaders.** Not supported now, not planned; `draw_meshlets*` is ignored.
- **GPU pointers replacing buffer objects.** mu_gfx keeps named buffers, pools, and uploads;
  that refactor is out of scope for this pass.

## Part 3 — Adopted design (mu_gfx, C, Vulkan 1.3)

### 3.1 New pass API (the core ask)

Data-oriented descs with designated initializers; the pass function owns transitions,
rendering state, and defaults.

```c
// ---- pass declarations ----

typedef struct PassAttachment {
    TextureID    texture;     // dense ID from the existing TextureSystem
    LoadOp       load;        // LOAD_OP_DEFAULT -> from usage (clear for color/depth targets)
    StoreOp      store;       // LOAD_OP_DEFAULT -> STORE
    float        clear[4];    // color; depth clear in clear[0] for depth attachments
} PassAttachment;

typedef struct PassDesc {
    const PassAttachment *colors;      // NULL when compute-only
    uint32_t              color_count; // 0..MAX_COLOR_ATTACHMENTS
    const PassAttachment *depth;       // NULL = no depth attachment
    const PassAttachment *resolve;     // optional, parallel to colors
    PipelineID            pipeline;    // bound by begin_pass; 0 = none (compute or deferred bind)
} PassDesc;

// Transitions declared attachments into their pass layout (from ImageState), flushes
// barriers, begins rendering with derived render area, sets viewport/scissor, binds PSO.
void begin_pass(CommandBuffer *cmd, const PassDesc *desc);

// Ends rendering.
void end_pass(CommandBuffer *cmd);
```

A pass becomes:

```c
static void pass_fire(Renderer *r, VkCommandBuffer cmd) {
    GPU_SCOPE(&r->gpuprofiler[r->current_frame], cmd, "Fire Pass", ...);

    PassAttachment color = { .texture = r->hdr_color[r->swapchain.current_image].id, .load = LOAD_OP_CLEAR };
    begin_pass(cmd, &(PassDesc){ .colors = &color, .color_count = 1, .pipeline = r->EnginePipelines.fire });
    push_constants(cmd, sizeof(FirePush), &fire_push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    end_pass(cmd);
}
```

What the backend does inside `begin_pass`, in order:

1. Resolve `TextureID` → `RenderTarget` (dense table, no pointer chasing).
2. Derive the render area from the first color attachment (depth fallback, same rule
   NoGraphicsAPI uses).
3. Emit **auto-transitions** from the per-mip `ImageState` table to the attachment layout
   (`COLOR_ATTACHMENT_OPTIMAL` / `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`), assert that no two
   attachments alias, and `flush_barriers()`.
4. Fill `VkRenderingAttachmentInfo` / `VkRenderingInfo` internally (load/store/clear mapped,
   `imageLayout` supplied by the backend, never the caller).
5. Set full-area viewport/scissor (negative-height convention stays hidden inside).
6. Bind the PSO when `pipeline != 0`.
7. Reset depth/stencil dynamic state to the known default (NoGraphicsAPI's "command state
   reset" learning — removes a class of cross-pass leaks).

Compute passes use the same entry point with `color_count = 0`; no rendering scope is begun.

**Not adopted here:** NoGraphicsAPI's `LoadOp::discard` defaults and stencil-separate
attachment struct can come later; mu_gfx keeps one `PassAttachment` for color and depth since
its shaders don't currently split stencil.

### 3.2 Draws that carry their own payload (root-argument learning, adapted)

mu_gfx's fixed 256-byte push-constant layout (the `PUSH_CONSTANT` macro) is already half of
NoGraphicsAPI's root idea. The missing half is making the payload a parameter of the draw
instead of a separate `vkCmdPushConstants` call:

```c
// Wraps vkCmdPushConstants with the fixed 256-byte layout, all stages, no repeats.
void draw_push(CommandBuffer *cmd, PipelineID pipeline, const void *data, uint32_t size);
void dispatch_push(CommandBuffer *cmd, PipelineID pipeline, const void *data, uint32_t size);
```

Every helper that can (draw, draw_indexed, draw_indirect, dispatch, …) gains a `ByteSpan`
payload variant in the style of NoGraphicsAPI's `draw(commands, ByteSpan root, ...)`:

```c
void cmd_draw(CommandBuffer *cmd, PipelineID pipeline, ByteSpan root,
              uint32_t vertex_count, uint32_t instance_count);
```

The shared-struct learning applies directly: declare the push struct once in
`src/slangtypes.h`, include it from the Slang shader, and keep the `_Static_assert` size
checks. (mu_gfx stays on Vulkan 1.3 `vkCmdPushConstants`, not `vkCmdPushDataEXT`, which needs
`VK_EXT_descriptor_heap`.)

### 3.3 Pipeline declaration cleanup

Keep `GraphicsPipelineConfig` and the ID-pool `PipelineID` table, but make it obey the
"defaults + named overrides" style:

- `pipeline_config_default()` returns the zero-value struct with sane fields, so call sites use
  designated initializers naming only what differs:

  ```c
  r->EnginePipelines.fire = pipeline_create_graphics(r, &(GraphicsPipelineConfig){
      .vert_path    = "compiledshaders/fire.vert.spv",
      .frag_path    = "compiledshaders/fire.frag.spv",
      .depth_format = VK_FORMAT_D32_SFLOAT,
      .blends       = { blend_disabled() },
  });
  ```

- Drop the three `blend_alpha/additive/disabled` helpers in favor of designated initializers at
  the rare call sites that need them, or keep exactly one `blend_preset()` if they recur.
- Pipeline creation accepts the shader *paths* it already does (hot-reload rebuild stays), but
  the format arrays should be derivable from the intended render targets where possible.

### 3.4 Timeline submission + DeleteQueue (adopted now)

Replace the fence-wait + device-wide `vkDeviceWaitIdle` pattern with NoGraphicsAPI's model:

- **Timeline semaphore** (`VkSemaphoreType::TIMELINE`, one device-wide) advances monotonically
  per submission. `submit_frame` signals value N; frame contexts record the value they last
  used.
- **Frame reuse** waits on the timeline instead of `vkWaitForFences`, matching
  `MAX_FRAMES_IN_FLIGHT` semantics: frame slot k is free when the timeline covers its last
  submission.
- **DeleteQueue** — fixed-capacity ring of `{retire_value, FixedFunction-like callback}`:

  ```c
  typedef struct DeleteQueue {
      struct { uint64_t retire_value; DeleteFn *fn; void *user; } entries[DELETE_QUEUE_CAP];
      uint32_t head, tail, count;
  } DeleteQueue;

  void delete_queue_defer(DeleteQueue *q, uint64_t retire_value, DeleteFn *fn, void *user);
  void delete_queue_tick(DeleteQueue *q, uint64_t completed_value); // each frame
  void delete_queue_drain(DeleteQueue *q);                         // shutdown, after wait_idle
  ```

  Implemented as planned for **shader hot reload**: old `VkPipeline`s are deferred with the
  current timeline value, removing the `vkDeviceWaitIdle` stall on reload.

  One planned item was **adapted during implementation**: swapchain recreate cannot defer its
  render-target destruction, because `rt_resize` → `rt_destroy_internal` frees images and
  rewrites bindless descriptors immediately on re-create — deferral would leave in-flight frames
  sampling rewritten descriptors (a use-after-free the old device-wide idle was masking). The
  recreate path instead does a targeted `vkWaitSemaphores(timeline_last_submitted)`: it covers
  exactly the outstanding submissions, still avoiding the device-wide stall. Immediate-safe
  destruction (pipelines, and everything destroyed at shutdown) goes through the DeleteQueue.
- **Readback waits** (`capture_consume`, GPU profiler collection) become precise
  `vkWaitSemaphores` on the covering value instead of fences-per-frame. (Not yet done; the
  capture pipeline still consumes on fence-free timeline slots.)

Shutdown order mirrors NoGraphicsAPI: `wait_idle` → drain every `DeleteQueue` → destroy
resources → destroy device.

### 3.5 Barriers: keep auto-transitions (decided)

mu_gfx keeps the per-mip `ImageState` tracker and derives transitions from it; the
NoGraphicsAPI global-barrier/`GENERAL`-layout model is documented but not adopted, because:

- The tracker already exists, is debugged, and produces optimal layouts without
  `VK_KHR_unified_image_layouts`.
- Auto-transitions keep the *call-site* surface just as small as global barriers — the author
  names resources, not stages — which is the actual goal.
- Global barriers can over-synchronize (one dependency covers unrelated resources); the
  tracker emits narrower barriers for free.

`begin_pass` hides the tracker behind intent ("this is a color target this pass"), which is the
same ergonomics NoGraphicsAPI gets from never having layouts at all. The descs are shaped so a
future global-barrier backend could replace the transition logic without changing call sites
(the desc names resources + usage, never Vulkan stage/access masks).

**Implemented addition:** `PassAttachment` gained `swapchain_view` (used when `target == NULL`),
so swapchain-image passes — ImGui's overlay — go through `begin_pass` too. Swapchain state is
backend-owned (`image_transition_swapchain`), so those attachments skip the `rt` tracker and
derive their render area from the swapchain extent.

## Part 4 — Other improvements borrowed from NoGraphicsAPI

1. **Descs with defaults everywhere.** Extend the designated-initializer style beyond pipelines:
   `RenderTargetSpec`, upload helpers, sampler creation. Every desc field gets a useful zero
   value; call sites name only deltas. (`SamplerDesc` like NoGraphicsAPI's: filter/address modes
   default to linear/repeat.)
2. **Spans by value.** The doctrine already mandates it; audit new APIs so `Span`, `ByteSpan`,
   and any `GpuRange`-like views are passed by value (pointer+size in registers).
3. **Vulkan types out of the pass layer** (minimal scope, agreed): the new pass/draw APIs hide
   `VkRenderingInfo` and layout enums. Existing `RenderTarget`/`Buffer` structs keep their
   `VkImage`/`VkPipeline` fields for now; full opaque-ification is a later phase.
4. **Type-safe per-draw payload** — a small `ByteSpan` wrapper type so payload size/alignment is
   checked once in the helper instead of at every call.
5. **Naming.** `begin_pass`/`end_pass`, `cmd_draw`, `dispatch_push` — snake_case verbs grouped
   in one public header section, per the existing doctrine.

## Part 5 — Rejected / deferred ideas

| Idea | Verdict | Why |
|---|---|---|
| `VK_EXT_descriptor_heap` + app-owned heaps | Deferred | Requires Vulkan 1.4-era driver support; existing Set-0 bindless delivers the same ergonomics. |
| Meshlet/task+mesh PSOs | Ignored | Out of project scope. |
| GPU pointers replacing buffer objects (`GpuCpuRange<T>`) | Deferred | Large refactor; current pools/offset-allocator work fine. |
| Global stage/access barriers + permanent `GENERAL` | Rejected for now | Tracker already gives narrower barriers; revisit if unified layouts land in the tree. |
| `vkCmdPushDataEXT` root copying | Replaced | Vulkan 1.3 push constants with a `ByteSpan` helper achieve the same shape. |
| GPU-resident, stage-specific roots (blog form) | Rejected | No GPU root selection until GPU-driven rendering exists. |
| Split barriers (GPU-address signal/wait tokens) | Rejected | Not expressible in Vulkan 1.3; not needed yet. |

## Part 6 — Implementation status

1. **`begin_pass` / `end_pass`** — done. `PassDesc` with auto-transitions from `ImageState`,
   render-area derivation, viewport/scissor, PSO bind, compute-only path, and swapchain-view
   attachments. Converted call sites: `pass_fire`, `post_pass`, all three `pass_smaa` stages,
   `pass_imgui`. Remaining raw rendering scope: none outside the backend (`pass_ldr_to_swapchain`
   is a blit, not a rendering scope).
2. **Payload draws** — done. `cmd_draw(r, cmd, BYTE_SPAN(push), ...)` and
   `dispatch_push(...)` carry the per-draw payload with the work, NoGraphicsAPI-root style;
   `emit_root_data` asserts 4-byte size and the 256-byte limit. All pass call sites converted;
   raw `vkCmdPushConstants`/`vkCmdDraw`/`vkCmdDispatch` survive only inside the wrappers.
3. **Pipeline descs** — done. Single designated-initializer default config; unset blends
   auto-filled with `blend_disabled()` (preserving previous behavior); `blend_additive` removed.
4. **Timeline + DeleteQueue** — done, with the recreate-path adaptation described in Part 3.4:
   timeline signal per submit, timeline-keyed frame-slot reuse, `DeleteQueue` with
   `tick()`/`drain()`, deferred hot-reload destruction, targeted recreate wait.
5. **Sweep the descs** — open. Defaults for `RenderTargetSpec`, samplers, uploads;
   spans-by-value audit.
