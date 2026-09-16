# Phase 2 — Closing Out the API Redesign

This document explains what we intend to do next, why each item matters, and what we
deliberately leave alone. It follows the pass API, root-argument model, pipeline defaults,
and timeline + DeleteQueue work (see `nographics-api-learnings.md`, Part 6).

The phase is small by design: three independent work items plus documentation. Nothing here
changes rendering behavior; everything here removes either hidden coupling or hidden
invariants.

---

## A. Desc defaults sweep (plan step 5, the last planned step)

### A1. `RenderTargetSpec` zero-value defaults

**Intent.** Make the zero value of `RenderTargetSpec` a valid, sane configuration:

| field | zero value today | becomes |
|---|---|---|
| `layers` | 0 (invalid — creates 0-layer images) | default to 1 at creation |
| `mip_count` | 0 = auto-compute (already good) | unchanged, documented |
| `aspect` | 0 = infer from format (already good) | unchanged, documented |
| `usage` | 0 (invalid — unusable image) | stays required; asserted |

Call sites then shrink from eight named fields to the three that matter:

```c
rt_create(r, &rt, &(RenderTargetSpec){
    .width = w, .height = h, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
});
```

**Why.** NoGraphicsAPI's core ergonomics lesson is "public descriptor structures have useful
defaults; call sites name only deltas". We applied that to pipeline configs; render-target
creation is the noisiest remaining desc. The defaults are already half-implemented by
accident (`mip_count == 0`, `aspect == 0`); we complete them and make `layers == 0` mean 1
instead of a silent miscreation. `usage == 0` stays an assert — an image that can be neither
sampled nor attached is a programming error, not a default.

### A2. Sampler creation wrapper

**Intent.** `sampler_create()` currently takes a raw `VkSamplerCreateInfo*`. We add a
`SamplerDesc` with defaults (linear filter, repeat addressing, 1..8 anisotropy, optional
compare) and reduce the public path to:

```c
sampler_create(r, &(SamplerDesc){.compare_enabled = true}, &out_id);
```

The `VkSamplerCreateInfo` construction moves behind the wrapper. The one existing call site
(the seven default samplers) converts; imgui's internal sampler stays untouched.

**Why.** Same rationale as A1, plus type hygiene: the public surface should not require
callers to know `VkSamplerCreateInfo` field ordering (anisotropy needs `maxAnisotropy` set
only when `anisotropyEnable` is on — a known foot-gun). Backend keeps the Vk type.

### A3. Upload helper defaulting

**Intent.** `renderer_upload_buffer_to_slice(..., staging_alignment)` requires every caller
to know the staging-buffer alignment rule. We keep it for the general case and make the
common case one call: `renderer_upload_buffer(r, cmd, dst_slice, BYTE_SPAN(data))`, which
applies the device-preferred alignment internally.

**Why.** Alignment is backend knowledge, not caller knowledge. The current signature makes
the common call longer than the rare one, which is backwards.

### A4. Spans-by-value audit

**Intent.** The doctrine requires `Span`/`ByteSpan`/range parameters passed by value
(pointer+size in registers). We audit the new APIs (`begin_pass`, `cmd_draw`,
`dispatch_push`, upload helpers) and the existing hot paths for violations.

**Why.** Cheap to do now while the surface is small; this is the discipline the doctrine
says to re-check after every change.

---

## B. Timeline readback hardening

**Current state, precisely.** Capture readback slots and GPU-profiler query pools are indexed
by `current_frame % CAPTURE_SLOTS` and consumed one-to-three frames later by slot arithmetic.
This is *correct today* — the frame-slot timeline wait in `frame_start` covers exactly the
submission that produced the data, because `CAPTURE_SLOTS == MAX_FRAMES_IN_FLIGHT`. But the
correctness is an accident of two constants agreeing, invisible to a future reader and
silently broken if either changes (e.g. `CAPTURE_SLOTS` raised to 4 for video pipelining, or
extra submissions added for async uploads).

**Intent.**

1. Record `submit_value` (the timeline value of the submission carrying the copy) into the
   capture slot when the copy is recorded; consume only after a timeline poll covers it
   (assert when it is already covered, wait otherwise).
2. Same treatment for the profiler: record the value, collect only when covered.
3. Replace the silent `if (c->in_flight[slot]) return;` skip with a debug-visible event —
   a dropped screenshot today is indistinguishable from a taken one.

**Why.** This is the "validate and resolve at system boundaries" doctrine applied to
completion tracking: the boundary (frame start) should consult the timeline, not infer
safety from array indices. It converts a latent invariant into an enforced one. It also
removes the last structural dependency on fences.

**Non-goal.** No behavior change in the happy path; no new sync primitives.

---

## C. Micro-fixes from the live verification run

The run validated the pass API end-to-end (~90 s of frames, resize path, zero validation
errors) and surfaced three small items:

### C1. Screenshot alpha channel

The app's own screenshot came out fully transparent: `capture_consume` copies the swapchain
pixel alpha as-is, but swapchain alpha is undefined. Fix: force alpha = 255 in both the BGRA
swap and the RGBA direct path.

**Why.** One-line correctness fix; makes the built-in capture trustworthy (and composable in
external tools).

### C2. `push_constants` becomes backend-internal

After the root-argument conversion, `push_constants` has zero remaining call sites outside
`emit_root_data`. Mark it `static` so the compiler enforces "one push path" instead of
relying on review.

### C3. Document the remaining `vkDeviceWaitIdle` calls

The idles left in `rt_destroy` (immediate path), `capture_resize`, `capture_shutdown`, and
the final swapchain destroy are intentional: they are shutdown/resize paths where a stall is
acceptable and deferral would add complexity for no frame-time benefit. Each gets a one-line
comment stating that, so the next reader does not "fix" them by accident.

**Why.** The philosophy doc says comments explain why; these four are exactly the kind of
"why is this slow on purpose" knowledge that gets lost.

---

## D. Deliberately out of scope

* **Move `ByteSpan` into `external/mu`** — right thing eventually (doctrine: reusable
  primitives live in mu), but it touches the submodule; do it as its own change.
* **Indexed/indirect payload variants** (`cmd_draw_indexed`, `dispatch_indirect_push`) — the
  engine has no indexed geometry yet; add when the first mesh lands.
* **`GpuRange`-based buffer commands** (address-based copies) — requires the Vulkan 1.4
  address-command extensions we already rejected for now.
* **Behavioral changes to rendering** — none of the above may alter output; verification is
  the same screenshot-diff-against-baseline method used in phase 1.

## Order of work and acceptance

1. C1/C2/C3 (small, independent, immediately verifiable)
2. B (timeline readback; verify with a screenshot + video record round and a debug run)
3. A1–A4 (mechanical sweep; verify with a clean build + one full-screen run)
4. Docs: fold statuses into `nographics-api-learnings.md` Part 6 and the design doc

Done means: `make` green, one live run with screenshot + video capture + resize + shader
hot reload and zero validation errors, and no raw `VkSamplerCreateInfo`/`RenderTargetSpec`
field-guessing left at call sites.
