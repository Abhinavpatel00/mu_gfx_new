# mu_gfx Editor — Design Document

A minimal, high-performance native text/code editor rendered directly on top of the existing
mu_gfx Vulkan renderer. This document covers the architecture, the data-structure trade-offs,
the text-rendering comparison, and the implementation order.

Non-goals for v1: undo/redo trees, multiple cursors, IME/CJK shaping, ligatures, LSP, plugins,
find/replace UI beyond a bare minimum. The architecture must not *block* them later, but none of
them justify complexity now.

---

## 1. Ground rules (from AGENTS.md, applied to this project)

| Doctrine rule | What it means here |
|---|---|
| Data locality first | Document, line index, and glyph instances are contiguous arrays; no per-line heap nodes |
| IDs over pointers | Lines, glyph runs, and atlas entries referenced by index, never by pointer into another array |
| Zero hidden allocation | Steady-state editing and scrolling allocate **nothing**; per-frame scratch comes from the existing per-frame linear pools |
| Preparation vs execution | Edit → index update → relex → layout (preparation) produce flat instance data; rendering (execution) is one unbranched draw |
| Measure first | Every stage timed with `mu_perf` timers + existing `GpuProfiler`; no optimization without a number |
| Reuse `external/mu` | Growable arrays (`mu_array`), arenas (`mu_allocators`), bitsets (`mu_bitset`), timers (`mu_perf`) |
| Single-header libs allowed when they earn it | `stb_truetype.h` (already vendored) for rasterization; `dmon.h` for file watching |
| Renderer is the renderer | No second Vulkan abstraction, no ImGui. One `begin_pass` + one `cmd_draw` with a `BYTE_SPAN` root |

---

## 2. What we reuse from mu_gfx

The renderer already solved every "engine" problem the editor needs:

- **`begin_pass` / `end_pass`** — one declarative text pass targeting the HDR color RT (or straight
  to LDR), with auto-transitions and viewport/scissor handling.
- **`cmd_draw` + `BYTE_SPAN` root** — per-draw push constants for viewport transform, atlas id,
  palette id.
- **Bindless set-0** — font atlas texture + palette texture are just two more texture IDs.
- **`r->cpu_pool`** — per-frame linear pool, already reset in `frame_start`. All glyph instances for
  the frame are bump-allocated here; the draw reads them via a device address. Zero allocations,
  zero frees, zero per-frame Vulkan buffer churn.
- **`r->staging_pool`** — ring for the (rare) atlas re-bake upload.
- **GpuProfiler** — the text pass shows up like any other pass; no special instrumentation needed.
- **Timeline + DeleteQueue** — atlas texture rebuild retires the old image safely on hot-reload of
  the font or atlas-growth resize.

The editor is therefore mostly **CPU-side data structures + one shader + one pass function**
(`pass_text`), plus input handling.

---

## 3. Document storage — comparison and choice

| Structure | Cursor move | Local insert/delete | Read a line | Memory | Complexity | Notes |
|---|---|---|---|---|---|---|
| **Gap buffer** | O(n) memmove worst case, O(1) near cursor | O(1) at cursor | O(1) via line index | 1 byte/char + gap | Trivial | One contiguous block; memcpy is fast (5–20 GB/s); ideal for 1 cursor |
| **Piece table** | O(log n) w/ index | O(1) append | O(pieces) pointer chase | Low (never copies text) | Moderate | Original text immutable + append-only edits; great undo, terrible locality |
| **Rope** | O(log n) | O(log n) | O(log n) | High (nodes, balance) | High | Tree walking = pointer chasing; rebalancing |
| **Piece tree** (Figma/Taylor Brown) | O(log n) | O(1) append | O(log n) | Low | High | Piece table + B-tree over positions; the "best" but far from minimal |

**Decision: gap buffer + incremental line index.**

Reasons, per doctrine:

1. **Locality beats asymptotics at editor scale.** A 10 MB source file fits in L3. Moving the gap
   1 MB is ~100 µs; every alternative pays more *total* bytes-moved for the same operations at
   this scale.
2. **Predictability.** One flat buffer, one integer gap position. No tree invariants to break, no
   per-piece allocation.
3. **Single cursor** is the v1 model; the gap buffer's weakness (multiple distant cursors) is a
   non-issue until it isn't — and the API below hides storage so it can be swapped.

### 3.1 Storage API (storage-replaceable by design)

```c
typedef struct Document Document; // opaque; storage kind is an internal detail

typedef struct { uint32_t byte_offset; uint32_t line; uint32_t column; } DocPos;

bool     doc_open(Document *doc, Span bytes);          // takes ownership of scratch
bool     doc_open_file(Document *doc, const char *path);
void     doc_close(Document *doc);

uint32_t doc_line_count(const Document *doc);
uint32_t doc_line_byte_length(const Document *doc, uint32_t line); // excludes '\n'
Span     doc_line_span(const Document *doc, uint32_t line, Arena scratch); // contiguous view

void     doc_insert(Document *doc, DocPos pos, Span bytes);
void     doc_delete(Document *doc, DocPos a, DocPos b); // half-open [a, b)

DocPos   doc_next_char(const Document *doc, DocPos pos);
DocPos   doc_prev_char(const Document *doc, DocPos pos);
DocPos   doc_pos_from_byte(const Document *doc, uint32_t byte_offset); // line/column resolved
```

- All positions are **byte offsets into the buffer**; line/column are *derived* from the line index
  (section 4), never stored as primary state. This is what makes the storage swappable: the storage
  only has to answer "give me bytes [a,b)" and "insert/delete at byte X".
- `doc_line_span` for a gap buffer is contiguous **unless the gap is inside the line**; in that case
  it copies into `scratch` (the per-frame arena). Fast path = no copy. Callers must not hold the
  span across edits.
- No allocation functions exposed. The storage owns its memory; growth happens inside `doc_insert`
  (amortized doubling, `mu_array`-style realloc — *not* in the steady-state hot path).

---

## 4. Line index — the incremental workhorse

One growable array mapping `line → byte offset of line start`:

```c
typedef struct {
    uint32_t *line_start; // line_start[i] = byte offset of line i's first byte
    uint32_t  line_count;
    uint32_t  capacity;
    uint32_t  dirty_from; // first line whose offsets are unverified; UINT32_MAX = clean
} LineIndex;
```

**Build:** one pass over the buffer (`memchr('\n')`-style scan), O(n) once at open.

**Incremental update on insert/delete of n bytes at line L:**

1. The edit changes only line L's *length* → recompute nothing structurally unless the edited bytes
   contain `\n` (or deleted `\n`s).
2. If newlines were added/removed: rescan **only the edited region**, splice the line-start array
   (one `memmove` of the tail — cache-friendly, typically a few KB).
3. If not: shift all offsets after the edit point by ±n — one `memmove` over
   `(line_count - L) * 4` bytes. For a 5k-line file that's ≤ 20 KB moved per edit; ~1–2 µs.

Measured budget target: **< 10 µs per edit** for a 10k-line file. If the edit is a paste of
thousands of lines, fall back to a full rebuild (O(n), still < 1 ms) rather than pathological
splices — rebuild is *simpler and faster* above a splice-size threshold (measure to pick it, ~1–2k
affected lines).

`mu_multi_index` (key→many hash index) was considered and **rejected** for this: line lookup is by
ordinal, not by key, and a flat array is strictly better on every doctrine axis here. It remains
the right mu primitive if we ever add symbol/bookmark jump indexes.

---

## 5. Cursor & selection — logically immediate, visually smooth

```c
typedef struct {
    DocPos   anchor;        // selection anchor (== cursor when no selection)
    DocPos   cursor;        // byte offset + derived line/column
    uint32_t goal_column_px; // sticky horizontal target for up/down moves
} TextCursor;
```

- **Logic is immediate**: `cursor` is exact the instant a key is pressed. Nothing in editing or
  layout waits on animation.
- **Visual interpolation is display-only**: the renderer keeps
  `visual_cursor_px` (float) and relaxes it toward the pixel position of `cursor` each frame:

  ```c
  float k = 1.0f - expf(-cursor_speed * dt); // frame-rate independent exponential smoothing
  visual_cursor_px += (target_px - visual_cursor_px) * k;
  ```

  Same form for scroll (section 6) — one shared helper. At `cursor_speed ≈ 25/s` the cursor settles
  in ~80 ms; under fast key repeat it lags by a few px, which reads as smoothness, not lag.
- **Sticky column**: up/down moves target the same *pixel column* of the previous cursor position
  (stored in px, not chars — tabs make char columns wrong). Clamped per line via the line index.
- Selection = [min(anchor, cursor), max(anchor, cursor)); rendered as a separate instance list
  (section 8) — selection quads are *not* text instances.

---

## 6. Viewport & scrolling — continuous, frame-rate independent

```c
typedef struct {
    float    scroll_y_px;   // document-space px of viewport top  (target)
    float    visual_y_px;   // what's rendered this frame
    float    scroll_x_px;
    float    visual_x_px;
    float    viewport_w_px, viewport_h_px;
    float    line_height_px;
} Viewport;
```

- **Document state and view state are fully separated.** Nothing in `Document`/`LineIndex` knows
  about pixels; nothing in `Viewport` is authoritative about text.
- `scroll_y_px` is the *target* set by wheel deltas / cursor-follow; `visual_y_px` relaxes toward it
  with the same exponential form as the cursor (`scroll_speed ≈ 12/s` for wheel, instant for
  scrollbar-style jumps if any).
- Wheel input accumulates into the target in **pixel units** (lines × `line_height_px`), so smoothing
  is identical at every DPI and font size.
- **Cursor-follow (type-on-last-line):** when the cursor moves out of the visual viewport, the
  target is set to bring it `margin_px` inside; smoothing gives the elegant glide for free.
- Clamping (`0 ≤ scroll ≤ doc_height_px − viewport_h`) applies to the *target*; the visual value
  overshoots briefly during smooth scroll — intentional, it reads as weight.
- Visible line range each frame: `first = floor(visual_y_px / line_height_px)`,
  `count = ceil(viewport_h / line_height) + 1`. Layout only touches this range plus the cache
  margin (section 7).

---

## 7. Text layout & glyph generation — visible lines only

Per visible line (plus `CACHE_MARGIN = 8` lines above/below):

```c
typedef struct { // 16 bytes / glyph instance, SoA-packed below
    float    x_px, y_px;      // document-space position
    uint16_t glyph_index;     // atlas tile
    uint16_t color_index;     // palette row (syntax color)
} GlyphInstance;
```

- **Layout cache**: ring keyed by `(line_number, edit_generation)`. A line's instances are rebuilt
  only if the line changed (generation bump from the line index) or it leaves the margin range.
  Scrolling alone rebuilds **nothing** for cached lines — it only changes which lines are drawn and
  the single viewport offset in the root push.
- Font metrics precomputed once: per-glyph advance, bearing, kerning pairs (one flat i16 table from
  `stb_truetype`). Monospace assumption for v1 simplifies the cache to advance×count — but the
  instance format already supports proportional text if we enable kerning later.
- Instances are written into the **frame's linear pool** (`r->cpu_pool` bump) as a contiguous
  `GlyphInstance` array; the draw's root carries `{instance_gpu_address, instance_count, viewport}`.
  The pool is reset by `frame_start` — the "zero allocations per frame" rule is enforced by the
  allocator we already have.
- Tabs expand to `TAB_COLUMNS × advance` during layout; no tab glyph is emitted.
- Worst-case instance count = visible lines × max line width (bounded by viewport, typically
  < 20k instances ≈ 320 KB — fits the 32 MB pool trivially; assert instead of growing).

**Line-length guard:** lines longer than the viewport width still lay out fully (v1); horizontal
smooth scrolling (section 6) exists precisely to navigate them.

---

## 8. GPU text rendering — comparison and choice

| Method | CPU/gen cost | GPU cost | VRAM | Quality at any zoom | Complexity | Code-editor fit |
|---|---|---|---|---|---|---|
| **Bitmap atlas** (stb_truetype bake) | One-time bake, ms | One texture fetch/px; trivial shader | Small (one 2k² at code sizes) | Perfect at baked size, blurry otherwise | Trivial | **Excellent** — editors run at 1–3 fixed sizes anyway |
| **SDF** (stb_truetype SDF bake) | Bake ~5–10× raster | Fetch + threshold (+ optional smoothing) | Same as bitmap | Crisp under 2–3× scale, soft corners | Low | Good; overkill for fixed-size text |
| **MSDF** | Needs offline/toolchain gen (no single-header C gen) | Fetch + per-channel median, ~3 taps | Same | Crisp at *any* scale incl. arbitrary zoom | Moderate (shader + atlas pipeline) | Good for zoomable UI text; not needed for v1 |
| **Slug** (analytic vector) | Per-glyph curve flattening during layout (CPU work scales with visible text) | Heavy VS curve eval + PS analytic coverage; needs stencil-then-cover (we have dynamic rendering + stencil) | **~zero** — no atlas at all | Perfect at any scale, any DPI | High (vendored HLSL shaders need port to Slang; needs outline extraction via stb_truetype's outline API) | The "endgame" quality option; biggest win is DPI independence, at real per-frame CPU cost |

Notes specific to us:

- `external/slug` is vendored but contains **only shaders + license/README** — it's an algorithm
  reference, not a drop-in runtime. Using it means: outline extraction (stb_truetype outline
  callbacks), curve packing in the layout step, shader port (HLSL → Slang), and a stencil-then-cover
  render path in `begin_pass`. All doable, none trivial.
- MSDF has no single-header C generator; adopting it means an offline tool step or vendoring
  `msdf-atlas-gen` (C++, against doctrine). Defer.

**Decision: bitmap atlas now, replaceable renderer behind two small seams.**

- The seams: (1) `GlyphInstance` (identical for every method), (2) `text_atlas_*` (bake/lookup).
  Swapping to SDF = same structure, different bake + 1-line shader change; swapping to Slug =
  replace instance generation per line (curves instead of quads) + a second pass with stencil.
- v1 bakes **one size** (the UI font size, with `@1x` at current DPI); re-bake on DPI change or
  font change is a hot-path event, not a frame event (ms-scale, on the timeline via DeleteQueue for
  the old atlas).

### 8.1 The text pass

One `begin_pass` + one `cmd_draw` for all text, plus selection/background quads in the same pass
(same shader, `kind` in the root-derived instance or separate instance lists — decided by
measurement in M4):

```
begin_pass: colors = { hdr_color[slot], CLEAR=editor_bg }
root = { viewport_transform, atlas_id, palette_id, instance_addr, instance_count, quad_addr, quad_count }
cmd_draw(r, cmd, BYTE_SPAN(root), instance_count * 6, 1)   // 6 verts/quad, SV_VertexID → corner
```

Fragment shader: one bindless texture fetch (atlas, R8), one palette row fetch, alpha blend.
The palette is a tiny 16×1 texture (or root array) so recoloring the theme never re-bakes glyphs.

---

## 9. Syntax highlighting — incremental, line-state based

- **Model**: per-line lexer with a single carry state: `(in_block_comment, in_string_continuation,
  paren_depth)`. State array is SoA parallel to the line index (`mu_bitset` for the two booleans +
  a `uint8_t` depth array).
- **Relex after an edit on line L**: relex L; if L's *outgoing* state differs from its stored
  incoming→outgoing pair, relex L+1, and so on until a line's state is unchanged (amortized: edits
  usually stop within 0–3 lines; a flipped `/*` relexes to the end — accepted, it's O(n) once and
  identical in every real editor).
- **Tokens → palette indices** during layout (not stored): keyword set = perfect-hash of ~40 words
  (static, from `mu_hash_table`-style open-addressing over a fixed table); strings/comments/numbers/
  preprocessor by the lexer; identifiers default. Lexing happens *inside* glyph generation so no
  token storage exists at all — zero memory, zero invalidation bookkeeping.
- Trade-off note: storing tokens per line (VS Code style) enables richer features (folding, match
  highlight) but adds a whole mutable token datastructure + invalidation. Rejected until a feature
  needs it; the lexer is storage-free so nothing is lost by deferring.

---

## 10. Input & latency

- GLFW key/mouse callbacks append **events with timestamps** into a per-frame event queue (fixed
  capacity, dropped-count asserted zero). Input processing = replay events over
  (document, cursor, viewport) at frame start, then the pipeline runs.
- **Input-to-photon budget** (measured, not hoped): event timestamp → submit → GPU ≈ 1 frame +
  present. We log P50/P99 of `frame_start timestamp − last event timestamp` in the existing
  profiler HUD; the number to beat is "one vsync interval".
- Text input uses GLFW's codepoint callbacks; UTF-8 internally (all byte-offset positions already
  treat bytes as bytes; `doc_next_char` advances over one UTF-8 sequence — 4-byte-prefix table, no
  lib).

---

## 11. File I/O & watching

- Open: read whole file (one `fopen/fread` into a scratch buffer → gap buffer init), line index
  build, first layout. Files up to ~50 MB fine; > 100 MB shows a warning and opens read-only (v1
  guard, one `if`).
- Save: gap buffer → one write (flush+fsync) — with the gap copied out contiguously.
- `dmon.h` (vendored) watches the open file for external changes → "file changed on disk, reload?"
  flag in the status line. Reload = full re-open (simple, correct).

---

## 12. Profiling plan (every item measurable from day one)

| Metric | How |
|---|---|
| Edit→ready (insert + index + relex + layout) | `mu_perf` around the preparation block; shown in HUD |
| Line-index update | timer inside `doc_insert` path (index update only) |
| Relex time | timer around the relex loop |
| Glyph generation | timer around visible-line layout loop |
| Instance bytes / count | counters next to the bump allocation |
| CPU frame total + GPU text pass | existing frame timers + `GpuProfiler` on the text pass |
| Input latency | event timestamp vs submit timestamp, P50/P99 |
| Allocations in steady state | assert: frame arena head deltas == predicted instance bytes; a `malloc` counter in debug builds must not move while typing/scrolling |
| Scroll smoothness | HUD shows `visual − target` error; oscillation or sawtooth is visible numerically |

The HUD reuses the existing profiler UI path; nothing new is built for it beyond text lines.

---

## 13. Implementation order (each milestone runs and renders)

1. **M1 — Document + line index + CLI test**: storage, index, insert/delete/positions; a tiny
   assert-based test over random edit sequences (no GPU). *Proves correctness of the foundation
   cheaply.*
2. **M2 — Atlas + text pass**: stb_truetype bake → atlas in bindless; `pass_text` renders one
   static string via `begin_pass`/`cmd_draw`. *Proves the render path end-to-end.*
3. **M3 — Editor shell**: window input → open a file, draw visible lines from the layout cache,
   fixed 1:1 scroll. Cursor blink (time-based, no smoothing yet).
4. **M4 — Editing + incremental everything**: insert/delete wired to key events; incremental index
   update, relex, cache invalidation. Selection quads. *This is the "real editor" milestone.*
5. **M5 — Smooth scrolling + smooth cursor**: float viewport, exponential relaxation,
   cursor-follow. Frame-rate independence verified at 60/144/uncapped.
6. **M6 — Syntax highlighting**: line lexer + palette + keyword hash.
7. **M7 — Profiling pass**: HUD numbers from section 12; fix whatever the numbers say — *in that
   order only*.
8. **M8 — Polish**: save (ctrl+s), dmon watch, open-file dialog, DPI change re-bake.

Steps 1 and 2 are independent and can proceed in parallel; everything after M4 is additive.

---

## 14. mu library / vendored usage summary

| Need | Use |
|---|---|
| Growable arrays (line index, cursor list later) | `mu_array` |
| Per-frame scratch (layout spans, instance upload) | existing `r->cpu_pool` (linear) |
| Fixed flags (block comment/string state per line) | `mu_bitset` |
| Keyword lookup | fixed open-addressing table (`mu_hash_table` pattern, static table) |
| Timers | `mu_perf` (`mu_time_now`) |
| RNG for edit-fuzz test | `mu_pcg` |
| Font rasterization + outlines (future Slug) | `external/stb/stb_truetype.h` |
| File watching | `external/dmon/dmon.h` |
| Slug algorithm reference (future) | `external/slug` (shaders only; port to Slang if/when pursued) |

Explicitly **not** used: cimgui (editor UI is our own text + quads), cglm (no matrix needs; the
root push carries a simple 2D transform), meshoptimizer/VMA (no new buffers beyond the atlas).

---

## 15. Risks / open questions

- **Gap buffer vs 100+ MB files**: memmove costs scale with file size; the read-only guard plus a
  future piece-table swap covers it. The storage seam (section 3.1) is the mitigation.
- **Proportional fonts / ligatures**: layout cache is the only consumer of advance data; enabling
  kerning later is a layout-loop change, not an architecture change.
- **Slug migration cost**: outline extraction + shader port ≈ the single largest chunk of work in
  this doc; only justified if bitmap quality at multiple DPIs proves insufficient in practice.
- **Selection + text in one draw vs two**: decided by measurement in M4 (default: one draw, quads
  first in the instance stream with `color_index = background`).
- **UTF-8 width in status line/column display**: byte columns vs display columns diverge; v1 shows
  display columns derived during layout (already computed), not stored.
