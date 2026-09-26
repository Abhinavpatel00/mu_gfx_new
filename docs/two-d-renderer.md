# 2D renderer — current design

Ground truth doc for the shipped sprite renderer. Everything here was read
from the tree; file:line references are the authority, this file is the map.

Code: `src/two_d/sprite.h`, `src/two_d/sprite.c`, `src/two_d/sprite_data.h`,
`src/two_d/sprite_init.inl`, `src/two_d/sprite_pass.inl`,
`src/two_d/picture.c`, shaders `shaders/sprite.slang`,
`shaders/sprite_cull.slang`, `shaders/common.slang`.
Wiring: `renderer.c` (owns `SpriteSystem`), `renderer.h` (`GameHooks`,
`GameFrame`), `src/constant.h` (bindings), `compileslang.sh` (spv build).

Replaces the old `docs/articles/two-d-renderer/*` series, which described an
aspirational API (`SpriteRenderer`, `sprite_layer`, Option-A degenerate quads,
`gpu_pool`/`cpu_pool` slices) that was never shipped. Deleted because it no
longer matched any struct, function, or buffer in the tree.

---

## 1. Shape of the system

One `SpriteSystem` per `Renderer` (`renderer.c:98`, created
`renderer.c:790`, destroyed `renderer.c:1569`).

```c
sprite_begin(s, &camera);          // src/two_d/sprite.c:28
sprite_push(s, (SpriteDraw){...}); // src/two_d/sprite.c:44, up to 262144x
sprite_flush(s, cmd, target, load, clear); // src/two_d/sprite_pass.inl:20
```

`sprite_flush` is called once per frame by `pass_sprites`
(`src/two_d/sprite_pass.inl:137-140`), which draws into
`hdr_color[current_image]` with `LOAD_CLEAR`. The game hook
(`GameHooks.frame`, `renderer.c:1491-1502`) runs just before, pushing sprites
through `GameFrame.sprites`. Post chain (`post_pass`, `pass_smaa`,
`pass_ldr_to_swapchain`) treats sprites like any other scene content.

No allocation in the frame. CPU scratch (`instances`, `sorted`, `slot_of`) is
`malloc`'d once in `sprite_system_init` (`src/two_d/sprite_init.inl:61-67`).
GPU buffers (`stream`, `out`) are device-local, created once
(`src/two_d/sprite_init.inl:74-85`).

---

## 2. Data

### SpriteInst — 32 bytes (`src/two_d/sprite_data.h:57-66`)

```c
typedef struct { float x, y, w, h; uint uv0, uv1, color, key; } SpriteInst;
_Static_assert(sizeof(SpriteInst) == 32);
```

`x,y,w,h` are render-target pixels, camera transform already baked in by
`sprite_push` (`src/two_d/sprite.c:66-74`). Negative `w/h` mirror in place;
quad still spans `[x, x+abs(w)]` so the anchor does not move. `uv0/uv1` are
packed unorm16 pairs with half-texel inset baked at load time, so push costs
no division. `color` is linear RGBA8, 0 means untinted white. `key` is the CPU
sort key, never read by the GPU.

### SpritePush — 32 bytes (`src/two_d/sprite_data.h:71-80`)

One payload for every cull dispatch and every draw. `batch` is the draw's
batch index, 0 for compute. `cpu_off` / `gpu_off` are byte offsets of this
flush in `stream` / `out`. `texture_id` / `sampler_id` are bindless slots for
the draw. `viewport` is target size in pixels; cull rect is always
`[0, 0, viewport]`.

### Sort key (`src/two_d/sprite_data.h:22-36`)

`[31:24] layer | [23:20] blend | [19:18] sampler | [17:14] atlas | [13:0] variant`.
Plain ascending sort gives back-to-front painter order: layer 0 and blend 0
(opaque) draw first, end up behind. `variant` is currently always 0
(`src/two_d/sprite.c:74`).

### SpriteBatch (`src/two_d/sprite.h:68-75`)

Run of sprites sharing `key`. `first`/`count` are padded to 256 so the GPU can
index the batch by block; `real_count` is the true count, padding reads as
invisible (zero `w/h`).

### Limits (`src/two_d/sprite_data.h:10-18`, `src/two_d/sprite.h:96-100`)

| Cap | Value | Notes |
|---|---|---|
| `SPRITE_BLOCK_SIZE` | 256 | cull workgroup size, padding quantum |
| `SPRITE_MAX_INSTANCES` | 262144 | pushes beyond are counted in `dropped`, skipped (`sprite.c:45-48`) |
| `SPRITE_MAX_BATCHES` | 256 | distinct keys; `key_slot` asserts past it (`sprite.c:15`) |
| `SPRITE_BUFFER_CAPACITY` | max + batches*block | `sorted` scratch size |
| `SPRITE_MAX_ATLASES` | 4 | `atlas_create` fails past it |
| `SPRITE_MAX_LAYERS` | 256 | layer clamped (`sprite.c:63`) |
| `SPRITE_MAX_PICTURES` | 4096 | `picture_load_rgba` fails past it |
| `SPRITE_STREAM_REGION` | capacity*32 + scratch | one full-size flush per frame slot |
| `SPRITE_OUT_REGION` | max*32 | compacted stream per frame slot |
| `SPRITE_MAP_SIZE` | 1024 | key->slot hash, 2x batch cap, L1-resident |

Scratch layout inside `stream`, per flush (`src/two_d/sprite_data.h:40-49`):
instances, then `vis[block_count]`, `block_base[block_count+1]`,
`first_block[batch_count+1]` (uploaded, not computed), `batch_base[batch]`,
`indirect[batch]`. `SPRITE_CPU_FLUSH_END` is the total.

---

## 3. CPU half: begin / push / batch / prepare

`sprite_begin` (`sprite.c:28-42`): resets counts, bakes camera
(`origin = x,y` snapped if `snap`, `zoom`, default 1).

`sprite_push` (`sprite.c:44-75`): resolves `w/h` (0 = picture size), blend
(default alpha), sampler (`SPRITE_SAMPLER_DEFAULT` takes picture's, else
clamped), layer (clamped), color (0 = white). Transforms to pixels, copies
picture `uv0/uv1`, packs key. Straight-line, no allocation.

`sprite_batch` (`sprite.c:80-139`, static): one hash pass (`key_slot`,
Knuth-multiply + linear probe, `sprite.c:10-26`), insertion-sort of at most
256 slots by key ascending, lay out batches on block boundaries, one gather
`instances -> sorted`, zero-fill padding. Returns false when empty.

`sprite_prepare` (`sprite.c:141-175`): runs batch, computes
`padded/blocks/stream_bytes`, picks this frame's ring region
(`slot = current_frame`, reset on new timeline value), fails cleanly
(`dropped++`, return false) when the region is full. Several small flushes
share a region until full; one full-size flush fills it. Output `SpriteLayout`
(`sprite.h:155-162`): `stream_base`, `out_base`, `stream_bytes`,
`block_count`, `batch_count`, `padded_count`. Caller `sprite_flush` owns the
region after this.

Empty frame or prepare failure still clears the target
(`sprite_pass_clear_only`, `sprite_pass.inl:6-18`).

---

## 4. GPU half: upload, cull chain, indirect draws

`sprite_flush` (`src/two_d/sprite_pass.inl:20-135`):

1. Build `SpritePush pc` (`pass.inl:22-35`): batch/block counts, offsets,
   viewport from target size.
2. Upload (`pass.inl:42-54`): two `renderer_upload_buffer_to_slice` copies,
   sorted instances then the `first_block[batch_count+1]` table, sharing one
   `TRANSFER -> COMPUTE` barrier over the whole flush (`pass.inl:56-58`).
3. Three compute dispatches, same `pc` (`pass.inl:62-87`). See section 5.
4. One indirect draw per non-empty batch (`pass.inl:89-130`): rebind graphics
   pipeline only on blend change, per-batch `SpritePush dp` carries
   `batch/texture_id/sampler_id`, `cmd_draw_indirect` reads its
   `VkDrawIndirectCommand` from `stream`. Recording cost is `O(batch_count)`,
   never `O(sprite_count)`.
5. Records `last_instances/last_batches/last_draws` for HUD.

Vertex shader (`shaders/sprite.slang:32-64`): `batch_base[batch] + iid`
gives the compacted index, loads 32 bytes from `sprite_out`, builds a
4-vertex triangle-strip quad, mirrors via sign tests, `pixel_to_uv` to NDC,
unpacks uv/color. Fragment (`sprite.slang:66-70`): bindless
`textures[texture_id].Sample(samplers[sampler_id], uv) * color`.

Bindings: stream = binding 5, compacted out = binding 6
(`src/constant.h:12-13`, `sprite.slang:5-6`, `sprite_cull.slang:9-10`),
written once into the bindless set at init (`sprite_init.inl:87-103`).


---

## 5. Note: why three compute pipelines and three dispatches

Shipped chain (`shaders/sprite_cull.slang`, one entry per `.spv` via
`compileslang.sh:62-66`, created `src/two_d/sprite_init.inl:110-112`):

| Kernel | Dispatch | Work | Writes |
|---|---|---|---|
| `cs_count` | `block_count x 256` threads, 1 KB shared | per-256-sprite-block visible count (`sprite_visible`: abs + AABB vs viewport) | `vis[block]` |
| `cs_prefix` | `1 x 64` threads, 256 B shared | phase A: scan `vis` to `block_base[]` + total; phase B: per-batch output base + `VkDrawIndirectCommand` (`vertexCount=4, instanceCount=e-s`) from `first_block` table | `block_base`, `batch_base`, `indirect` |
| `cs_compact` | `block_count x 256` threads, 1 KB shared | per-block local scan + stable scatter `stream -> out` at `block_base[g] + local_prefix` | compacted `out` |

Barriers between each (`pass.inl:65-87`): count-write to prefix-read,
prefix-write to compact/vertex-read, compact-write to vertex/indirect-read.
`prefix` must see all `vis`; `compact` must see all `block_base`; the vertex
stage must see `out` and `indirect`. No barrier goes without removing the
dependency it guards.

Why 3 `VkPipeline` objects: Vulkan bakes entry + `numthreads` + groupshared
into the pipeline. `count`/`compact` need 256 threads and 1 KB shared;
`prefix` needs 64 threads and 256 B shared. One fused `cs_main` with a `mode`
branch would run `prefix` at 4x threads and 4x shared, add divergence to every
invocation, and hurt occupancy, to save 2 pipeline objects that are created
once and never in the hot path.

Why 3 dispatches even if pipelines were fused: order-preserving compaction
without atomics or CPU readback needs the scan barrier. Rejected options:

- Single-dispatch `atomicAdd` append: 1 pipeline, 1 dispatch, no prefix. But
  global atomics contend at 262k sprites and produce arrival order, not
  painter order, a layer/blend correctness bug. Per-batch atomics plus fixup
  just re-invents the prefix.
- CPU cull in `sprite_prepare`: deletes all compute, scratch, and 4 barriers;
  visibility is 4 compares per sprite. Re-adds `O(N)` CPU per-sprite work.
  Right call at small counts or grouped submission; keep the GPU path for
  large sparse scenes.


---

## 6. Pictures and atlases (`src/two_d/picture.c`, `src/two_d/sprite.h:47-66`)

`Picture` is a sub-rect of one atlas: `atlas/sampler/uv0/uv1/w/h`. UVs are
quantized at load with half-texel inset, so bilinear never bleeds and no
gutters are needed. `picture_load` (file via stb_image) and
`picture_load_rgba` (memory) both go through `atlas_reserve` + `atlas_upload`.

Packer: classic shelf, one open shelf per atlas (`picture.c:46-66`). Rects
abut exactly. `atlas_reserve` (`picture.c:163-194`): try existing atlases,
then grow the last (double height, `wait_idle`, image copy,
`picture.c:71-161`), then create a new atlas (2048x2048 initial, 8192 max).

Upload (`picture.c:199-254`): staging buffer + one submit per region,
`UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY`. Loading is expected before
frames are in flight; `atlas_grow` waits for idle because retiring the old
image must outlive every sampling frame.

No eviction, no defrag, no mipmaps (`mip_count=1`, `R8G8B8A8_SRGB`). Atlas
handles stay valid for process lifetime; `picture_system_destroy` destroys
textures only at shutdown.

---

## 7. Frame integration and lifetime

`renderer_frame` order (`renderer.c:1480-1549`): game hook pushes sprites,
bind bindless sets (graphics + compute), `pass_sprites` (this doc),
`post_pass -> pass_smaa -> pass_ldr_to_swapchain`, nuklear/capture, submit.
`pass_sprites` always `LOAD_CLEAR`s `hdr_color[current_image]`
(`sprite_pass.inl:137-140`).

Per-frame ring: 3 disjoint regions in each buffer (`MAX_FRAMES_IN_FLIGHT`);
`sprite_prepare` resets a slot's tails on a new timeline value. Safe because
a slot is reused only after its submission retires.

Destruction is immediate (`sprite_init.inl:133-144`): `wait_idle`, destroy
pictures/textures, destroy `stream`/`out`, free CPU scratch. Shutdown drains
delete queues before `destroy_device`, per repo policy.

Failures are explicit, never aborts in-frame: push past cap drops + counts;
prepare past region drops + counts; upload failure clears target and returns;
unknown `PictureId` or bad blend/sampler fall back or skip
(`sprite.c:45-63`). Init failures (`malloc`, buffer create) are fatal with
`log_fatal`, before any frame runs.

---

## 8. How to use (game side, `main.c` farm demo)

1. `GameHooks.start` (`main.c:235`): `picture_load` / `picture_load_rgba`
   once. Keep the returned `PictureId`.
2. `GameHooks.frame` (`main.c:826-860`): `sprite_begin(s, &camera)`, then
   `sprite_push(s, (SpriteDraw){.picture, .x, .y, .w/.h (0 = natural,
   negative = mirror), .layer, .blend, .sampler (or DEFAULT), .color})`
   per sprite.
3. Renderer flushes; read `s->last_instances/last_batches/last_draws/dropped`
   for HUD.

Camera (`sprite.h:77-82`): `x,y` = world top-left, `zoom` = pixels per unit,
`snap` = quantize origin. World `(wx,wy)` lands at `(wx-origin_x)*zoom`
pixels.

Change the count only by changing the algorithm, then measure per workload.

