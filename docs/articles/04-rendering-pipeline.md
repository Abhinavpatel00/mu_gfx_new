# Article 4: The Rendering Pipeline — One Pass, One Draw

## The rendering philosophy

A code editor's rendering is not a graphics problem. It is a data preparation problem followed by a single GPU submission. The graphics library handles the hard part — Vulkan synchronization, pipeline barriers, swapchain management. The editor only needs to produce a flat array of glyph instances and submit them.

The entire text rendering is **one `begin_pass` + one `cmd_draw`**. There is no separate pass for selection, for the caret, for the background, for the gutter. All of it is rectangles and glyphs in the same pass, same shader, same draw call.

```c
// The text pass in its entirety:
PassAttachment color = { .texture = r->hdr_color[r->swapchain.current_image].id, .load = LOAD_OP_KEEP };
begin_pass(cmd, &(PassDesc){ .colors = &color, .color_count = 1, .pipeline = r->text_pipeline });

push_constants(cmd, sizeof(TextRoot), &(TextRoot){
    .viewport_transform = { ... },
    .atlas_id = atlas_id,
    .palette_id = palette_id,
    .instance_addr = instance_gpu_address,
    .instance_count = instance_count,
    .quad_addr = quad_gpu_address,
    .quad_count = quad_count,
});

cmd_draw(cmd, BYTE_SPAN(push), instance_count * 6, 1); // 6 verts/quad, SV_VertexID → corner
end_pass(cmd);
```

## The instance format

Each glyph is a 16-byte record. Structure of Arrays packing keeps the GPU cache hot during vertex fetches.

```c
typedef struct ALIGNAS(16) GlyphInstance {
    float    x_px;       // viewport-relative X
    float    y_px;       // viewport-relative Y
    uint16_t glyph_index;  // atlas tile index
    uint16_t color_index;  // palette row (syntax color)
} GlyphInstance;
```

Six vertices per glyph. The vertex shader uses `SV_VertexID` to compute the quad corner — no vertex buffers, no attribute fetches. The instance is read exactly once per glyph.

This is not an optimization. It is the natural consequence of the data-oriented design: a contiguous array of glyphs, a single draw call, and a shader that does almost no work per pixel.

## The root payload

The `ByteSpan` root carries everything the draw needs:

```c
typedef struct ALIGNAS(16) TextRoot {
    float    viewport_transform[4][4]; // 2D transform for viewport
    uint32_t atlas_id;
    uint32_t palette_id;
    uint64_t instance_gpu_address;
    uint32_t instance_count;
    uint32_t quad_addr;  // GPU address of quad vertex buffer
    uint32_t quad_count; // 6 * instance_count
} TextRoot;
```

The root is 256 bytes exactly — the `PUSH_CONSTANT` macro guarantees this. The shader reads the viewport transform, the atlas ID (for bindless texture indexing), and the palette ID. That's it. One texture fetch per fragment. One palette lookup. One blend.

## The fragment shader

The fragment shader is minimal:

1. Fetch coverage from the atlas (R8, bindless texture).
2. Look up color from the palette (16×1 texture or root array).
3. Decode sRGB → linear.
4. Apply coverage-scaled alpha with `ONE / ONE_MINUS_SRC_ALPHA` blending.

The palette is a tiny 16-row texture. Theme changes recolor the palette — **not the atlas**. This means switching from light to dark theme never re-bakes a single glyph. The cost is one descriptor write.

## What's drawn

The instance stream contains:

1. **Background rectangles** — editor background, gutter background, selected-line highlight. Drawn first, same shader, same draw call.
2. **Glyph instances** — text for all visible lines.
3. **Selection rectangles** — highlighted text ranges.
4. **Caret** — a blinking rectangle at the cursor position.
5. **Overlays** — scrollbar, status line, find/replace field.

All are quads with per-instance colors. The order matters for visual stacking but not for draw call count — one `cmd_draw` for all of them.

## Why one draw call

One draw call means:
- One root push.
- One scissor.
- One profiler span.
- One pipeline bind.
- Zero draw-call overhead.

The instance count for a typical view is 20,000–50,000 glyphs (60 lines × 400 chars/line). That's 120,000–300,000 vertices. A modern GPU processes millions of vertices per frame. The draw call is not the bottleneck — the CPU preparation is.

The preparation step writes instances into the frame's linear pool (`r->cpu_pool` bump allocation). The pool is reset by `frame_start`. This is the **zero-allocation-per-frame guarantee**: on a warm cache hit, the only allocation is the instance data itself, which is exactly the memory needed to render what's visible.

## Preparation vs execution

The separation is strict:

**Preparation** (CPU, one frame):
1. Determine visible line range from viewport.
2. For each visible line, check the layout cache.
3. If cached and valid, copy prepared instances to the frame pool.
4. If not cached, lay out the line: lex, generate glyphs, write instances.
5. Upload instances to GPU via the frame pool's device address.

**Execution** (GPU, one frame):
1. `begin_pass` — auto-transitions, viewport/scissor, binds pipeline.
2. `cmd_draw` — one call, all instances.
3. `end_pass`.

The CPU does all the work. The GPU does almost nothing. This is the correct balance for a text editor: the CPU cost of preparing instances is the real cost, and it scales with what's visible, not with the document size.

## What's next

Article 5 covers the font system: how `stb_truetype` bakes the atlas, how glyphs are looked up, and how the atlas pages are managed and retired.

The rendering pipeline is simple because the data preparation is rigorous. Every glyph instance is prepared once, uploaded once, and drawn once. No per-frame allocation, no per-frame synchronization, no surprises.
