# Vulkan Renderer Design & Architecture Reference

Status: maintained renderer reference, but not fully synchronized with source.
The application now uses RGFW and Nuklear; GLFW/cimgui/ImGui names in older
sections and diagrams below are historical. The shared CPU-pool reset described
below is not a guarantee that new GPU-readable frame data is safe across frames
in flight. Audit upload ownership before adding consumers. The old sibling
[rendererdesign.md](../rendererdesign.md) is a historical snapshot.

For the proposed static 3D API, legacy renderer review, GPU lifetime requirements,
and game roadmap, see the [Cozy Builder plan](../cozy-builder-plan.md). That plan
is a specification, not an implemented game API.

This document provides a comprehensive architectural breakdown of the Vulkan rendering engine. It is designed to quickly familiarize new developers and AI assistants with the design principles, codebase structure, memory layout, frame flow, bindless resource model, shader hot-reloading system, and extension patterns.

---

## 1. System Overview & Technology Stack

The renderer is a modern, lightweight, high-performance Vulkan 1.3 rendering engine written in C99 / C++17.

### Core Libraries & Dependencies
* **Dynamic Vulkan Loading**: `volk` (`volk.h`) for zero-overhead dynamic Vulkan function dispatch.
* **Memory Management**: Vulkan Memory Allocator (`VmaAllocator` / `vk_mem_alloc.h`) for GPU buffer and image allocations.
* **Math Library**: `cglm` (`cglm.h`) for vector and matrix operations (with `CGLM_ALL_UNALIGNED`).
* **UI & Debugging**: `cimgui` (C bindings for Dear ImGui) integrated via Vulkan dynamic rendering.
* **Shader Compiler**: Slang (`slangc`) compiled to SPIR-V via `compileslang.sh`.
* **Hot Reloading**: `dmon` (`dmon.h`) for OS-level file system watching of `shaders/` and `compiledshaders/`.
* **Profiling**: CPU profiling via `Tracy` (`TracyC.h`) and `mu_perf` micro-timers; GPU profiling via custom query pools (`GpuProfiler`).
* **ID Allocation & Pool Management**: `mu_id_pool` and `offset_allocator` (`mu.h`, `offset_allocator.h`).

---

## 2. Core Architectural Principles

### 1. Dynamic Rendering (No Render Passes / Framebuffers)
The renderer exclusively uses Vulkan 1.3 **Dynamic Rendering** (`vkCmdBeginRendering` / `vkCmdEndRendering`). Legacy `VkRenderPass` and `VkFramebuffer` objects are **not** used anywhere in this engine.

### 2. Fully Bindless Descriptor Model
* **Single Global Descriptor Set**: All textures, samplers, and storage images are bound once per frame at Set 0:
  * **Binding 0**: `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` (`MAX_BINDLESS_TEXTURES = 65536`)
  * **Binding 1**: `VK_DESCRIPTOR_TYPE_SAMPLER` (`MAX_BINDLESS_SAMPLERS = 256`)
  * **Binding 2**: `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` (`16384`)
* **No Per-Draw Descriptor Binds**: Draw calls do not rebind descriptor sets. Shaders select resources dynamically using uint32 IDs passed via **Push Constants** or **Uniform Buffers**.

### 3. Fixed 256-Byte Push Constant Layout
All push constants across vertex, fragment, and compute shaders use a uniform 256-byte memory footprint defined via the `PUSH_CONSTANT` macro in `common.h`. This guarantees pipeline layout compatibility across all engine pipelines.

```c
PUSH_CONSTANT(MyPassPush,
    uint32_t src_texture_id;
    uint32_t output_image_id;
    uint32_t sampler_id;
    float    param;
);
```

The payload is a parameter of the work itself: `cmd_draw(r, cmd, BYTE_SPAN(push), vertex_count, instance_count)` and `dispatch_push(r, cmd, BYTE_SPAN(push), gx, gy, gz)` copy the root data with the draw/dispatch and validate size/alignment in one place. Raw `vkCmdPushConstants` is backend-only.

### 4. Synchronization2 & Explicit Barrier Batching
State tracking (`ImageState`) tracks layout, stage, access flags, and queue family per image/mip. Barriers are queued into a `BarrierBatch` and flushed in batch (`flush_barriers()`) before pipeline execution to minimize synchronization overhead.

### 5. Declarative Pass API (`begin_pass` / `end_pass`)
Passes are declared, not hand-assembled. A pass describes *intent* — color/depth attachments (with load/store/clear), shader reads, storage writes, and the pipeline — through a `PassDesc`, and `begin_pass()` does everything else:

* Resolves `RenderTarget`s and derives transitions from the `ImageState` tracker (attachment layouts, sampled-read, GENERAL for storage) — call sites never name layouts or stage masks.
* Flushes the barrier batch once.
* Fills `VkRenderingInfo` internally and derives the render area from the first color attachment (depth fallback; swapchain attachments take the swapchain extent).
* Sets full-area viewport/scissor (negative-height convention stays hidden inside).
* Binds the PSO when `desc.pipeline` is set (0 = caller binds later, e.g. ImGui).
* Compute passes set `color_count = 0`; no rendering scope is begun.

`PassAttachment` uses zero-value defaults (`LOAD`/`STORE`), so call sites name only deltas. Setting `swapchain_view` instead of `target` renders into a swapchain image; the backend tracks swapchain state itself (`pass_imgui` uses this).

### 6. Submission Timeline & Deferred Destruction
A device-wide **timeline semaphore** advances monotonically on every submission (`timeline_last_submitted`). All reuse and lifetime decisions key on it:

* **Frame-slot reuse**: `frame_start` waits on the slot's `FrameContext::timeline_value` via `vkWaitSemaphores` instead of fences. The per-frame fence stays armed purely as a submission-failure trap (re-armed every frame).
* **DeleteQueue**: fixed-capacity ring of `{retire_value, callback, user}` entries. Resources destroyed while the GPU may still use them are retired with the covering timeline value; `delete_queue_tick()` runs due callbacks each frame, `delete_queue_drain()` runs everything after `wait_idle` at shutdown.
* **Shader hot reload** (`pipeline_rebuild`) defers old `VkPipeline` destruction on the timeline — no `vkDeviceWaitIdle` stall on reload.
* **Swapchain recreate** does a targeted `vkWaitSemaphores(timeline_last_submitted)` instead of a device-wide idle (required: resize destroys/recreates render targets and rewrites bindless descriptors).

---

## 3. Directory & File Organization

```
.
├── main.c                 # Primary renderer implementation, engine state, frame loop, passes
├── common.h               # Core definitions, macros (PUSH_CONSTANT, VK_CHECK), includes
├── compileslang.sh        # Bash script invoking slangc for shader compilation
├── Makefile               # Build system supporting debug, release, and ASAN target
├── docs/
│   ├── rendererdesign.md  # Architecture specification
│   ├── texture.md         # Texture subsystem design & async upload specification
│   └── convention.md      # General conventions (Vulkan coordinate system)
├── shaders/               # Source Slang shaders (.slang)
│   ├── fire.slang         # Procedural graphics pass
│   ├── postprocess.slang  # Tone mapping & post-processing compute pass
│   ├── smaa_edge.slang    # SMAA Edge Detection pass
│   ├── smaa_weight.slang  # SMAA Blending Weight Calculation pass
│   ├── smaa_blend.slang   # SMAA Neighborhood Blending pass
│   └── tonemapping.slang  # Color space conversion utilities
├── compiledshaders/       # Auto-generated SPIR-V output (.spv)
└── src/
    ├── constant.h         # System constants (max limits, array bounds)
    ├── helpers.h          # Vulkan object creation helpers & GpuProfiler implementation
    └── slangtypes.h       # Shared CPU/GPU type definitions (GlobalData, vectors, matrices)
```

---

## 4. Primary Data Structures & Engine State

The primary engine state lives inside the global heap-allocated `Renderer` struct (`g_renderer`):

```c
typedef struct Renderer {
    // Timings
    double cpu_frame_ns, cpu_active_ns, cpu_wait_ns, cpu_wait_accum_ns;
    uint32_t current_frame; // 0..MAX_FRAMES_IN_FLIGHT - 1
    float dt;

    // Per-frame contexts (each carries its last timeline_value for reuse waits)
    FrameContext frames[MAX_FRAMES_IN_FLIGHT];

    // Swapchain & Device
    FlowSwapchain swapchain;
    InstanceContext instance;
    DeviceContext devc;
    GLFWwindow *window;
    DeviceInfo info;

    // Subsystems
    TextureSystem texture_system;
    Bindless bindless_system;
    DefaultSamplerTable default_samplers;
    RendererPipelines render_pipelines;

    // Render Targets (triple-buffered per swapchain image)
    RenderTarget depth[MAX_SWAPCHAIN_IMAGES];
    RenderTarget hdr_color[MAX_SWAPCHAIN_IMAGES];
    RenderTarget ldr_color[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_final[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_edges[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_weights[MAX_SWAPCHAIN_IMAGES];

    // Static Textures
    TextureID dummy_texture;
    TextureID smaa_area_tex;
    TextureID smaa_search_tex;

    // GPU Profiling & Pools
    GpuProfiler gpuprofiler[MAX_FRAMES_IN_FLIGHT];
    BufferPool cpu_pool;     // Linear bump allocator (reset every frame)
    BufferPool gpu_pool;     // TLSF offset_allocator for device memory
    BufferPool staging_pool; // Ring buffer allocator for uploads

    // Pipeline handles
    struct { ... } EnginePipelines;

    // Submission timeline & deferred destruction
    VkSemaphore timeline;              // signalled once per submit, monotonic
    uint64_t    timeline_last_submitted;
    DeleteQueue delete_queue;          // {retire_value, callback} ring
} Renderer;
```

---

## 5. Memory Allocation Architecture (`BufferPool`)

The engine uses three specialized memory allocation strategies built on top of `BufferPool`:

1. **`BUFFER_POOL_LINEAR` (`cpu_pool`, 32 MB)**:
   * CPU host-visible memory.
   * Reset every frame in `frame_start()` via `buffer_pool_linear_reset()`.
   * Ideal for dynamic uniform buffers, per-frame push parameters, and transient frame data.

2. **`BUFFER_POOL_RING` (`staging_pool`, 128 MB)**:
   * CPU host-visible ring buffer.
   * Tail advances as fences complete in frame sync (`buffer_pool_ring_free_to()`).
   * Used for streaming CPU-to-GPU data uploads.

3. **`BUFFER_POOL_TLSF` (`gpu_pool`, 512 MB)**:
   * Two-Level Segregated Fit allocator managed via `offset_allocator` / `mu`.
   * Device-local VRAM for long-lived vertex, index, mesh, and storage buffers.

---

## 6. Frame Lifecycle & Pass Pipeline

Each frame executes through a strictly ordered pipeline in `main.c`:

```mermaid
classDef pass fill:#2d4a2d,stroke:#7fbf7f,color:#fff;
graph TD
    A[glfwPollEvents] --> B[pipeline_rebuild + delete_queue_tick]
    B --> C[frame_start: wait frame-slot timeline value]
    C --> D[Acquire Swapchain Image]
    D --> E[imgui_begin_frame + bind Set 0]
    E --> G[pass_fire: begin_pass HDR render]
    G --> H[post_pass: begin_pass compute tonemap]
    H --> I[pass_smaa: 3x begin_pass sub-passes]
    I --> J[pass_ldr_to_swapchain: blit]
    J --> K[UI + pass_imgui: begin_pass swapchain attachment]
    K --> L[Present transition]
    L --> M[submit_frame: signal timeline N+1 + present]
```

### Detailed Pass Descriptions

1. **`pass_fire` (Graphics Pass)**:
   * `begin_pass` with one clear color attachment (`hdr_color`); transitions, rendering scope, viewport/scissor, and pipeline bind are all derived.
   * Executes procedural rasterization shader (`fire.slang`) outputting HDR floating-point color.

2. **`post_pass` (Compute Pass)**:
   * `begin_pass` with `shader_reads = {hdr_color}` and `shader_writes = {ldr_color}`; transitions to `SHADER_READ_ONLY_OPTIMAL` / `GENERAL` are derived, no rendering scope.
   * Dispatches compute shader (`postprocess.slang`) performing exposure tone mapping and color grading from HDR sampled image to LDR storage image.

3. **`pass_smaa` (3-Stage Anti-Aliasing Pass)**:
   * Each stage is one `begin_pass` with a clear color attachment plus declared shader reads.
   * **Stage 1 (`smaa_edge`)**: Renders LDR color into edge target `smaa_edges`.
   * **Stage 2 (`smaa_weight`)**: Evaluates edges alongside precalculated `smaa_area_tex` and `smaa_search_tex` LUTs to write blending weights into `smaa_weights`.
   * **Stage 3 (`smaa_blend`)**: Blends original LDR image with neighbor pixels based on calculated weights, outputting to `smaa_final`.

4. **`pass_ldr_to_swapchain` (Transfer Blit)**:
   * Uses `vkCmdBlitImage` to copy `smaa_final` into the acquired swapchain image.

5. **`pass_imgui` (Overlay Pass)**:
   * `begin_pass` with a swapchain attachment (`swapchain_view`, load op `LOAD`), pipeline 0 — ImGui binds its own pipeline.
   * Uses ImGui Vulkan backend (`ImGui_ImplVulkan_RenderDrawData`) to render profiler windows, UI controls, and text directly onto the swapchain image before presentation.

---

## 7. Dynamic GPU Profiling System

The profiler (`GpuProfiler` in `src/helpers.h`) tracks nanosecond-accurate GPU pass durations and hardware statistics:

* **Scope Macro**: Passes are timed using the RAII-style `GPU_SCOPE` macro:
  ```c
  GPU_SCOPE(frame_prof, cmd, "Pass Name", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
      // Pass command recording
  }
  ```
* **Metrics Tracked**:
  * Pass execution time (milliseconds / microseconds).
  * Rolling averages (EMA), minimum and maximum pass times.
  * Vertex shader invocations (`vs_invocations`).
  * Fragment shader invocations (`fs_invocations`).
  * Clipping primitives count (`primitives`).
  * Frame time budget breakdown (CPU Active vs. CPU Wait vs. GPU Frame).

---

## 8. Shader Hot-Reloading Pipeline

1. **File Watcher (`dmon`)**: Listens recursively on `shaders/` and `compiledshaders/`.
2. **Compilation Trigger**: Editing a `.slang` file triggers `watch_callback()`, which invokes `compileslang.sh` via `trigger_shader_compilation()`.
3. **Slang Compilation**: `slangc` parses entry points (`vs_main`, `fs_main`, `cs_main`) and compiles updated SPIR-V targets to `compiledshaders/`.
4. **Dirty Flag Marking**: `pipeline_mark_dirty()` scans active pipeline entries and flags matches.
5. **Runtime Pipeline Rebuild**: At the start of the next frame, `pipeline_rebuild()` reconstructs `VkPipeline` objects seamlessly without interrupting execution. Old pipelines are retired on the submission timeline via the `DeleteQueue` — no device-wide stall.
6. **Pipeline Caching**: `pipeline_cache_save()` serializes `VkPipelineCache` to `pipeline_cache.bin` on application exit.

---

## 9. Developer Guide: How to Add a New Rendering Pass

To add a new graphics or compute pass to the engine, follow these steps:

### Step 1: Create the Slang Shader
Create `shaders/my_pass.slang`:
```slang
import slangtypes;

struct MyPush {
    uint32_t src_tex;
    uint32_t out_img;
    float    intensity;
    uint32_t pad;
};

[shader("compute")]
[numthreads(16, 16, 1)]
void cs_main(uint3 thread_id : SV_DispatchThreadID, uniform MyPush push) {
    // Shader logic accessing Bindless textures/images
}
```

### Step 2: Register Pipeline in `Renderer`
In `main.c` under `EnginePipelines`:
```c
// Add pipeline ID field
uint32_t my_pass_pipeline;

// In graphics_init() or pipeline creation section:
r->EnginePipelines.my_pass_pipeline = pipeline_create_compute(r, "compiledshaders/my_pass.comp.spv");
```### Step 3: Implement Pass Function
Declare intent through `PassDesc`; transitions, barriers, rendering scope, viewport/scissor, and pipeline bind are all derived. `pipeline` is the `PipelineID` (0 = bind yourself). Colors take zero-value defaults (`LOAD`/`STORE`) — name only deltas.

Compute pass:
```c
static void pass_my_custom(Renderer *r, VkCommandBuffer cmd) {
    uint32_t image = r->swapchain.current_image;
    GpuProfiler *frame_prof = &r->gpuprofiler[r->current_frame];

    GPU_SCOPE(frame_prof, cmd, "My Custom Pass", VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
        RenderTarget *reads[]  = {&r->hdr_color[image]};
        RenderTarget *writes[] = {&r->ldr_color[image]};

        begin_pass(r, cmd, &(PassDesc){
            .shader_reads       = reads,
            .shader_read_count  = 1,
            .shader_writes      = writes,
            .shader_write_count = 1,
            .pipeline           = r->EnginePipelines.my_pass_pipeline,
        });

        MyPush push = {
            .src_tex   = r->hdr_color[image].bindless_index,
            .out_img   = r->ldr_color[image].bindless_index,
            .intensity = 1.0f,
        };
        dispatch_push(r, cmd, BYTE_SPAN(push), (r->swapchain.extent.width + 15) / 16,
                      (r->swapchain.extent.height + 15) / 16, 1);
    }
}
```

Graphics pass with a clear color attachment (and optional depth):
```c
static void pass_my_draw(Renderer *r, VkCommandBuffer cmd) {
    PassAttachment color = {.target = &r->hdr_color[r->swapchain.current_image],
                            .load   = LOAD_CLEAR};
    PassAttachment depth = {.target = &r->depth[r->swapchain.current_image],
                            .load   = LOAD_CLEAR,
                            .clear  = {0.0f}}; // clear[0] = depth clear value

    begin_pass(r, cmd, &(PassDesc){
        .colors       = &color,
        .color_count  = 1,
        .depth        = &depth,
        .pipeline     = r->EnginePipelines.my_draw_pipeline,
    });

    cmd_draw(r, cmd, BYTE_SPAN(draw_push), 3, 1);
    end_pass(cmd);
}
```

### Step 4: Insert into Main Frame Loop
In `main()` inside `main.c`:
```c
pass_fire(r, cmd);
pass_my_custom(r, cmd); // <-- Added here
post_pass(r, cmd);
pass_smaa(r, cmd);
```

---

## 10. Building & Debugging Commands

```bash
# Debug build (default)
make -j$(nproc)

# Release build (O3 optimization)
make release -j$(nproc)

# Address Sanitizer (ASAN) build for memory leak & corruption hunting
make run_asan

# Clean build artifacts
make clean
```

---

## 11. Coding Conventions & Best Practices

1. **Vulkan Coordinate System**: Maintain Vulkan NDC conventions ($Y$ points downwards, depth range $[0.0, 1.0]$).
2. **Push Constant Alignment**: Always use `PUSH_CONSTANT(Name, BODY)` and verify standard 256-byte alignment (`_Static_assert`).
3. **Explicit Memory Allocation**: Allocation of temporary buffers in passes must use `cpu_pool` (`BUFFER_POOL_LINEAR`). Do not call `malloc()` or `vkAllocateMemory()` in frame loops.
4. **State Transition Integrity**: Images touched outside `begin_pass` (blits, captures, present transitions) are transitioned explicitly with `rt_transition_all()` / `image_transition_swapchain()` followed by `flush_barriers()`. Inside a pass, declare attachments/reads/writes in the `PassDesc` and let `begin_pass` derive them.
5. **Resource Lifetimes**: Destroy GPU resources only when no in-flight submission uses them: wait on the covering timeline value, or defer with `delete_queue_defer(r, r->timeline_last_submitted, fn, user)`. Shutdown order: `vkDeviceWaitIdle` → `delete_queue_drain` → destroy resources → device.
