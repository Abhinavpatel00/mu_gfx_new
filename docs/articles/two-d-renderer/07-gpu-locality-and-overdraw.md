# 07 — GPU locality and overdraw

> **Why this article exists.** Everything so far optimized what reaches the GPU. This article
> is about what the GPU does with it. For a 2D renderer the answer is lopsided: a 100 000
> sprite frame is 400 000 vertices — trivial for any GPU made this decade — but at 1080p it
> is 2 million pixels, and every blended sprite touching the same pixel reads *and* writes
> it. Fill rate, not geometry, is what the measured frame spends its time on, and the fixes
> are all about moving less data.

**What you have at the end.** An understanding of where the frame's GPU time actually goes,
a list of mitigations ordered by measured effectiveness, and the two engine flags
(`LOAD_CLEAR` on the target, discard-style transitions) that keep the render target itself
from dominating.

---

## 7.1 The vertex path: cheap by construction, verify anyway

The quad is emitted as `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP` with 4 vertices per instance
(article 01). The alternative, a 6-vertex triangle list, costs 50 % more vertex shader
invocations for identical coverage. The post-transform vertex cache makes the 6-vertex case
less bad than it looks — vertices 3–5 of the second triangle hit the cache — but the
4-vertex strip costs nothing and wins, so there is nothing to argue.

Per-instance work in the vertex shader, after article 03's record format:

* one indexed load of 32 bytes (`pc.instances[pc.base + iid]`), L1-resident for the 4
  vertices of the quad;
* two float multiplies and adds to map pixel position to clip space against `pc.view`;
* two unorm16 dequantizations (multiplies by `1/65535`);
* the two flip comparisons article 03 §3.5 deleted from the GPU side entirely.

That is roughly 20 ALU ops per vertex for the whole sprite. A GPU executes a vertex in well
under 10 ns even at low clocks; 400 000 of them is well under a millisecond. Confirm with
the `GpuProfiler` scope around the sprite pass (article 03 §3.7): if the pass is slow, the
*fragment* side is where the time is, which is the rest of this article.

One deliberate omission: no index buffer. `SV_VertexID` derives the corner, so the
"indexed" in `vkCmdDrawIndexedIndirect` (article 04) refers to the *command*, not to an
index buffer — there is none, and `firstIndex = 0` means the driver treats the draw as
non-indexed for fetch purposes.

---

## 7.2 The overdraw arithmetic

Numbers first, mitigations second. Consider the working case from this series' benchmarks:
1080p, `SPRITE_TARGET_FORMAT` (`VK_FORMAT_R16G16B16A16_SFLOAT`, 8 bytes per pixel), and a
stack of sprites with 8× average overdraw — a busy 2D scene with particles, UI, and a
background.

| Cost | Formula | Result |
|---|---|---|
| Target written once | 1920×1080 × 8 B | 16.6 MB per frame |
| Target written 8× (blended overdraw) | 16.6 × 8 | **132 MB per frame** |
| Blend = read-modify-write | reads 8 + writes 8 B/px | ×2 → **265 MB per frame** |
| At 60 fps | 265 MB × 60 | **15.9 GB/s** |

For comparison, the instance stream for 100 000 sprites is 3.2 MB — article 02's point that
it is under 5 % of frame traffic, now with the other 95 % accounted for. A mid-range laptop
GPU has roughly 60–100 GB/s of memory bandwidth, so *one* pass of 8× blended overdraw at
1080p consumes a quarter of the entire budget. This is why 2D renderers die by overdraw and
not by anything this series optimized before now.

The table also explains two choices already made:

* **`LOAD_CLEAR`, not `LOAD_LOAD`** (`pass_sprites`, article 03 §3.7): the target is fully
  rewritten every frame by a full-screen background sprite, so loading last frame's contents
  is 16.6 MB of bandwidth for nothing.
* **`STORE_KEEP`** when the target is presented, `STORE_DISCARD` when it is not: discarding
  an intermediate target's contents skips the write-back entirely. `rt_transition_all`
  handles the layout side; the load/store ops come from the `PassDesc`.

---

## 7.3 Mitigations, ordered by effectiveness

**1. Do not draw invisible sprites (article 04's kernel).** The cheapest pixel is the one
never submitted. Degenerate marks cost a few comparisons per sprite and remove both
fragments *and* blend traffic.

**2. Cut overdraw at the source, per layer.** Particles, shadows, and glow effects are the
8× in the arithmetic above. The engine cannot fix this; the *art direction* can, and the
counter that proves it is cheap: track per-layer submission counts in debug builds. When one
layer contributes 60 % of submissions, that is where to look.

**3. Fewer blend classes, not fewer batches.** Article 03's batching already merges
everything sharing GPU state; what it cannot merge is the *ordering* constraint between an
opaque sprite and a blended one. Keeping most sprites opaque (`blend = 0`) lets them share a
batch even across layers, and opaque sprites can also take a depth test — early-Z rejects
their fragments before the expensive blend unit ever runs.

**4. Premultiplied alpha.** Blending with `src = ONE, dst = ONE_MINUS_SRC_ALPHA` instead of
`src = SRC_ALPHA` costs the same bandwidth, but the CPU can pre-multiply color once at push
time instead of the shader computing it per fragment — one less multiply per fragment, and it
composes correctly at atlas edges (no dark halos), which is worth more than the multiply.

**5. Texel-aligned UVs and atlas locality (article 05).** A bilinear fetch touches a 2×2
texel footprint; sprites sampled from nearby atlas regions keep their footprint lines in L2
across the whole batch. Rotated or heavily minified sprites scatter their footprint — and
minification without mips makes it 4–16× worse. Exact numbers are vendor-specific; the
direction is not.

**6. Match the target format to the content.** `R16G16B16A16_SFLOAT` costs 8 B/px; if the
pipeline never needs HDR range, `R8G8B8A8_UNORM` halves every number in §7.2's table
including the 15.9 GB/s. A per-project decision, but a one-line change with a measurable
frame-time delta.

What did *not* make the list: sorting opaque sprites front-to-back (there is no depth buffer
in this pipeline by default; adding one buys early-Z at the cost of the clear and the test —
worth it only above ~4× overdraw of *opaque* content), and tile-based-renderer-specific
tricks (the measured platform here is a laptop iGPU, which is tile-based — but the wins
above are portable, and the tile-specific ones are driver-dependent guesses).

---

## 7.4 Failure modes

| Symptom | Likely cause | Fix |
|---|---|---|
| Sprite pass slow, vertex count tiny | fill rate, not geometry | §7.3 items 1–3, in order |
| Frame time scales with *sprite area*, not count | overdraw | §7.2's arithmetic; find the layer |
| Dark halos around atlas sprites | straight-alpha blend over premultiplied atlas | item 4 |
| Slow only on iGPU, fine on dGPU | target format + overdraw jointly exceed iGPU bandwidth | items 2 and 6 |
| Overdraw "fixed", frame still slow | you fixed the wrong pass | `GpuProfiler` scopes around *every* pass (article 08) |

The summary: **the GPU is bandwidth-starved by blending, not compute-starved by sprites.**
Every mitigation above reduces bytes per pixel; none of them touch the CPU or the batching
work of the previous articles, because the two sides of the pipe have different
bottlenecks and need different tools.

**Next:** [08 — Measurement gates](08-measurement-gates.md), the closing article: how every
number quoted in this series stays true as the code changes — counters, a fixed benchmark
scene, a perf log, and the CI gate that fails the build when a regression lands.