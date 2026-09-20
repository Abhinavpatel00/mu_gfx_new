# 04 — GPU-driven culling and indirect draws

> **Why this article exists.** Article 03 measured the CPU batcher at 0.7–4.7 ms for
> 100 000 sprites. That is fine at 60 fps and fatal at 120 fps, and it does not scale past
> a few hundred thousand sprites at all. The per-sprite work — transform, visibility,
> count — is embarrassingly parallel data, which makes it the GPU's job. This article moves
> it there, and is honest about when that is a pessimization.

**What you have at the end.** A compute kernel that classifies every instance, writes
per-batch indirect commands, and lets the CPU record a fixed, tiny number of draws without
knowing how many sprites are visible.

---

## 4.1 What moves, and what does not

| Work | Article 03 | Article 04 |
|---|---|---|
| Read the game's sprite submission | CPU | CPU (unchanged) |
| Compute the state key `(layer, picture, blend)` | CPU | CPU (unchanged) |
| Bucket + sort into batch ranges | CPU | **CPU (unchanged)** |
| Transform rect → clip space | vert shader | vert shader |
| Visibility test (frustum, off-screen, alpha 0) | nobody | **compute** |
| Per-batch instance count | CPU | **compute** |
| Emit draw commands | CPU | **compute** |
| Record draws | CPU | CPU, but count-free |

The sort stays on the CPU, and that is deliberate:

* Article 03 measured the whole CPU batcher at 0.7–1.1 ms **when submission is grouped by
  state** — the normal case for a layered game. The GPU has nothing to win there, and a GPU
  sorting network for 100 000 records is a far bigger engineering project than the 60 lines
  of histogram code it would replace.
* The CPU must know the batch *ranges* to upload instance data in order. A GPU that
  re-sorted would force a device-side indirection through a `visible[]` index list, which
  costs an extra dependent load per vertex (that is what `shaders/scene3d.slang:37` does for
  3D, and it is the right trade there, with far fewer, far larger instances).

What moves is everything that is *per-sprite and belongs to nobody on the CPU*.

---

## 4.2 The order-preservation problem, and two ways out

A compute kernel that compacts visible instances with `InterlockedAdd` produces a compact
array in **arrival order, not painter's order**. In 2D that is a correctness bug: sprites
from later layers can end up drawn before earlier ones. `shaders/scene3d.slang:30-34` does
exactly this compaction with `InterlockedAdd` — and it is fine in 3D because 3D uses a depth
buffer and per-instance order does not matter. **Do not port that trick to 2D unchanged.**

Two order-preserving options, both real:

**Option A — order-preserving visibility marks (recommended).** The kernel keeps every
instance in place and, when a sprite is invisible, degenerates it: `size = 0`. The quad
still reaches primitive setup and rasterizes to zero fragments. The instance array keeps its
painter order, no indirection is added, and the vertex shader is unchanged. This removes
*fragment* work, which — per article 07 — is where the bulk of the frame's cost lives
anyway.

**Option B — stable per-batch compaction.** Each batch's instance range is compacted by
itself with a wave-level scan (`WavePrefixCountBits` + a per-wave base accumulated in order),
which is stable within the range, so painter's order survives. This also removes vertex work.
It is 4× the kernel complexity and it only pays when instance *count* is the bottleneck
rather than fill rate — i.e. when you draw hundreds of thousands of mostly tiny or mostly
overlapping sprites. Ship A first; add B behind a flag when a measurement asks for it.

The code below implements A. That is not laziness: A gets you the two things that actually
matter (a CPU that never sees per-sprite data, and draws that cull themselves), and it keeps
the option to add B later without touching the renderer's interface.

---

## 4.3 The enablement facts, checked

| Requirement | Status in this backend |
|---|---|
| `bufferDeviceAddress` (device-address pointers in shaders) | enabled, `vk.c:314`; `gpu_pool` has `SHADER_DEVICE_ADDRESS_BIT`, `vk.c:3160` |
| `multiDrawIndirect` | enabled, `vk.c:307` |
| `drawIndirectCount` (`vkCmdDrawIndirectCount`) | enabled, `vk.c:313` |
| `shaderDrawParameters` (`SV_InstanceID`, `SV_StartInstanceLocation`) | enabled, `vk.c:293` |
| `drawIndirectFirstInstance` | **not enabled** — absent from `apply_caps`, `vk.c:283-315` |
| compute-writable, device-local storage | `gpu_pool` is `GPU_ONLY` with `STORAGE | DEVICE_ADDRESS | INDIRECT_BUFFER | TRANSFER_DST`, `vk.c:3159-3163` |

The missing `drawIndirectFirstInstance` shapes the design: **every indirect command carries
`firstInstance = 0`**, and the batch base index travels in the root payload. This costs
nothing — it is also the cheaper shader (`pc.base + iid` is one integer add in a register,
instead of relying on the driver to fold a base instance into `SV_InstanceID`).

A buffer slice's device address is `r->vk.gpu_base_addr + slice.offset`; `gpu_base_addr` is
exactly that base address, computed once at init (`vk.c:3169-3171`) and currently unused
anywhere in the tree. This is what it was put there for.

I verified the shader half of this design before writing it: a compute kernel that reads and
writes `SpriteInst` through pointers in a push constant compiles with `slangc` and validates
under `spirv-val --target-env vulkan1.3`, emitting:

```text
OpCapability PhysicalStorageBufferAddresses
OpExtension "SPV_KHR_physical_storage_buffer"
OpMemoryModel PhysicalStorageBuffer64 GLSL450
```

The same capability appears in the **vertex** stage (`sprite.vert.spv`), plus
`OpCapability DrawParameters` for `SV_InstanceID`. Both are backed by features this backend
enables, so the design is not hypothetical.

---

## 4.4 The kernel

```slang
/* shaders/sprite_cull.slang */
#include "common.slang"
#include "../src/two_d/sprite_data.h"

struct SpriteCullPush {
    SpriteInst* instances;   /* in:  the sorted instance array (gpu_pool)         */
    SpriteInst* output;      /* out: same order, invisible sprites degenerated    */
    uint*       counts;      /* out: per-batch visible count (zeroed by the host) */
    BatchInfo*  batches;     /* in:  first + gpu_state per batch                  */
    uint        batch_count;
    uint        sprite_count;
    float       view[4];     /* x0, y0, x1, y1 in world pixels */
};
[[vk::push_constant]] SpriteCullPush pc;

[shader("compute")][numthreads(64, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.sprite_count)
        return;

    SpriteInst s = pc.instances[i];

    /* Pixel-exact AABB reject: no data-dependent branches, one multiply per compare. */
    float x1 = s.pos.x + s.size.x;
    float y1 = s.pos.y + s.size.y;
    bool visible = (s.size.x > 0.0) && (s.size.y > 0.0) && (s.color.a != 0u) &&
                   (x1 > pc.view[0]) && (s.pos.x < pc.view[2]) &&
                   (y1 > pc.view[1]) && (s.pos.y < pc.view[3]);

    if (!visible)
        s.size = float2(0.0, 0.0); /* degenerate: order preserved, zero fragments */

    pc.output[i] = s;

    if (visible) {
        /* Batches tile the array, so a search in a small L1-resident table beats any
           per-instance batch field. */
        uint batch = batch_for_index(pc.batches, pc.batch_count, i);
        InterlockedAdd(pc.counts[batch], 1u);
    }
}
```

Two choices worth defending:

* **`batch_for_index` is a binary search over ≤ 1024 batch records** — about 10 iterations,
  all L1 hits. The alternative, storing a batch id per instance, costs 2 bytes per sprite
  and a second stream read; the histogram in article 03 already needs the *bucket* id,
  which differs from the *batch* id because of the merge step. A 12 KB table plus a
  log-1024 search is cheaper than 200 KB of extra per-instance data.
* **`InterlockedAdd` on a 1024-entry counter array.** Atomics with ≤ 1024 targets, all
  L1/L2-resident, are cheap. The same trick at per-sprite granularity would not be.

The vertex shader's only change is one deleted line: the flip selects (article 03 §3.5 moved
flips into the UV endpoints, so nothing is left to select on the GPU).

---

## 4.5 The host side: zeroing counters, recording count-free draws

```c
/* src/two_d/sprite.c — one device-local slice for everything the GPU writes */
static void sprite_cull_resources_create(SpriteRenderer *sprites, VkBackend *vk) {
    sprites->cull_slice = buffer_pool_alloc(
        &vk->gpu_pool,
        sizeof(SpriteInst) * SPRITE_MAX_INSTANCES +               /* visible instances */
            sizeof(uint32_t) * SPRITE_MAX_BATCHES +               /* batch counters    */
            sizeof(VkDrawIndirectCommand) * SPRITE_MAX_BATCHES +  /* indirect commands */
            sizeof(BatchInfo) * SPRITE_MAX_BATCHES,               /* batch tile table  */
        256);
    assert(sprites->cull_slice.buffer && "gpu_pool exhausted: raise size_of_gpu_pool");
    sprites->cull_addr = vk->gpu_base_addr + sprites->cull_slice.offset;
}
```

Recording becomes a loop whose body contains **no per-sprite quantity at all**:

```c
static void sprite_record_gpu_driven(SpriteRenderer *sprites, VkBackend *vk, VkCommandBuffer cmd) {
    const Buffer *sorted = &sprites->gpu_instances[vk->current_frame];
    uint32_t      bcount = sprites->batch_count;

    /* 1. Zero the counters and write the indirect templates: a few KB, not a few MB.
          Both live in this frame's host-visible cpu_pool staging. */
    VkDrawIndirectCommand *cmds = (VkDrawIndirectCommand *)staging_cmds(sprites, vk);
    for (uint32_t b = 0; b < bcount; b++)
        cmds[b] = (VkDrawIndirectCommand){.vertexCount = 4, .instanceCount = 0,
                                          .firstVertex = 0, .firstInstance = 0};
    memset(staging_counters(sprites, vk), 0, sizeof(uint32_t) * SPRITE_MAX_BATCHES);

    /* 2. Upload through the staging ring: bounded by batch count, not sprite count. */
    BufferSlice counters = renderer_upload_buffer(vk, cmd, counters_bytes(sprites, bcount), 16);
    BufferSlice indirect = renderer_upload_buffer(vk, cmd, cmds_bytes(sprites, bcount), 16);

    /* 3. Cull. Writes visible instances, bumps the counters it was handed. */
    SpriteCullPush cull = {.instances    = sorted->address,
                           .output       = (SpriteInst *)sprites->visible_addr,
                           .counts       = (uint32_t *)counters_addr,
                           .batches      = (BatchInfo *)sprites->batch_info_addr,
                           .batch_count  = bcount,
                           .sprite_count = sprites->count,
                           .view         = {view_x0, view_y0, view_x1, view_y1}};
    dispatch_push(vk, cmd, BYTE_SPAN(cull), (sprites->count + 63) / 64, 1, 1);

    /* 4. One barrier: compute writes must be visible to the indirect stage. */
    cmd_buffer_barrier(cmd, indirect.buffer, indirect.offset, indirect.size,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

    /* 5. One indirect draw per batch, still rooted so it learns its base and picture. */
    for (uint32_t b = 0; b < bcount; b++) {
        SpritePush push = {.instances = sprites->visible_addr,
                           .base      = sprites->batches[b].first,
                           .picture   = sprites->pictures.bindless[...].slot,
                           .sampler   = vk->default_samplers.samplers[SAMPLER_LINEAR_CLAMP]};
        push_constants(vk, cmd, BYTE_SPAN(push));
        vkCmdDrawIndexedIndirect(cmd, indirect.buffer,
                                 indirect.offset + b * sizeof(VkDrawIndirectCommand), 1,
                                 sizeof(VkDrawIndirectCommand));
    }
}
```

Five notes:

1. **Calling `vkCmdDrawIndexedIndirect` directly is legitimate.** `pass_*` functions receive
   a raw `VkCommandBuffer`, and `pass_nuklear` already calls `vkCmdBindIndexBuffer` and
   `vkCmdDrawIndexed` directly (`src/nuklear_pass.inl:50-66`). The engine wraps state that
   must be *derived* from engine state — passes, transitions, root payloads. A draw whose
   parameters only the caller knows is exactly what the raw handle is for.
2. **`firstInstance = 0`, always** (§4.3). The base travels in the root payload.
3. **`cmd_buffer_barrier` (`vk.h:614`) is the one non-image barrier in this series.**
   `BarrierBatch` handles images only, so compute writes to an indirect buffer need the
   explicit helper. Skip it and the draws read the zeroed template or last frame's counts.
4. **Read the counters back only for the UI.** `sprite_submitted_count` is one submission
   behind — exactly what article 01's API promised. Fetch it by copying the counter buffer
   into a host-visible readback slot and waiting on the timeline value, the way
   `capture_consume` does (`renderer.c:319`). Never stall a frame for a number nobody needs
   this frame.
5. **Recording cost is now bounded by state count.** The push loop runs `batch_count` times
   regardless of `sprite_count`. One million sprites and one hundred thousand record the
   same commands; only the dispatch size changes.

---

## 4.6 When this is a pessimization

Do not adopt this because it sounds advanced. Adopt it when a measurement asks for it.

| Condition | Verdict |
|---|---|
| ≤ 20 000 sprites, submission already grouped | **CPU wins.** ~0.1–0.2 ms of prep; dispatch + barrier + indirect overhead is comparable, and you have added a kernel to debug. |
| 100 000 sprites at 60 fps | **Marginal.** CPU batcher measured 0.7–1.1 ms; the GPU path costs one dispatch and a barrier and removes almost all per-sprite CPU work. |
| 100 000+ sprites at 120 fps, or CPU already at 60 % of frame time | **GPU wins clearly.** Per-sprite CPU cost is gone; only the state histogram remains. |
| Large worlds with most sprites off-screen | **GPU wins decisively.** Option A turns thousands of invisible sprites into zero-fragment quads. |
| You have not measured your frame yet | **Change nothing.** Article 08 first. |

The measurable outcome to verify: `sprite->count` no longer influences the recorded command
count (only `batch_count` does), and with the camera parked in a corner of a large scene,
`sprite_submitted_count` — the GPU's own number — is visibly lower than the submission count.
If both hold, the work moved.

**Next:** [05 — Pictures: atlas, residency, uploads](05-pictures-atlas-residency.md), where
`PictureID` stops being a bindless slot and becomes a dense index, the atlas stops
fragmenting batches, and evicting a texture stops being a way to crash the driver.