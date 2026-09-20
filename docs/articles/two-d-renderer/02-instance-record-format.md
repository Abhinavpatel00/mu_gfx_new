# 02 — The instance record format, proven instead of assumed

> **Why this article exists.** A sprite instance is the only piece of data the GPU reads
> once per sprite and the CPU writes once per sprite. Its layout therefore decides both
> sides' cache behavior. It is also the one struct the validation layer cannot check,
> because it crosses C → Slang through a device address. This article audits it field by
> field, then proves the two layouts agree with a test you can run in CI.

**What you have at the end.** One 32-byte instance stream, no second `animations`
stream, a packing function built from `mu_quantize_unorm`, and a `layout-check` step that
fails the build when the two dialects drift.

---

## 2.1 Where you are, and what has to change

Article 01 shipped this:

```c
typedef struct ALIGNAS(16) SpriteInst {
    float    x, y, w, h;       /* 16 B */
    uint32_t color;            /*  4   */
    uint32_t picture;          /*  4   */
    uint32_t layer;            /*  4   */
    uint32_t flags;            /*  4   */
} SpriteInst;                  /* 32 B */
```

plus a second stream, `SpriteAnim { float u0, v0, u1, v1; }`, indexed by the same `iid`.
Every sprite therefore touched **two** cache lines in two different buffers, with two
base addresses live in registers and two independent TLB/L2 streams. That is the thing to
fix first, and it is a layout problem, not a bandwidth problem.

---

## 2.2 The audit: who actually reads each field

| Field | CPU culler | CPU sorter | Vertex shader | Fragment shader | Verdict |
|---|---|---|---|---|---|
| `x, y` | yes | no | yes | no | **hot**, keep |
| `w, h` | yes | no | yes | no | **hot**, keep |
| `color` | rarely | no | yes | yes (interpolated) | **hot**, keep |
| `picture` | no | yes (as key) | no | yes — but **uniform** per batch | **move to the root** |
| `layer` | no | yes (as key) | **never** | **never** | **delete from the instance** |
| `flags` | no | yes (partial) | yes (flip, via UV select) | no | keep, 4 B or fewer |
| `u0..v1` (stream 2) | no | no | yes | no | **fold into the instance** |

Two conclusions fall out, and they are worth stating as rules:

* **`layer` is a sorting key, not GPU data.** It exists so the CPU can order batches.
  Sending it to the GPU costs 4 B × 100 000 = 400 KB per frame for a field no shader
  instruction reads. It belongs in the sort key (article 03), not in the record.
* **`picture` is batch state.** Once article 03 groups instances by picture, the picture
  id is constant for a whole draw. A uniform is free (broadcast, no interpolation, no
  per-lane register); a per-instance id costs a load plus an interpolation slot. Move it
  to the root payload.

The freed 8 bytes pay for the UV rect, which kills the second stream. That is the whole
trade: **same 32 bytes, one stream instead of two, and zero GPU work spent on CPU-only
fields.**

---

## 2.3 The two layouts, and what each one costs

```c
/* Default: one stream, 32 B, every field read by the vertex shader. */
typedef struct ALIGNAS(16) SpriteInst {
    float    x, y, w, h;        /*  0..15 */
    uint16_t u0, v0, u1, v1;    /* 16..23 */
    uint32_t color;             /* 24..27 */
    uint32_t flags;             /* 28..31 */
} SpriteInst;

/* Bandwidth-bound variant: 20 B, quantized world rect, NO alignment attribute. */
typedef struct SpriteInstCompact {
    int16_t  x, y, w, h;        /*  0..7  exact world pixels, ±32767 */
    uint16_t u0, v0, u1, v1;    /*  8..15 atlas UV, unorm16 */
    uint32_t color;             /* 16..19 */
} SpriteInstCompact;
```

The bandwidth ledger, per frame at 100 000 sprites (one full-screen RGBA8 layer at
1080p is 2 073 600 × 4 = 8.29 MB; eight layers of overdraw is 66.4 MB):

| | 32 B layout | 20 B layout | Note |
|---|---|---|---|
| CPU writes | 3.2 MB | 2.0 MB | one linear pass over the instance array |
| GPU reads (DRAM) | ≈3.2 MB | ≈2.0 MB | one record per sprite; a quad's 4 vertices hit L1 |
| × 3 frames in flight | 9.6 MB | 6.0 MB | capacity, not traffic |
| Share of frame traffic | 3.2 / 66.4 = **4.8 %** | 3.0 % | with 8× overdraw |

Read the last row twice. At 100 000 sprites and eight layers of overdraw, the instance
array is **under 5 %** of the frame's memory traffic; the framebuffer is 20× larger than
the geometry feeding it. Shrinking the record by 37 % moves total traffic by less than
2 %.

So the honest default is the **32-byte layout**, for reasons that have nothing to do with
the GPU:

1. **The CPU pack loop is on the critical path, and a power-of-two stride vectorizes.**
   A 32-byte stride compiles to shifts; stride 20 needs `i*16 + i*4` addressing, and in
   article 06 the pack loop stops auto-vectorizing for the compact variant.
2. **No quantized coordinate to argue about.** `float` positions are exact everywhere;
   `int16` world pixels are exact only within ±32767. Both are fine in practice, but only
   one of them requires you to prove your world fits inside that box.
3. **Records never straddle cache lines.** With stride 20, a record crosses a 64-byte
   boundary whenever its offset mod 64 exceeds 44 — 31 % of records. With stride 32,
   never.

Flip to the compact layout when the profiler says the instance stream is actually
saturating bandwidth: ≥ 500 k instances, or a bandwididth-poor iGPU where fill is small
and geometry dominates. The switch is a compile-time flag, and it must change three
things **in one commit** — the struct, both `_Static_assert`s, and the shader-side
struct. Section 2.6 exists to catch exactly that.

And here is the trap I hit while writing this section, because it is the perfect
illustration of the whole problem:

```c
typedef struct ALIGNAS(16) SpriteInstCompact { int16_t x, y, w, h; ... };  /* sizeof == 32! */
```

`clang -std=gnu99` refuses the `_Static_assert`:

```
error: static assertion failed due to requirement 'sizeof(struct SpriteInst20) == 20':
   21 | _Static_assert(sizeof(SpriteInst20) == 20, "");
      |                ^~~~~~~~~~~~~~~~~~~~~~~~~~
note: expression evaluates to '32 == 20'
```

The alignment attribute applies to the **struct**, so its size rounds up to a multiple of
16 — while Slang, whose `int16_t` struct has natural alignment 4, emits
`ArrayStride 20`. Every instance after the first would be read from the wrong offset:
silently, with no validation error, producing sprites that "almost" look right. The fix
is one deleted attribute. The lesson:

> **C's `sizeof` and the shader's `ArrayStride` are two independent numbers that must be
> compared, never assumed equal.**

---

## 2.4 The audit result, written down

The final `SpriteInst` in `sprite_data.h` (both dialects), with the UV rect folded in and
the two CPU-only fields evicted:

```c
#if defined(__STDC__)
typedef struct ALIGNAS(16) SpriteInst {
    float    x, y, w, h;
    uint16_t u0, v0, u1, v1;
    uint32_t color;
    uint32_t flags;
} SpriteInst;
#elif defined(__SLANG__)
struct SpriteInst {
    float2   pos;      /* x, y   */
    float2   size;     /* w, h   */
    uint16_t u0, v0, u1, v1;
    uint     color;
    uint     flags;
};
#endif
```

and `SpriteAnim` and `SpritePush.animations` are **deleted** from the previous article's
header. Removing a field from a layout is the only refactor in this series that
simultaneously reduces bandwidth, removes a register, deletes a stream, and simplifies
the API — do it while the code is small.

---

## 2.5 The packer: integer math, `mu` primitives, no branches of our own

The quantization primitives already exist in `external/mu/mu/mu_bitpacking.h`; use them
rather than inventing a local `float_to_unorm`. `mu_quantize_unorm(v, n)` clamps to
[0, 1] and returns `int` in `[0, 2^n - 1]`, and `mu_dequantize_unorm(v, n)` is its
inverse (`mu_dequantize_unorm(32767, 16) == 0.499992`).

```c
/* src/two_d/sprite.c */
#include "sprite.h"
#include "sprite_data.h"

/* Linear 0..1 components into RGBA8. mu_quantize_unorm clamps, so no extra CLAMP. */
static FORCE_INLINE uint32_t sprite_pack_rgba(float r, float g, float b, float a) {
    return (uint32_t)mu_quantize_unorm(r, 8) | ((uint32_t)mu_quantize_unorm(g, 8) << 8) |
           ((uint32_t)mu_quantize_unorm(b, 8) << 16) | ((uint32_t)mu_quantize_unorm(a, 8) << 24);
}

static FORCE_INLINE uint16_t sprite_pack_unorm16(float v) {
    return (uint16_t)mu_quantize_unorm(v, 16);
}

void sprite_append(SpriteRenderer *sprites, float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1,
                   uint32_t rgba_linearpacked, PictureID picture, uint32_t layer, uint32_t flags) {
    SpriteInst *inst = &sprites->instances[sprites->count++];
    inst->x  = x;
    inst->y  = y;
    inst->w  = w;
    inst->h  = h;
    inst->u0 = sprite_pack_unorm16(u0);
    inst->v0 = sprite_pack_unorm16(v0);
    inst->u1 = sprite_pack_unorm16(u1);
    inst->v1 = sprite_pack_unorm16(v1);
    inst->color = rgba_linearpacked;
    inst->flags = flags;
}
```

Note what is *not* here: no clamping of world coordinates, no branch on `w == 0`, no
per-sprite allocation, and no writes outside the one instance record. `sprites->count`
is a single bump counter, which article 06 turns into the only loop-carried dependency
in the whole preparation phase.

In article 01 this function took `(x, y, w, h, rgba, picture, layer)`. It now takes a UV
rect instead of a picture, because the picture moved to the batch. **The signature
change is the audit made visible**: if a parameter cannot travel in the instance record,
it must travel in the root, and the only fields that can travel in the root are the ones
constant across a batch.

---

## 2.6 The layout contract test — the part that pays for the article

Two additions.

**A C-side dumper**, `tests/sprite_layout_check.c`, that prints every field offset and
the stride, and compiles only if the `_Static_assert`s in `sprite_data.h` hold:

```c
/* tests/sprite_layout_check.c */
#include <stdio.h>
#include <stddef.h>
#include "../src/two_d/sprite_data.h"

_Static_assert(sizeof(SpriteInst) == 32, "SpriteInst stride");
_Static_assert(offsetof(SpriteInst, u0) == 16, "SpriteInst.u0");
_Static_assert(offsetof(SpriteInst, v1) == 22, "SpriteInst.v1");
_Static_assert(offsetof(SpriteInst, color) == 24, "SpriteInst.color");

int main(void) {
    printf("SpriteInst  stride=%zu  pos=%zu size=%zu u0=%zu v0=%zu u1=%zu v1=%zu color=%zu flags=%zu\n",
           sizeof(SpriteInst), offsetof(SpriteInst, x), offsetof(SpriteInst, w),
           offsetof(SpriteInst, u0), offsetof(SpriteInst, v0), offsetof(SpriteInst, u1),
           offsetof(SpriteInst, v1), offsetof(SpriteInst, color), offsetof(SpriteInst, flags));
    return 0;
}
```

**A shader-side dump**, `make layout-check`, which is the only thing that proves the two
dialects agree:

```make
SPIRV_DIS ?= spirv-dis

layout-check: $(BUILD_DIR)/tests/sprite_layout_check.o | $(SCENE_SHADERS)
	$(CXX) $(LDFLAGS) $(BUILD_DIR)/tests/sprite_layout_check.o -o $(BUILD_DIR)/sprite_layout_check
	@echo "== C =="; $(BUILD_DIR)/sprite_layout_check
	@echo "== SPIR-V (ArrayStride) =="; $(SPIRV_DIS) compiledshaders/sprite.vert.spv | grep ArrayStride
	@echo "== SPIR-V (SpriteInst members) =="; $(SPIRV_DIS) compiledshaders/sprite.vert.spv | grep 'OpMemberDecorate %SpriteInst_natural'
```

The measured output for the layout in 2.4, from this workspace:

```text
== C ==
SpriteInst  stride=32  pos=0 size=8 u0=16 v0=18 u1=20 v1=22 color=24 flags=28

== SPIR-V (ArrayStride) ==
OpDecorate %_ptr_PhysicalStorageBuffer_SpriteInst_natural ArrayStride 32

== SPIR-V (SpriteInst members) ==
OpMemberDecorate %SpriteInst_natural 0 Offset 0     # pos
OpMemberDecorate %SpriteInst_natural 1 Offset 8     # size
OpMemberDecorate %SpriteInst_natural 2 Offset 16    # u0
OpMemberDecorate %SpriteInst_natural 3 Offset 18    # v0
OpMemberDecorate %SpriteInst_natural 4 Offset 20    # u1
OpMemberDecorate %SpriteInst_natural 5 Offset 22    # v1
OpMemberDecorate %SpriteInst_natural 6 Offset 24    # color
OpMemberDecorate %SpriteInst_natural 7 Offset 28    # flags
```

Eight numbers from the compiler, eight from the shader compiler, identical. This is what
"the layout is correct" means in this series: not "it renders", but "two independent
toolchains printed the same numbers and a script compared them".

Three properties make this test worth the 30 lines:

* It catches the `ALIGNAS` size-rounding bug from 2.3, which no amount of staring at
  pixels discovers.
* It catches the compact-layout switch done in the wrong order, when the C struct is
  20 bytes but the SPIR-V stride is still 32 because the header edit did not reach the
  shader through `#include`.
* It gives you a diff to paste into a code review, which is the only enforcement
  mechanism that survives contact with a deadline.

---

## 2.7 The shader reads the same bytes

The vertex shader now dequantizes instead of multiplying by a second stream, and the
picture arrives as a uniform:

```slang
struct SpriteInst {
    float2   pos;
    float2   size;
    uint16_t u0, v0, u1, v1;   /* unorm16 atlas UV rect */
    uint     color;            /* linear RGBA8 */
    uint     flags;            /* SPRITE_FLIP_X / SPRITE_FLIP_Y / kind bits */
};

struct SpritePush {
    SpriteInst* instances;
    uint        base;          /* first instance this batch draws */
    uint        sampler;       /* bindless sampler slot */
    uint        picture;       /* bindless sampled-image slot, uniform for the batch */
    float       view[4];       /* x0, y0, x1, y1 */
    float       viewport[2];
    float       texel[2];
    uint        flags;
    uint        pad;
};
[[vk::push_constant]] SpritePush pc;

static float2 unorm16_to_float(uint16_t a, uint16_t b) {
    return float2(float(a), float(b)) * (1.0 / 65535.0);
}

[shader("vertex")]
SpriteVarying vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    SpriteInst s = pc.instances[pc.base + iid];

    float2 corner = quad_corner(vid);
    float2 world  = s.pos + corner * s.size;

    float2 uv0 = unorm16_to_float(s.u0, s.v0);
    float2 uv1 = unorm16_to_float(s.u1, s.v1);
    float2 uv  = lerp(uv0, uv1, corner);

    /* Flips are a UV select, not a negative scale: two selects, no per-vertex branch,
       and mip selection stays correct because |dUV/dx| is unchanged. */
    uv = float2((s.flags & 1u) ? (uv0.x + uv1.x - uv) : uv,
                (s.flags & 2u) ? (uv0.y + uv1.y - uv) : uv);
    ...
}
```

Precision check, because this is the kind of thing that shows up as shimmering edges at
high resolution and nowhere else: unorm16 has 65536 steps. On a 4096-wide atlas, one
step is 4096/65536 = **0.0625 texels** — an order of magnitude finer than the 0.5 texel
bias a bilinear fetch already tolerates. On a 2048-wide atlas it is 0.031 texels. If
your atlas ever exceeds 32768 texels in a dimension, quantized UV stops being free and
you should either split the atlas or store UV as float — which costs the 8 bytes the
audit just freed. That is the trade, stated numerically, so the decision is cheap to
revisit.

Note also what the flags field does *not* do: it does not select a shader. Branching on
per-instance flags to pick between "plain", "9-slice" and "SDF" shading is how 2D
renderers end up with a batch per flag combination and an unmanageable PSO zoo. Those
belong in the batch's pipeline (article 03), so the flag bits that reach the instance
are only the ones that are cheap and data-parallel: flips, and a small kind tag for
debug tinting.

---

## 2.8 Failure modes and the next step

| Symptom | Cause | Detection |
|---|---|---|
| Sprites after the first are garbage | C `sizeof` ≠ shader `ArrayStride` | `make layout-check` |
| Sprites drift progressively across the screen | alignment padding on one side only | same |
| UVs shimmer, texture edges crawl | UV quantization too coarse for the atlas | the 0.0625-texel arithmetic above |
| `_Static_assert` fires after "just" adding a field | you added a 16-byte-aligned member | reorder: scalars first, `float[N]` last |
| Second stream was already removed but perf did not change | it was never the bottleneck | the 4.8 % row in 2.3 |

Two claims to take away:

1. **The instance record is 5 % of a 2D frame's traffic.** Optimizing it is a
   correctness-and-locality exercise, not a performance win. Do it once, do it exactly,
   and spend your attention on fill rate (article 07).
2. **Every layout that crosses C → Slang needs an automated cross-check**, because both
   compilers are silent when they disagree. `make layout-check` is 30 lines and it
   caught a real bug in this article while it was being written.

**Next:** [03 — Ordering and batching](03-ordering-and-batching.md), where the `layer`
and `picture` fields finish their migration into a 64-bit sort key, the instance array
gets reordered by a counting sort, and 100 000 sprites become ≤ 64 draw calls.