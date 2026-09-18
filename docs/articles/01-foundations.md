# Article 1: Foundations — Why a Graphics Library Makes the Best Editor

## The premise

Most code editors are built the same way: a generic UI framework (ImGui, Qt, VS Code's web stack) layered on top of a text-processing engine. The editor inherits the framework's rendering model, its memory habits, and its latency profile. You get a working editor, but you also get everything you didn't ask for — compositing layers, layout engines, theme systems — and you pay for all of it in memory, in CPU time, and in the opacity of the rendering pipeline.

This article series proposes a different approach: build a code editor directly on top of a low-level Vulkan graphics library. Not a UI framework. Not a widget toolkit. A graphics library that gives you a render pass, a draw call, and a handful of GPU resources — and nothing else.

The library is `mu_gfx`: a C-based Vulkan renderer with a `begin_pass` / `end_pass` API, bindless textures, per-frame linear pools, timeline-based submission, and a `cmd_draw` primitive that takes a `ByteSpan` root payload. It was built for rendering graphics. But its primitives map almost perfectly onto what a text editor needs to render: rectangles, text, and one scissor.

## Why this matters

The performance characteristics of a code editor are dominated by three things: how fast a keystroke becomes visible pixels, how much memory the editor consumes when idle, and how predictable the frame budget is. A generic UI framework fights you on all three:

- **Memory**: UI frameworks retain layout trees, style caches, and widget state for every element on screen, even when invisible. A code editor has one thing to show: text lines in a viewport.
- **Latency**: Every intermediate compositing step adds a frame of delay. If the framework needs a layout pass, a paint pass, a composite pass, and then a Vulkan submission, you've added four things between the key press and the pixel.
- **Predictability**: Framework frame budgets vary because the framework does work you don't need. A text editor's frame is either "render visible lines" or "sleep."

By starting from the graphics library, we eliminate the middle layer entirely. The editor's rendering is one pass, one draw call, one shader. There is no widget tree, no layout engine, no compositor.

## What the graphics library gives us

The `mu_gfx` renderer provides the following facilities that an editor can reuse without modification:

| Facility | Editor use |
|---|---|
| `begin_pass` / `end_pass` | Declare the text pass with color attachments, load/store ops, and clear colors |
| `cmd_draw` + `ByteSpan root` | Submit all visible glyph instances in one draw with a single root payload (viewport, atlas ID, palette ID) |
| Bindless texture set | Font atlas pages and the color palette are just two more texture IDs |
| `r->cpu_pool` | Per-frame linear allocator for glyph instances — reset every frame, zero allocations on warm cache hits |
| `r->staging_pool` | Ring buffer for the rare atlas re-bake upload |
| Timeline + DeleteQueue | Retire old atlas pages after the final submission that uses them |
| `GpuProfiler` | Time the text pass alongside every other pass |
| GLFW backend | Window, keyboard/text input, clipboard, event waiting |
| `mu` containers | `mu_array`, `mu_allocators`, `mu_bitset`, `mu_perf`, `mu_pcg` |
| Vendored stb | `stb_truetype` for rasterization, `stb_rect_pack` for atlas packing |
| Vendored dmon | File watching for external-change detection |

The editor is therefore approximately: **CPU-side data structures + one shader + one pass function + input handling**. The renderer is not an abstraction to fight — it is the substrate.

## What the editor must build

The renderer does not provide:

- **Document storage**. No gap buffer, no line index, no byte offsets. The editor owns the text.
- **Text layout**. No glyph instance generation, no line wrapping, no cache invalidation.
- **Syntax highlighting**. No lexer, no token streams, no incremental relex.
- **Input semantics**. No cursor, no selection, no undo/redo, no clipboard.
- **File I/O**. No open/save, no file watching integration, no atomic writes.

These are the editor's problems, and they are CPU problems. The GPU's job is simple: render what the CPU gives it.

## The design constraints

Everything in this series follows the engineering doctrine embedded in `AGENTS.md`:

1. **Data locality over asymptotic complexity.** A gap buffer moving 1 MB is ~100 µs on modern RAM. A rope or piece tree paying pointer-chasing overhead for the same operation is slower at editor-scale files (≤ 10 MB, fits in L3).
2. **Zero hidden allocation.** Steady-state editing and scrolling allocate nothing. Per-frame scratch comes from the existing per-frame linear pool.
3. **IDs over pointers.** Lines, glyph runs, and atlas entries are referenced by index, never by pointer into another array.
4. **Preparation vs execution.** Edit → index update → relex → layout produce flat instance data. Rendering is one unbranched draw.
5. **Measure first.** Every stage timed with `mu_perf` + `GpuProfiler`. No optimization without a number.
6. **Simple over abstract.** One draw call. One shader. No second Vulkan abstraction, no ImGui.

## The first release

The first useful release includes: open/save, tabs with one active view, caret and selection, clipboard, word/line navigation, indentation, linear undo/redo, literal find, C-family highlighting, horizontal/vertical scrolling, DPI handling, and external-change notification.

It deliberately defers: soft wrapping, multiple carets, split views, semantic analysis, LSP, plugins, regex search, bidirectional layout, full shaping, and IME composition. These are real additional systems, not one-line extensions.

## What's next

Article 2 covers the foundation that everything else builds on: the document. We'll examine why a gap buffer plus an incremental line index is the right choice, how byte offsets serve as the single canonical position representation, and how the line index avoids the O(n) tail rewrite that kills naive implementations.

The editor is not a miniature VS Code. It is a direct-manipulation text renderer where every millisecond is accounted for, every byte of memory is intentional, and the graphics library is not a constraint — it is the advantage.
