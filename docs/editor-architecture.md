# Preferred architecture: a native, performance-first code editor

Status: proposed architecture, not an implemented subsystem or benchmark result.

This is an independent proposal. It does not replace `editor-design.md`. Its priorities are low interaction latency, predictable work, then memory footprint. Small draw counts and compact structures are means, not success criteria.

## 1. Decisions in brief

- Use the existing mu_gfx rendering API, but initialize an editor-specific rendering profile rather than the whole graphics demo.
- Keep the document CPU-only: UTF-8 bytes in a gap buffer, with a blocked line-length index. Move the gap on edits, never merely on navigation.
- Store canonical positions as byte offsets. Keep documents, views, and rendering state separate.
- Route every mutation through a replacement transaction. Build ordinary linear undo/redo into the foundation.
- Use fixed-height, unwrapped, monospace text first. Bound glyph output in both screen dimensions; exceptionally long lines get explicit scan budgets.
- Keep a bounded persistent CPU layout cache. Copy prepared instances into GPU upload storage whose reuse is covered by the submission timeline.
- Render a grayscale bitmap atlas, ordered solid rectangles, then text and overlays. Prefer a few simple draws over a mandatory single draw.
- Use a resumable C-family lexer with sparse state checkpoints. Styling may lag; editing must not wait for a document-wide relex.
- Keep document mutation and Vulkan work on the main thread. Use one I/O worker with owned immutable request/result buffers.
- Sleep when there is no work. Animate scrolling if desired; make caret movement immediate by default.

### First useful release

Include open/save, tabs with one active view, caret and selection, clipboard, word/line navigation, indentation, linear undo/redo, literal find, C-family highlighting, horizontal/vertical scrolling, DPI handling, and external-change notification.

Defer soft wrapping, multiple carets, split views, semantic analysis, LSP, plugins, regex search, bidirectional layout, full shaping, and IME composition. These are real additional systems, not promised one-line extensions. The first release is not a fully internationalized editor: it preserves arbitrary input bytes, supports defined UTF-8 navigation, and renders unsupported text with explicit fallback glyphs.

## 2. Existing infrastructure versus new work

Confirmed in the current workspace:

| Existing facility | Intended use |
|---|---|
| `main.c`: `begin_pass`, `end_pass`, `cmd_draw`, `BYTE_SPAN` | Pass declaration and draw submission |
| Bindless images/samplers and texture upload machinery | Font atlas pages |
| Submission timeline and `DeleteQueue` | Upload reuse and deferred resource retirement |
| `GpuProfiler`, `mu_time_now`, optional Tracy | CPU/GPU measurements |
| GLFW in the current build | Window, key/text events, clipboard, event waiting |
| mu containers, allocators, bitsets, PCG | CPU storage and deterministic tests |
| Vendored stb and dmon | Font rasterization/packing and file watching |

`NoGraphicsAPI` is a separate library in the workspace; this proposal targets the existing C renderer in `main.c`, not a simultaneous API migration. RGFW is vendored but is not the current application backend.

The renderer is not already a complete editor platform. Input dispatch, persistent text caches, file transactions, text layout, font management, and the editor-specific initialization path are new work. The current profiler UI uses ImGui; a standalone editor needs its own small text/rectangle HUD or external tracing.

### Integration prerequisite: upload lifetimes

`frame_start` currently waits for a frame slot and resets a shared `cpu_pool`. Waiting for one slot does not by itself make bytes used by another pending submission reusable. Before storing editor instances there, establish one of these explicit ownership mechanisms: one upload region per frame slot, or a timeline-retired ring.

**Preferred implementation:** frame-slot regions with independently reset heads. Reuse a region only after its last submission completes. Keep CPU layout scratch separate from host-visible GPU memory. This is a correctness requirement, not an optional optimization.

## 3. Ownership and module boundaries

| Owner | Data and lifetime |
|---|---|
| `Editor` | Dense document/view tables, active IDs, command dispatch, I/O mailboxes |
| `Document` | Byte storage, line index, revision, undo history, file identity and saved revision |
| `EditorView` | Document ID, caret/anchor offsets, preferred horizontal position, viewport, layout cache |
| Syntax subsystem | Language checkpoints and pending scan frontier |
| Font system | Font bytes, glyph metrics, lookup tables, atlas resources and generations |
| Render frame slot | Transient GPU data and final-use submission value |
| I/O job | Owned path/payload, request ID, document ID/generation, captured revision |

Use IDs for cross-system references. Temporary pointers into storage are borrowed and expire on the next mutation that can move or grow that storage. Closing a document invalidates its generation so late worker results cannot attach to a reused slot.

Keep ownership visible in function parameters. Do not build an object hierarchy, message bus, or virtual storage interface. A narrow document API is sufficient; implementation replacement does not require runtime polymorphism.

Proposed modules are document/history, view/layout, syntax, font/text rendering, platform I/O, and the editor application. Generic reusable index/container primitives belong in `external/mu` when implemented; editor-specific transactions and caches do not.

## 4. Document storage and line index

### 4.1 Byte gap buffer

A document owns one growable byte allocation and two gap boundaries. Logical reads map to at most two physical spans. File bytes do not need a trailing NUL.

- Navigation does not move the gap.
- Replacement moves the gap to the range start, expands it over deleted bytes, then consumes it with inserted bytes.
- Reserve once for a batch before applying it; growth is an explicit slow path.
- Choose spare capacity from measured workloads. Do not automatically double a large allocation without measuring peak memory.
- Open reads directly into an allocation that can become document storage, avoiding an unconditional second file-sized copy.

The gap buffer makes local typing, sequential scans, and ownership simple. It does not guarantee low latency for alternating distant edits: movement and growth remain O(document bytes). The blocked index removes a different source of per-keystroke tail work; it does not fix that fundamental tradeoff.

If measured distant-edit latency fails the acceptance workload, replace byte storage with a packed chunk tree. This is not a free swap: span iteration, indexing, history application, and tests need adaptation.

### 4.2 Preferred index: packed blocks of line lengths

Do not store a global absolute byte offset for every line. That makes typing at the start rewrite offsets for the entire remaining document.

Instead store unsigned byte lengths, **including LF terminators**, in leaf blocks. Always represent at least one logical line. A file ending in LF has a final zero-length line. CRLF remains two stored bytes; display and caret logic treat the terminator as one boundary.

Initial tuning candidate: 256 lengths per leaf, approximately 1 KiB of payload. This is a benchmark parameter, not a permanent public limit.

- Allocate leaves from a growable indexed pool; use block IDs, not individually allocated linked nodes.
- Maintain an ordered dense directory of block IDs.
- Maintain aggregate byte counts and line counts in a flat segment tree over directory entries.
- Resolve byte-to-line or line-to-byte by descending aggregate counts, then scanning at most one leaf. Sequential iteration carries the current leaf position instead of doing a tree lookup per line.
- Ordinary character edits change one line length and O(log block count) aggregates. No global tail-offset rewrite occurs.
- Newline insertion/deletion splices affected leaf ranges. Split full leaves; merge or redistribute underfilled neighbors at a lower occupancy threshold to avoid oscillation.
- A directory size/order change may rebuild its aggregate tree in O(block count). Batch paste/delete topology changes into one rebuild; do not rebuild for each inserted newline.

This trades a bounded leaf scan and extra metadata for less metadata movement during ordinary typing in high-line-count documents. Leaf topology changes still have global work. If that tail latency matters, a multi-level packed tree is the next step, not a hash table. Benchmark a flat-offset baseline alongside this proposal to verify the tradeoff on small files.

### 4.3 Exact replacement rule

All edits replace the half-open byte range `[a,b)` with a byte span. Resolve both offsets against the **old** index. At a line boundary, choose the following line; EOF belongs to the last line, including the final empty line when present.

Let A and B be the endpoint lines. Preserve the prefix from the start of A to a and the suffix from b through the stored end of B. New line lengths describe:

`prefix bytes + inserted bytes + suffix bytes`

Splice those lengths in place of lines A through B. Only inserted bytes need scanning for LF: the prefix cannot contain LF, and the suffix can only end in the old B terminator. When B is not the last document line, do not add an extra empty line after that terminator; the unaffected following line already exists. At EOF, retain the final empty line when required.

Specialize the no-newline, same-line case into one length adjustment. Bulk operations prepare replacement lengths once. Test this rule independently before adding block splits and tree aggregates.

### 4.4 API contract

Use byte offsets for mutations and canonical positions. A range read returns at most two borrowed spans for gap-buffer storage; a sequential reader hides the gap without copying whole lines.

Use the custom mu span appropriate to the implementation language. Pass every `Span`, `ByteSpan`, and `GpuRange` parameter by value. Pass owners and mutable scratch allocators by pointer/reference. Borrowed spans do not transfer ownership: owned-memory open needs an explicit transfer operation distinct from copying a span.

Document range/lifetime preconditions and assert programmer errors. External file/encoding errors use results and messages. Do not add allocation-failure recovery or turn the renderer wrapper into a Vulkan validation layer.

## 5. Transactions, history, and text semantics

A replacement transaction records the old byte range, inserted/removed byte counts, affected old/new line ranges, and old/new revision. It is the only mutation entry point used by typing, paste, undo, and replace.

A command may group replacements into one history transaction. For non-overlapping ranges expressed in one pre-edit coordinate system, apply in descending byte order; resolve overlaps before mutation. Reserve storage/history space once for the group.

Caret, selection anchor, and future marks are byte offsets. Transform them through replacements using a defined left/right insertion affinity. Do not store authoritative line/column alongside the offset. Preferred horizontal caret position is view state and resets on intentional horizontal navigation.

History uses dense records plus chunked byte payload storage, not a document snapshot per keystroke. Store inserted/deleted bytes for replay and caret/selection before and after the group. Coalesce adjacent typing under explicit command/time rules; paste and indentation are separate groups. New edits after undo discard redo.

Track a unique history state identity and the saved state identity. Undoing back to the saved state clears dirty status. Eviction must not make another state appear saved. Enforce a configurable byte budget by evicting whole oldest groups. For a single edit exceeding the budget, the preferred policy is to retain that group alone and report the temporary budget excess; never silently make part of a group non-undoable.

### Encoding and file fidelity

- Preserve file bytes, BOM presence, LF/CRLF, and a missing final newline. Do not normalize on load or save. Newline insertion uses the detected predominant style; explicit conversion is one undoable command.
- Decode UTF-8 with bounds checks. Invalid sequences consume one byte and display a replacement marker while preserving the original byte.
- Code-point navigation is the first-release contract, not grapheme navigation. Combining marks and emoji sequences can therefore require multiple key presses; document this limitation. Full Unicode needs segmentation, width, shaping, and bidi policies, not just a UTF-8 decoder.
- ASCII controls except recognized whitespace display visibly rather than executing terminal-like behavior. Embedded NUL is data. Treat CRLF as one caret boundary; a lone CR is a visible control in the initial LF-indexed model.
- Tabs advance to the next tab stop, not by a fixed number of columns regardless of position.
- Unsupported glyphs occupy a defined fallback cell. Do not claim Unicode terminal-width correctness. Layout and hit testing share the same decoding/advance rules.

## 6. Views, layout, and bounded caches

Use fixed line height and no soft wrapping initially. Represent vertical position as a line ordinal plus local pixel displacement, or use double precision for large document coordinates. Emit viewport-relative floats to the GPU, avoiding precision loss from huge document-space positions.

A view owns its caret/selection, target and displayed scroll, dimensions, font/layout settings, and bounded layout cache. Hit testing uses the displayed transform, not the scroll target. A document edit transforms the top-of-view byte anchor so editing above the viewport does not unexpectedly move the visible text.

### Cache organization

Use a dense array/ring of cached visible rows plus a small overscan margin, and a byte-budgeted payload pool. Cache only the horizontally useful glyph range and nearby margin. Records carry line ordinal, layout interval, text-valid and style-valid state, font metrics generation, and settings generation.

Each edit explicitly updates this small set:

1. Keep rows before the affected range.
2. Invalidate rows intersecting the old/new edited range.
3. Shift unaffected following row ordinals by the line-count delta.
4. Invalidate style separately wherever lexer state is unverified.
5. Preserve unaffected geometry on theme changes; rebuild on font metrics/tab-size changes.

A global document revision is useful for asynchronous result rejection, but must not force every row to rebuild after every key press. Cache payloads never point into mutable document bytes or frame upload memory.

### Long lines

Clipping in the shader is insufficient: it does not bound CPU decoding or generated instances. Emit only glyphs intersecting the viewport plus margin. Use sparse line-relative byte/advance checkpoints for long lines, built lazily under a work budget and held in a byte-bounded cache. After editing such a line, invalidate its checkpoints from the edit onward.

ASCII fixed-width segments can skip directly when their no-tab/no-special-character status is known. General text needs decoding to build checkpoints. A first jump far into an uncached pathological line may show a temporary loading region while scanning progresses; never pretend its exact hit-test geometry is ready. Prioritize the caret region, and do not silently drop editable text.

Bound work by bytes/time as well as lines: one line can contain the entire file. Font discovery, syntax scanning, and checkpoint creation follow the same rule. Normal cache misses may allocate/grow at preparation boundaries; steady-state hits and reserved local typing should not allocate. Track these separately rather than promising zero allocations forever.

### Selection and movement

Generate selection rectangles only for visible intersecting rows, with a defined newline selection policy. Derive up/down movement from preferred horizontal advance; tabs and fallback glyphs use the same mapping as hit testing. Clamp target and displayed scroll after resize or document shrink.

Caret position is immediate by default. Optional caret animation must not delay editing or selection. Wheel scrolling may use exponential relaxation, with immediate scrollbar/search jumps and a reduced-motion option. Exponential interpolation toward a fixed target does not itself overshoot. Snap tiny residual motion to zero so the event loop can sleep.

## 7. Incremental syntax and search

The initial highlighter is a C-family lexical highlighter, not a compiler. Keep resumable state sufficient for comments, continued strings/preprocessor lines, and supported raw-string delimiters. A pair of booleans is insufficient for all C++ lexical constructs. Unsupported languages start as plain text.

Store sparse incoming-state checkpoints at line boundaries and detailed style runs only for cached visible text. Keep checkpoints in packed blocks so edits do not rewrite a global absolute-offset array. An edit marks a downstream interval unverified and resumes scanning from the preceding valid checkpoint.

Stop propagation only at an unchanged, correctly remapped checkpoint whose incoming state matches the newly computed state. Equality at an arbitrary edited line is not enough. Retain old downstream states for convergence comparison, distinguishing comparison data from trusted state. Subsequent edits merge invalidation ranges and restart scans whose input changed.

Apply a per-tick byte/time budget. The lexer must pause within long tokens or lines. Until the state reaching a visible region is known, use plain styling rather than stale colors. A jump near EOF after an opening comment change may need substantial catch-up; this is an explicit limitation of sequential lexical dependencies.

Visible style runs contain line-relative byte ranges and palette indices. Geometry and style invalidate separately, so completing syntax need not repeat layout. Do not retain a token object for every token in every document.

Literal search runs incrementally over the span reader, including matches crossing the gap. Use a linear-time streaming matcher with bounded retained matches; prepare pattern state when the query changes. Searches carry a revision and cancel/restart on mutation. Next/previous navigation must not require retaining every match. Initial search is exact byte matching of the entered UTF-8 pattern, with optional ASCII-only case folding; an empty pattern has no matches. Unicode folding and regex are separate features.

## 8. Fonts and rendering

### Bitmap atlas first

Use `stb_truetype` for trusted bundled fonts and `stb_rect_pack` for R8 coverage pages. Start with ASCII and a fallback glyph; discover additional glyphs on demand. Rasterization happens outside draw execution under a work budget. Missing glyphs use fallback until ready.

The font system retains source bytes while stb references them. Its header warns against untrusted font files. Arbitrary downloaded fonts are not an initial supported input path; broad font loading needs a separately evaluated parser/isolation policy.

Key entries by face, glyph, pixel size, and raster settings. Keep rectangles stable within published pages. Font/DPI changes create new generations; never repack a live page behind cached UVs. Set cache/page budgets, evict unused font-size generations first, and retire resources after their final recorded and submitted use.

Appending to an existing image still requires synchronization against prior sampling and correct transitions. Prefer publishing prepared pages between frames with fresh descriptor slots when needed. Never overwrite descriptors referenced by pending work. Noncoherent mapped-memory flushes go through the engine upload path.

Bitmap rendering has a small shader and good fixed-size behavior, but stb has no font hinting. Test small-size readability at target DPI. SDF is an alternative for moderate scaling, not an automatic quality improvement. MSDF needs a generator/tooling decision and commonly samples one RGB texture. Slug uses preprocessed curve/band textures and analytic evaluation; it implies neither zero GPU storage nor stencil-then-cover. These are measured alternatives, not promised drop-in replacements sharing an identical instance format.

### Prepared data and draw order

Start with glyph records containing viewport-relative position, glyph/metric ID, and style data. Assert exact CPU/Slang sizes and offsets when implementing. Metrics supply bearings, dimensions, and UV bounds. Do not build a dense all-pairs kerning table for monospace text.

Prepare ordered batches: background/gutter/selection rectangles, glyphs, then caret/decorations/menus as stacking requires. Use one pass where practical, with a few draws and explicit scissors. Split glyph batches where atlas or pipeline state requires it; preserve visual order rather than sorting overlapping text blindly.

Vertex-ID quads are a good initial path. Six vertex invocations do not guarantee one instance-memory read per glyph. Compare indexed/instanced variants only if GPU measurements warrant it.

Initially resolve style colors into packed instance colors during preparation. Theme changes recolor the bounded visible payload, not the atlas. This avoids an unconditional palette texture lookup per fragment; revisit a shader palette if measurements favor it.

Use premultiplied linear RGB output with coverage-scaled alpha and ONE / ONE_MINUS_SRC_ALPHA blending. Decode theme sRGB colors before blending and match output conversion to the swapchain format so conversion happens once. R8 coverage is linear data. Test text edges and contrast, not only geometry.

### Editor renderer profile

Render directly to a suitable swapchain attachment where supported. Omit HDR, depth, SMAA, capture buffers, scene pools, and demo resources. Reuse infrastructure without inheriting its default allocations. Configurable initialization is a prerequisite if the current startup cannot omit them.

Copy prepared visible instances into frame-slot-owned upload regions. A 320 KB upload at 144 Hz is approximately 46 MB/s: often acceptable, but not zero. Measure straightforward per-rendered-frame copies first. Persistent GPU caches need evidence of upload/copy pressure.

## 9. Input, scheduling, and asynchronous ownership

GLFW key events trigger commands; committed character events insert text. Do not synthesize printable input from physical key codes. Keep callbacks short and queue timestamped events with owned text payloads. Coalesce replaceable motion/resize events, never text or key transitions. A capacity limit needs a spill/growth or drain strategy, not a release-build silent drop. Clipboard paste is one owned payload and one history group.

A tick processes events, applies transactions, updates visible layout, spends bounded time on syntax/search/checkpoints, then renders if dirty or animating. Process input promptly; do not schedule full-file work ahead of it. Present/acquire waits and in-flight depth affect latency even when CPU preparation is fast. Measure one versus multiple frames in flight before choosing the default.

When clean, wait for events until the next blink/animation deadline. Worker completion wakes the loop. Hidden/minimized windows do not submit frames, but I/O and command completion still progress. Pending incremental work schedules another bounded tick without requiring constant GPU rendering. Focus loss cancels transient drag/key state and reduces animation activity.

Keep one main-thread document writer. Workers never borrow mutable document spans. Jobs own inputs/results and carry document generation, revision, and request identity. Apply results only when those identities still match. Start with one I/O worker; syntax/search remain cooperatively budgeted on the main thread, avoiding full snapshots merely to add threads.

## 10. Open, save, and external changes

Open on the I/O worker into owned storage. Install the result on the main thread only for the live request. Build the index in bounded chunks while showing progress; the initial implementation does not expose partially indexed data as fully editable. Cancellation discards the job result. Do not use a blanket read-only cutoff as a substitute for measuring the actual storage limitations.

For asynchronous save, the main thread captures an immutable copy of the current bytes and revision, using the two document spans without first flattening into another temporary. This costs one document-sized allocation/copy and can stall for large documents: measure it and label it a slow path. A future chunked/COW snapshot needs its own ownership design. Never let the worker read a gap buffer being edited.

Write the snapshot to a uniquely created temporary file in the destination directory, handle partial writes, preserve required permissions, flush/sync, then replace the destination using platform facilities. On POSIX, sync the directory when durable replacement is requested. Report failures and retain dirty state; do not delete the original before successful replacement. Atomic visibility and crash durability are different guarantees.

Initially reject saving through symlinks unless the user explicitly chooses a resolved destination. Document that replacement changes inode identity, can break hard-link sharing, and may not preserve ACLs/xattrs without additional platform work. Check for external changes before replacement; a metadata check still leaves a race, so do not claim conflict-free compare-and-swap saving.

Serialize saves per destination. Mark the captured state saved only on success; edits made while saving remain dirty. Closing a document does not implicitly cancel a write already committing. Define cancellation points before starting the commit phase.

Watch the parent directory with dmon so replace-style saves are observed. Callbacks enqueue owned notifications, never mutate documents. Coalesce/debounce events and reconcile file identity/metadata; watcher events are hints, not proof of content equality. Distinguish own saves from external replacements. Offer reload for clean documents and a conflict choice for dirty ones; never silently overwrite unsaved edits.

At shutdown, finish/cancel jobs according to their commit state and join workers. Wait for GPU idle, drain every delete queue, then destroy editor resources and device. No recorded command buffer may retain a resource destroyed during font reload or shutdown.

## 11. Dependency policy

C single-header libraries are welcome where they reduce real work without imposing the document model. Pin versions, retain licenses, isolate implementation macros in one translation unit, and review input trust, allocation hooks, thread behavior, and platform support.

| Need | Preferred choice |
|---|---|
| Arrays, pools, spans, timing, deterministic randomness | Existing mu facilities; add reusable missing primitives there |
| Font rasterization and rectangle packing | Vendored `stb_truetype` and `stb_rect_pack`, subject to font trust policy |
| File watching | Vendored `dmon` |
| Window, clipboard, committed text | Current GLFW backend; no concurrent backend migration |
| Main document editing/history | Purpose-built transaction layer, not `stb_textedit` |
| Small search/path fields | Evaluate `stb_textedit` separately; its header discourages large-text use |
| C-family lexical helpers | Evaluate `stb_c_lexer` as a reference/helper, not assume incremental support |
| Full Unicode or shaping later | Evaluate appropriate libraries when scheduled; header-only packaging does not establish correctness |

Do not add Nuklear/ImGui merely for editor rectangles, or introduce another renderer abstraction. Third-party code may remain isolated as existing dependencies; new project code, examples, and tests must not use C++ standard-library facilities.

## 12. Memory accounting and performance gates

Account for committed capacity and peak temporary storage, not just logical bytes:

`CPU = document capacities + line leaves/directory/tree + history + layout/style/checkpoints + font data/bitmaps + I/O snapshots + application/platform overhead`

`GPU = swapchain + atlas pages + upload regions + renderer/driver allocations`

Line-length payload is approximately four bytes per line before leaf slack and metadata. A million tiny lines can make indexing a major fraction of document memory. Report leaf occupancy and aggregate-tree capacity. History and asynchronous save snapshots can each exceed the original file size; they must appear in peak-memory measurements. Do not count mapped GPU memory twice when reporting unified-memory systems.

Initial configurable budget candidates, to be tuned rather than treated as verified requirements:

| Resource | Starting policy |
|---|---|
| Layout/style/long-line cache | 2 MiB per active view; evict payloads for inactive views |
| History | 16 MiB per document with an application-wide 64 MiB target; see oversized-group exception |
| Atlas | 1024 x 1024 R8 pages, initially one, 8 MiB normal page budget |
| Instance upload | Start near 1 MiB per frame slot, reserve/grow at preparation boundaries |
| Syntax/search maintenance | Initial combined 0.5 ms per active tick; check budget at small byte batches |

These are application settings, not giant unconditional static arrays. Font generation overlap can temporarily exceed the normal atlas budget while old frames drain. Large viewports may require a larger visible cache/upload reservation; never assert on ordinary user content merely to preserve a guessed capacity. If a cache cannot hold all visible payloads, stream bounded batches rather than generating the entire document.

### Measurement workload

Record CPU/GPU model, RAM, compiler flags, build type, resolution, DPI, refresh rate, present mode, and frame depth. Run release measurements separately from sanitizer correctness runs. Report percentiles and worst observed stalls, not only averages.

Test empty files, ordinary 100 KiB and 10 MiB sources, a 100 MiB file, a million short lines, one multi-megabyte line, dense tabs, malformed UTF-8, and mixed line endings. Include:

- Local typing near the beginning/middle/end, and alternating distant edits.
- Newline edits at leaf split/merge boundaries; large paste/delete and undo/redo.
- Cold jumps and cached scrolling, horizontal jumps within a huge line.
- A comment edit propagating to EOF, canceled searches, and edits during save.
- DPI changes, atlas pressure, resize/minimize, idle, and sustained input bursts.

Initial goals on declared reference hardware: ordinary local edit preparation P99 below 1 ms, background maintenance within its configured slice, no heap allocations on warmed cache-hit frames, and no periodic rendering while fully idle except enabled caret blink. These are goals, not claims of achieved performance. Record large-operation stalls separately instead of hiding them inside ordinary typing statistics.

Timestamp each event through preparation and submission; count all events, not only the latest event in a batch. CPU event-to-submit is not input-to-photon. GPU timestamps measure execution; presentation/display latency needs appropriate presentation instrumentation or external measurement. Track queue waits, gap bytes moved, index bytes touched, scanned bytes, cache misses, emitted glyphs, and memory high-water marks.

## 13. Implementation order and acceptance

| Stage | Deliverable and gate |
|---|---|
| A: CPU foundation | Gap buffer, flat reference index, blocked index, transactions/history; differential tests pass under ASan/UBSan; benchmark both indexes |
| B: Render foundation | Configurable editor initialization, safe frame upload ownership, trusted font atlas, static text/rectangles; Vulkan validation clean |
| C: Useful editor | Input, selection, undo/redo, clipboard, open/save, dirty state; byte-for-byte save/reload and injected I/O failures verified |
| D: Bounded views | Horizontal/vertical culling, cache invalidation, long-line checkpoints; compare cached output/hit testing against uncached reference |
| E: Incremental features | Budgeted syntax, literal search, watcher reconciliation; incremental results equal complete reference results after convergence |
| F: Performance/polish | DPI/font lifecycle, idle scheduling, optional smoothing, workload report; optimize only measured bottlenecks |

A and B can proceed independently. Instrumentation starts in A/B, not after the editor is finished. Tests additionally cover empty/trailing lines, CRLF boundaries, edits spanning the gap, redo invalidation, saved-state eviction, checkpoint remapping, input queue pressure, stale worker generations, failed saves, and pending GPU resource retirement.

### Validation performed for this proposal

A temporary C99 harness checked the section 4 replacement rule and gap movement against a contiguous byte reference and full line rescan. It exhaustively covered short two-symbol documents and ranges, then ran 100,000 deterministic random replacements including arbitrary bytes: **142,225 replacements passed** with Clang AddressSanitizer and UndefinedBehaviorSanitizer. Every resulting byte-to-line position was checked.

This validates a small algorithmic model only. It does not validate an implemented blocked tree, history system, GPU path, Unicode navigation, or the performance goals above. Those remain explicit milestone gates.
