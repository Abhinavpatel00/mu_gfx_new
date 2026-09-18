# Article 9: Memory Architecture and Zero-Allocation Steady State

## The promise

The editor allocates nothing on warm cache-hit frames. Every frame's scratch comes from the per-frame linear pool (`r->cpu_pool`), which is reset by `frame_start`. This is not a hope — it is an invariant enforced by the allocator design.

The memory budget is explicit:

```
CPU = document capacities + line leaves/directory/tree + history + layout/style/checkpoints + font data/bitmaps + I/O snapshots + application/platform overhead
GPU = swapchain + atlas pages + upload regions + renderer/driver allocations
```

## Per-frame linear pool

The `r->cpu_pool` is a linear allocator already existing in the renderer. It is reset at `frame_start`. The editor writes glyph instances into this pool via bump allocation. The draw reads them via the GPU device address. The pool is freed automatically when the frame completes and the next `frame_start` resets the head.

This is the **zero-allocation guarantee** for rendering. On a warm cache hit, the editor allocates exactly:

- `visible_lines × max_chars_per_line × sizeof(GlyphInstance)` bytes.
- That is it. No malloc, no free, no pool growth, no fragmentation.

Worst-case instance count for a typical view: 60 lines × 200 chars × 16 bytes = 192 KB. This fits comfortably in the 32 MB pool. The pool asserts instead of growing.

## Document memory

The gap buffer is one contiguous allocation. For a 10 MB file, that is 10 MB. The line index is approximately 4 bytes per line plus leaf slack and metadata: a million tiny lines can make indexing a major fraction of document memory.

The blocked line-length index (Article 2) uses leaf blocks of 256 lengths, approximately 1 KiB payload per leaf. Blocks are allocated from a growable indexed pool. The directory is a dense array of block IDs. A flat segment tree over directory entries maintains aggregate byte counts and line counts.

- Resolve byte-to-line or line-to-byte by descending aggregate counts, then scanning at most one leaf.
- Sequential iteration carries the current leaf position — no tree lookup per line.
- Ordinary character edits change one line length and O(log block count) aggregates. No global tail-offset rewrite occurs.

## History memory

History uses dense records plus chunked byte payload storage:

- Dense records: `HistoryRecord` array, approximately 64 bytes per record.
- Chunked payloads: the actual inserted/deleted bytes, stored in a growable chunk pool.
- Budget: 16 MiB per document, 64 MiB application-wide.
- Eviction removes whole oldest groups.

The history memory is bounded and configurable. It does not grow unboundedly with editing.

## Layout cache memory

The layout cache is a dense array of rows. Each row is approximately 64 bytes plus the glyph instances. For 200 visible rows (8-line margin above and below a 60-line viewport):

- Row metadata: 200 × 64 bytes = 12.8 KB
- Glyph instances: 200 × max_chars × 16 bytes ≈ 200 × 200 × 16 = 640 KB
- Total: ~650 KB per active view

The configurable budget is 2 MiB per active view. Inactive views have their payloads evicted.

## Font atlas memory

Atlas pages are 1024×1024 R8 = 1 MB each. The initial budget is one page. The normal page budget is 8 MB (8 pages). Font generation overlap can temporarily exceed the normal budget while old frames drain.

Each page is a `Texture` with a `VkImage`, `VkImageView`, and `VmaAllocation`. The page is retired via the DeleteQueue when no in-flight frame references it.

## Upload regions

Instance upload is the frame pool bump allocation. At 144 Hz, a 320 KB upload is approximately 46 MB/s — often acceptable, but not zero. The upload region is per-frame-slot and independently reset. A region is reused only after its last submission completes.

The layout scratch is separate from host-visible GPU memory. CPU layout produces instances in the CPU pool; they are copied to the GPU upload region during preparation. The copy is bounded by the instance count and is the dominant upload cost.

## What is never allocated on the hot path

The following are never allocated during normal editing or scrolling:

- Document bytes (pre-allocated gap buffer)
- Line index entries (pre-allocated array, grows by `mu_array` doubling only on expansion)
- History records (pre-allocated dense array, grows by doubling)
- Glyph instances (bump-allocated from per-frame pool, never individually allocated)
- Atlas pages (pre-allocated, reused across DPI changes)
- Style runs (computed inline during layout, never stored)
- Token objects (never stored — recomputed during glyph generation)
- Event queue entries (pre-allocated fixed-capacity ring)

The only allocation on a warm cache hit is the instance data itself — exactly the memory needed to render what is visible. This is the measure of the editor's efficiency.

## Peak memory accounting

Peak memory is not just logical bytes. History and asynchronous save snapshots can each exceed the original file size. They must appear in peak-memory measurements.

For a 10 MB file with active editing:
- Document: 20 MB (gap buffer with spare capacity)
- Line index: 0.5 MB
- History: 16 MB (worst case)
- Layout cache: 2 MB per view
- Font atlas: 1–8 MB
- Upload regions: 0.5 MB per frame slot
- Application/platform overhead: 10 MB
- **Total peak: ~50–60 MB**

For a 100 MB file (read-only):
- Document: 200 MB (gap buffer, read-only guard)
- Line index: 5 MB
- All other subsystems: same
- **Total peak: ~220 MB**

These are measured budgets, not guessed limits. The editor reports its memory usage alongside its performance metrics.

## What's next

Article 10 covers the implementation roadmap: the six stages from CPU foundation to polished editor, the acceptance criteria for each stage, and the measurement workloads that validate every performance claim.

Memory architecture is the editor's foundation. A clear budget means predictable performance. A zero-allocation hot path means responsive editing. These are not features — they are the editor's operating conditions.
