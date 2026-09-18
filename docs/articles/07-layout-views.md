# Article 7: Layout, Views, and Bounded Caches

## Visible lines only

The editor never lays out more text than is visible. For each frame, the visible line range is:

```c
uint32_t first_line  = floor(visual_y_px / line_height_px);
uint32_t line_count = ceil(viewport_h_px / line_height_px) + CACHE_MARGIN;
```

`CACHE_MARGIN` is 8 lines above and below the viewport. This margin prevents cache thrashing during slow scroll frames — a small price for avoiding re-layout every frame.

Layout only touches visible lines plus the margin. Scrolling alone rebuilds **nothing** for cached lines — it only changes which lines are drawn and the single viewport offset in the root push constant.

## The layout cache

The cache is a dense array/ring of cached visible rows. Each record carries:

```c
typedef struct {
    uint32_t line_number;
    uint32_t layout_generation;  // generation of the layout
    bool     text_valid;         // glyph instances are current
    bool     style_valid;        // syntax colors are current
    uint32_t font_metrics_gen;   // generation of font metrics
    uint32_t settings_gen;       // generation of tab size, etc.
    // glyph instances follow (SoA, packed)
} LayoutCacheRow;
```

The cache is keyed by `(line_number, edit_generation)`. A line's instances are rebuilt only if the line changed (generation bump from the line index) or it leaves the margin range. Scrolling alone rebuilds nothing for cached lines.

The cache payload never points into mutable document bytes or frame upload memory. It is a self-contained copy.

## Cache invalidation on edit

Every edit explicitly updates the cache:

1. **Keep rows before** the affected range.
2. **Invalidate rows intersecting** the old/new edited range.
3. **Shift unaffected following row ordinals** by the line-count delta.
4. **Invalidate style separately** wherever lexer state is unverified.
5. **Preserve unaffected geometry** on theme changes; rebuild on font metrics/tab-size changes.

A global document revision is useful for asynchronous result rejection, but **must not force every row to rebuild after every key press**. The cache handles per-row invalidation.

## The viewport and smooth scrolling

Document state and view state are fully separated. Nothing in `Document`/`LineIndex` knows about pixels. Nothing in `Viewport` is authoritative about text.

```c
typedef struct {
    float    scroll_y_px;   // target: document-space px of viewport top
    float    visual_y_px;   // what's rendered this frame
    float    scroll_x_px;
    float    visual_x_px;
    float    viewport_w_px, viewport_h_px;
    float    line_height_px;
} Viewport;
```

`scroll_y_px` is the target set by wheel deltas and cursor-follow. `visual_y_px` relaxes toward it with exponential smoothing:

```c
float k = 1.0f - expf(-scroll_speed * dt);
visual_y_px += (scroll_y_px - visual_y_px) * k;
```

This is frame-rate independent by construction (`k = 1 − e^(−λ·dt)`), has a constant settle time (~3/λ), and behaves correctly when the target changes mid-flight — which is the normal case while typing.

**Cursor-follow**: when the cursor moves out of the visual viewport, the target is set to bring it `margin_px` inside. The smoothing gives the elegant glide for free.

Wheel input accumulates into the target in **pixel units** (lines × `line_height_px`), so smoothing is identical at every DPI and font size.

## Long lines

Clipping in the shader is insufficient — it does not bound CPU decoding or generated instances. The editor emits only glyphs intersecting the viewport plus margin.

For lines longer than the viewport width, the editor uses **sparse line-relative byte/advance checkpoints** built lazily under a work budget. After editing such a line, invalidate its checkpoints from the edit onward.

ASCII fixed-width segments can skip directly when their no-tab/no-special-character status is known. General text needs decoding to build checkpoints. A first jump far into an uncached pathological line may show a temporary loading region while scanning progresses — **never pretend its exact hit-test geometry is ready**. Prioritize the caret region.

Work is bounded by bytes/time as well as lines: one line can contain the entire file. Font discovery, syntax scanning, and checkpoint creation follow the same rule. Normal cache misses may allocate/grow at preparation boundaries; steady-state hits and reserved local typing should not allocate.

## Selection and movement

Selection rectangles are generated only for visible intersecting rows. Up/down movement derives from the preferred horizontal position — tabs and fallback glyphs use the same mapping as hit testing.

Caret position is immediate by default. Optional caret animation must not delay editing or selection. Wheel scrolling may use exponential relaxation, with immediate scrollbar/search jumps. Snap tiny residual motion to zero so the event loop can sleep.

## What's next

Article 8 covers the input system: how GLFW events become editor commands, how the input queue is structured, and how latency is measured from event timestamp to GPU submission.

The layout cache is the editor's performance guarantee. It is what makes scrolling free and makes editing fast. Every other subsystem feeds into it or is fed by it.
