# Article 10: Implementation Roadmap and Performance Gates

## The six stages

The implementation proceeds through six stages. Each stage runs and renders something. No stage is purely theoretical.

### Stage A: CPU Foundation

**Deliverable**: Gap buffer, flat line index, blocked line index, transactions/history.

**Gate**: Differential tests pass under ASan/UBSan. Benchmark both indexes.

The foundation is pure CPU logic with cheap invariants:
- Line index ≡ buffer scan (invariant check)
- Positions always on UTF-8 boundaries
- Replacements preserve all bytes except the replaced range

A `mu_pcg`-driven random edit/verify loop finds storage and index bugs in seconds. The fuzz test runs 100,000 deterministic random replacements and checks every byte-to-line position.

**Instrumentation starts here**: `mu_perf` timers around every subsystem. No instrumentation after the editor is finished.

### Stage B: Render Foundation

**Deliverable**: Configurable editor initialization, safe frame upload ownership, trusted font atlas, static text/rectangles.

**Gate**: Vulkan validation clean. `begin_pass` + `cmd_draw` renders a static string end-to-end.

The editor renderer profile omits HDR, depth, SMAA, capture buffers, scene pools, and demo resources. It reuses infrastructure without inheriting its default allocations. The initialization path must be configurable to omit the graphics demo's resources.

Upload ownership is the correctness prerequisite: establish one upload region per frame slot, or a timeline-retired ring. Reuse a region only after its last submission completes. Keep CPU layout scratch separate from host-visible GPU memory.

### Stage C: Useful Editor

**Deliverable**: Input, selection, undo/redo, clipboard, open/save, dirty state.

**Gate**: Byte-for-byte save/reload and injected I/O failures verified.

Open on the I/O worker into owned storage. Install the result on the main thread only for the live request. Build the index in bounded chunks while showing progress.

Save captures an immutable copy of the current bytes and revision. Write to a uniquely created temporary file, handle partial writes, flush/sync, then replace the destination. On POSIX, sync the directory when durable replacement is requested. Report failures and retain dirty state.

File watching via `dmon` observes external changes. Callbacks enqueue owned notifications — never mutate documents. Coalesce/debounce events and reconcile file identity/metadata.

### Stage D: Bounded Views

**Deliverable**: Horizontal/vertical culling, cache invalidation, long-line checkpoints.

**Gate**: Compare cached output/hit testing against uncached reference.

The layout cache must be provably correct: cached output must match uncached rendering for every visible line, every edit, every scroll position. Hit testing must match the visual geometry.

Long-line checkpoints must be built lazily under a work budget. A jump far into an uncached line must show a temporary loading region — never pretend its geometry is ready.

### Stage E: Incremental Features

**Deliverable**: Budgeted syntax, literal search, watcher reconciliation.

**Gate**: Incremental results equal complete reference results after convergence.

The incremental lexer must produce identical results to a full re-lex after it converges. This is verified by comparing against a reference implementation that re-lexes the entire document. The early-exit optimization must not change the output.

Search must find all matches. Next/previous navigation must not require retaining every match. The search must cancel and restart on document mutation.

### Stage F: Performance and Polish

**Deliverable**: DPI/font lifecycle, idle scheduling, optional smoothing, workload report.

**Gate**: Optimize only measured bottlenecks. The report must show P50/P99 for every metric.

DPI changes trigger atlas re-bakes via new generations — never repack a live page behind cached UVs. Idle scheduling: when clean, wait for events until the next blink/animation deadline. Optional smoothing: cursor and scroll exponential relaxation.

The workload report documents every measurement: CPU model, RAM, compiler flags, build type, resolution, DPI, refresh rate, present mode, frame depth. Report percentiles and worst observed stalls, not only averages.

## Acceptance criteria

The initial performance goals on declared reference hardware:

| Metric | Goal |
|---|---|
| Ordinary local edit preparation (P99) | Below 1 ms |
| Background maintenance | Within configured slice (0.5 ms per tick) |
| Heap allocations on warmed cache-hit frames | Zero |
| Periodic rendering while fully idle | None (except enabled caret blink) |
| Input-to-photon latency | One vsync interval (P50/P99) |

## Measurement workload

Test the following scenarios and report all metrics:

- **File sizes**: empty, 100 KiB, 10 MiB, 100 MiB (read-only), 1 MiB file with a million short lines, one multi-megabyte line.
- **Edit patterns**: local typing at beginning/middle/end, alternating distant edits, newline edits at leaf split/merge boundaries, large paste/delete, undo/redo.
- **Navigation**: cold jumps, cached scrolling, horizontal jumps within a huge line.
- **Syntax**: a comment edit propagating to EOF, canceled searches, edits during save.
- **System**: DPI changes, atlas pressure, resize/minimize, idle, sustained input bursts.

Track these measurements:
- CPU event-to-submit latency (P50/P99)
- CPU frame total and GPU text pass time
- Input latency (event timestamp vs submit timestamp)
- Allocations in steady state (debug build assert: frame arena head deltas == predicted instance bytes)
- Scroll smoothness (HUD shows visual − target error)
- Memory high-water marks
- Gap bytes moved, index bytes touched, scanned bytes, cache misses, emitted glyphs

## The dependency policy

Every dependency is justified by a concrete need and reviewed against the doctrine:

| Need | Source |
|---|---|
| Arrays, pools, spans, timing, RNG | `external/mu` facilities |
| Font rasterization and rectangle packing | Vendored `stb_truetype` and `stb_rect_pack` |
| File watching | Vendored `dmon` |
| Window, clipboard, committed text | Current GLFW backend |
| Main document editing/history | Purpose-built transaction layer |
| Small search/path fields | `stb_textedit` (evaluated separately) |
| C-family lexical helpers | `stb_c_lexer` (reference only) |

C++ standard-library facilities are **never** used in project code, examples, or tests. Nuklear/ImGui is not used for editor rectangles — the editor UI is text + quads rendered through the same pipeline as the document. No second Vulkan abstraction. No renderer wrapper.

## The decision log

Every major choice in this project follows the same pattern: it was chosen, the alternatives were rejected, and the concrete signal that would make us revisit it is documented.

The decisions are:
1. **Gap buffer + blocked line index** over rope/piece table — revisit at 100+ MB multi-cursor files
2. **Byte offsets as canonical positions** — never revisit
3. **Flat line index** over tree/hash — revisit at millions of lines
4. **Splice-or-rebuild threshold** (measured) — revisit if either path misprices by > 2×
5. **Exponential relaxation for cursor/scroll** — revisit if the feel is wrong (5-line change)
6. **Scroll in pixels, not lines** — never revisit
7. **Layout cache keyed by (line, generation)** — revisit if per-frame layout beats cache maintenance
8. **Bitmap atlas first** — revisit on multi-DPI quality complaints
9. **Instances bump-allocated per frame** — revisit at 144 Hz upload pressure
10. **One draw call, `SV_VertexID` quads** — never revisit for v1
11. **Highlighting computed during layout; tokens never stored** — revisit when a stored-token consumer is scheduled
12. **Palette as 16×1 texture** — never revisit
13. **UTF-8 byte offsets without unicode library** — revisit on IME/CJK
14. **Input events queued with timestamps** — never revisit
15. **Reuse renderer wholesale** — never revisit
16. **Correctness by fuzz before pixels** — never revisit

Each decision is optimized for **cheap reversal** — seams, not predictions. Most decisions end with a low reversal cost, which is deliberate.

## The first release

The first useful release includes: open/save, tabs with one active view, caret and selection, clipboard, word/line navigation, indentation, linear undo/redo, literal find, C-family highlighting, horizontal/vertical scrolling, DPI handling, and external-change notification.

It deliberately defers: soft wrapping, multiple carets, split views, semantic analysis, LSP, plugins, regex search, bidirectional layout, full shaping, and IME composition.

## What's next

Article 11 covers smooth cursor animations, cursor trails, and smooth scrolling: the exponential easing system that bridges instant logic and smooth visuals, the display-only caret animation, and the fading afterimage trail.

## Final word

This editor is not built to compete with VS Code or Neovim. It is built to be the fastest, most memory-efficient code editor possible on a single screen, using a single GPU context, with zero abstraction overhead. Every line of code earns its place. Every millisecond is accounted for. Every byte of memory is intentional.

The graphics library is not a constraint. It is the advantage. It gives us a render pass, a draw call, a GPU allocator, and a timeline. The editor fills in the rest: the document, the layout, the syntax, the input. Simple. Fast. Predictable.
