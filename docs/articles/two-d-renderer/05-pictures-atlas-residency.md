# 05 — Pictures: atlas, residency, uploads

> **Why this article exists.** Article 02 moved `picture` out of the per-instance record and
> into the batch key; article 03 then measured that batches fragment whenever the dense
> picture index is not dense. This article builds the picture table that makes that promise
> real, the atlas that keeps texture memory small, and the upload path that gets pixels onto
> the GPU without stalling a frame.

**What you have at the end.** A `PictureTable` with dense batching indices, one growing
atlas, an upload path mirroring `nuklear_init`'s verified one-time-submit pattern, and
eviction that is safe by construction.

---

## 5.1 Two identifiers, one table

A `PictureID` is a stable, game-visible handle. A *dense index* is what the batcher sorts by.
They are not the same number, and conflating them is what fragments batches:

```c
typedef struct PictureEntry {
    uint32_t bindless_slot;  /* descriptor slot the shader samples              */
    uint32_t dense_index;    /* what the batcher sorts by (see 5.2)             */
    uint32_t atlas_offset;   /* texel x/y packed by mu_pack_2x16, w/h likewise  */
    uint32_t refcount;
} PictureEntry;

typedef struct PictureTable {
    PictureEntry entries[MAX_PICTURES];   /* indexed by PictureID */
    uint16_t     dense_to_id[MAX_PICTURES];
    uint16_t     dense_free_list[MAX_PICTURES];
    uint32_t     dense_count;
    mu_id_pool   pool;                    /* recycles PictureIDs */
} PictureTable;
```

Rules:

* `PictureID` values are **stable for the lifetime of a loaded picture** and recycled by
  `mu_id_pool` on eviction. The game never sees a recycled id, because `pool` hands out
  monotonic handles; the *entry* is what gets reused.
* `dense_index` is **valid only while the picture is resident**. It is assigned on upload
  from `dense_free_list` and returned on evict. The batcher's key uses it; a non-resident
  picture cannot appear in a frame because nothing can sample it.

Two dense indices matter here, and keeping them straight prevents a classic bug: the
*bindless slot* is dense across all allocated textures (the descriptor heap is dense by
construction — `bindless_array` hands out slots from a pool), but *residency* is not. A
frame's working set is maybe 32 pictures out of 4 096 loaded ones; sorting by bindless slot
sorts by allocation order, which is scattered, which is exactly the "draw count tracks sprite
count" failure from article 03 §3.8.

---

## 5.2 The atlas: one texture beats four thousand

The numbers, computed rather than guessed:

| Layout | Memory | Descriptor cost | Batching |
|---|---|---|---|
| 4 096 × 64×64 RGBA8 textures | 16 MB + per-image overhead, 4 096 image views | 4 096 descriptors of 65 536 | every distinct texture = one batch break |
| One 2048² RGBA8 atlas | 4 MB, +1.33× for mips ≈ 5.3 MB | 1 descriptor | one batch for everything on it |
| One 2048² BC7 atlas | 1 MB, +1.33× ≈ 1.33 MB | 1 descriptor | same, with block compression |

The 2048² atlas with a skyline (shelf) packer wins on all three axes for 2D sprites, and it
is what `MAX_PICTURES 4096` × typical 64–256 px sprites actually fits into (about 300
64×64 sprites per atlas; a couple of atlases, one per blend class, is normal). BC7 is the
right call when the art budget matters more than upload cost — BC7 encoding is offline.

Two atlas-specific details that prevent bleeding artifacts:

* **Gutters.** Pad every packed sprite by 1 texel on each side and inset the UV rect by half
  a texel. Bilinear filtering then never leaves the sprite's own texels. This is data you
  control at pack time; there is no shader-side fix that is cheaper.
* **Texel-aligned UVs.** Article 02's unorm16 UV endpoints are exact when the source rect is
  pixel-aligned in the atlas, which the packer guarantees. Rotated sub-sprites would break
  this — do not rotate in the atlas; rotation belongs to the instance transform.

The packer itself is ~60 lines: keep a current shelf y, a current shelf height, and a
free-list of full shelves; first-fit the sprite's width into open shelves, else open a new
one. A skyline packer buys maybe 5–8 % packing density over shelf for 2D sprite sets; it is
not worth the complexity until a measurement says the atlas is overflowing.

---

## 5.3 Uploads: the one-time-submit pattern, measured

`create_texture` (`vk.c:899`) takes a `ByteSpan` of pixels and does everything for you:
staging allocation, copy, transition to `IMAGE_LAYOUT_SHADER_READ_ONLY`. So the question is
only *when* to call it. There are two legitimate answers:

**Load time (the common case).** You are inside `nuklear_init`-style initialization — no
frames are in flight, `vkDeviceWaitIdle` has either just run or nothing has been submitted
yet. Call `create_texture` directly. This is exactly what `nuklear_init` does
(`src/nuklear_renderer.inl:35-66`): map a staging buffer, copy, submit a one-time command
buffer, wait on the fence it returns. Verified pattern; do not invent a new one.

**Mid-frame (the atlas update).** A new picture arrives during frame N. Do **not** submit a
one-time command buffer — that stalls the timeline you are about to submit against. Instead,
treat the atlas like any other per-frame resource:

1. Copy the new sprite's pixels into the frame's `cpu_pool` staging ring (16-byte aligned,
   like every other `renderer_upload_buffer` call in this series).
2. Record `vkCmdCopyBufferToImage` into frame N's main command buffer, followed by an
   `rt_transition_all` batch entry for the atlas image (`UNDEFINED → SHADER_READ_ONLY`,
   since the copy overwrites the whole region).
3. The picture becomes sampleable in frame N's draw — no stalls, no extra submissions.

One caveat: the atlas must be re-creatable. `STORAGE | COLOR_ATTACHMENT |
SAMPLED_IMAGE | TRANSFER_DST | TRANSFER_SRC` (`vk.c:3146-3151`) is what an atlas needs; a
growing atlas that is *rebuilt* (new size, new image, copy old contents) uses the same
`TRANSFER_SRC` flag — that is why it is in the creation flags.

---

## 5.4 Eviction: destroy is immediate, so *you* must defer

The doctrine says resource destruction is immediate. The driver says destroying a `VkImage`
that a recorded-but-not-finished command buffer still samples is undefined behavior. Both
are right, which is why `DeleteQueue` exists:

```c
/* Evicting a picture whose last use may still be in flight. */
void picture_table_evict(SpriteRenderer *sprites, VkBackend *vk, PictureID id) {
    PictureEntry *e = &sprites->pictures.entries[id];

    /* Return the dense index at once: no new frame can batch against it. */
    sprites->pictures.dense_free_list[sprites->pictures.dense_free_count++] = e->dense_index;
    e->dense_index = DENSE_INVALID;

    /* The image itself may still be sampled by recorded/executing frames:
       defer destruction to a timeline value past every possible use. */
    delete_queue_defer(r, r->vk.timeline_last_submitted, picture_destroy_cb,
                       (void *)(uintptr_t)e->bindless_slot);
    e->bindless_slot = 0;
    mu_id_pool_release(&sprites->pictures.pool, id);
}

static void picture_destroy_cb(VkBackend *vk, void *user) {
    /* bindless_array tracks the slot; destroying the image and freeing the slot
       happen together, after the timeline value passed when this was queued. */
    bindless_array_free_slot(&vk->bindless, (uint32_t)(uintptr_t)user);
    destroy_texture(vk, (TextureID){.slot = (uint32_t)(uintptr_t)user});
}
```

The ordering is the whole point:

1. **Dense index returns immediately** — the *batcher* can no longer reference the picture,
   which is the guarantee article 03's key packing depends on.
2. **Image destruction defers** — past `timeline_last_submitted`, the value the delete queue
   keys on, which is ≥ the last submission that could have recorded a draw sampling it. The
   game-side rule from the doctrine ("destroy only after no recorded or executing frame uses
   them") is implemented here, once, instead of being every caller's problem.
3. **The `PictureID` recycles only after both** — `mu_id_pool_release` runs after the entry
   is fully disarmed, so a game that kept the id gets a no-op, not a use of a dead texture.

At shutdown the doctrine's sequence applies unchanged: `wait_idle`, drain every
`DeleteQueue`, then destroy the atlas and the device.

---

## 5.5 Failure modes

| Symptom | Cause | Fix |
|---|---|---|
| Draw count ≈ sprite count | sorting by bindless slot instead of dense index | §5.1: `dense_index` in the key |
| Color bleeding at sprite edges | no gutters, or UVs not texel-aligned | pack with 1-texel padding, inset UVs half a texel |
| Device lost / validation error on evict | destroyed an image a recorded frame still samples | §5.4: `delete_queue_defer`, never direct destroy mid-run |
| Sprites vanish for one frame after load | picture became sampleable a frame late | mid-frame path must copy in frame N's command buffer (§5.3) |
| Atlas overflow assert mid-game | shelf packer full, game keeps loading | fail loudly at load; raise atlas size or evict first — never silently wrap |
| Stutter when a picture loads | one-time submit mid-frame | §5.3: use the staging ring, not a second submission |

The one-line summary of this article: **the atlas is a batching decision, not a memory
decision.** The memory table above is the justification; the dense index is the mechanism.

**Next:** [06 — CPU locality: the packing loop](06-cpu-locality-packing.md), which stays on
the CPU and asks what the 0.7 ms of measured prep time is actually made of, and what the
game-side API must look like for the answer to be "as little as possible".