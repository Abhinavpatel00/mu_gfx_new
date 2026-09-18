# Article 5: Font Atlas and Glyph Rendering

## The bitmap atlas approach

Text rendering for a code editor is a solved problem: bake glyphs into a bitmap atlas, render textured quads. The choice is not between good and bad text rendering — it is between bitmap atlases and everything else, and bitmap atlases win for fixed-size code text.

The comparison:

| Method | CPU/gen cost | GPU cost | VRAM | Quality at any zoom | Editor fit |
|---|---|---|---|---|---|
| **Bitmap atlas** | One-time bake, ms | One texture fetch/px | Small (one 2k² at code sizes) | Perfect at baked size | **Excellent** |
| **SDF** | Bake ~5–10× raster | Fetch + threshold | Same | Crisp under 2–3× scale | Good; overkill |
| **MSDF** | Needs offline toolchain | ~3 taps per pixel | Same | Crisp at any scale | Deferred |
| **Slug** | Per-glyph CPU curve work | Heavy VS eval | ~zero VRAM | Perfect at any scale | Deferred |

Editors run at 1–3 fixed sizes. Bitmap atlases are perfect at those sizes. SDF buys zooming we don't need. MSDF requires a C++ toolchain (against doctrine). Slug requires outline extraction, curve packing, and a shader port — the single largest work item in the project, justified only if DPI problems arise.

## The font system

The font system manages three things: font bytes, glyph metrics, and atlas resources.

```c
typedef struct {
    uint8_t      *source_bytes;  // retained by the font system
    uint32_t      byte_length;
    FontMetrics  metrics;        // precomputed per-glyph advance, bearing
    uint16_t      atlas_page;    // which atlas page each glyph lives on
    // ... per-face, per-size tables
} FontSystem;
```

The font system retains the source bytes while `stb_truetype` references them. Its header warns against untrusted font files — arbitrary downloaded fonts are not an initial input path.

Key entries are indexed by face, glyph, pixel size, and raster settings. Rectangle positions in the atlas are stable within published pages. Font or DPI changes create new generations — **never repack a live page behind cached UVs**.

## Atlas baking with stb_truetype

The baking process runs outside the draw loop, under a work budget:

1. For each needed glyph, call `stbtt_GetBitmap` to rasterize coverage into an R8 buffer.
2. Pack the glyph rectangles into an atlas page using `stb_rect_pack`.
3. Upload the atlas page to the GPU via the staging pool and timeline-based DeleteQueue.

```c
// Bake a single glyph into the atlas
stbtt_BakeFontBitmap(font_bytes, font_pixel_h, first_char,
    pixels, width, height, &x0, &y0, &x1, &y1,
    info + first_char - first_char, num_chars);
```

The atlas starts at 1024×1024 R8. It grows in power-of-two pages as needed. Each page is a separate texture with its own bindless index.

## Atlas page lifecycle

Atlas pages follow the same lifecycle as all GPU resources in mu_gfx:

1. **Create**: bake, upload, get a texture ID and bindless index.
2. **Use**: referenced by glyph instances in the current and future frames.
3. **Retire**: when no in-flight frame references it, defer destruction via the DeleteQueue with the timeline value of the last submission that used it.

This means font atlas re-bakes (for DPI changes or font switches) are hot-path events, not frame events. The old page is deferred. The new page is published between frames with fresh descriptor slots.

```c
// When DPI changes, create new pages and retire old ones
TextureID new_atlas = create_texture(r, &(TextureCreateDesc){
    .width = new_width, .height = new_height,
    .format = VK_FORMAT_R8_UNORM,
    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    .debug_name = "font_atlas"
});
// Publish new pages, retire old with DeleteQueue
delete_queue_defer(r, last_frame_timeline, destroy_texture, old_atlas_data);
```

## Glyph lookup

Glyphs are looked up by `(face, glyph_id, pixel_size)`. The cache maps this triple to an atlas page and UV rectangle.

Missing glyphs use a fallback until they are baked. The first release bakes ASCII plus a fallback marker. Additional glyphs are discovered on demand and baked lazily under a work budget.

The cache is byte-bounded: a configurable budget (e.g., 8 MB normal page budget) controls how many pages exist. Eviction removes unused font-size generations first.

## Glyph instance generation

During layout, each visible glyph produces a `GlyphInstance`:

```c
// For each glyph in a visible line:
GlyphInstance inst = {
    .x_px = cursor_x_px,
    .y_px = cursor_y_px,
    .glyph_index = atlas_glyph_index,
    .color_index = syntax_color_index,
};
```

The cursor advances by `stbtt_GetCodepointHMetrics(face, codepoint)->advance` divided by the font's pixel size. Tabs expand to `TAB_COLUMNS × advance` — no tab glyph is emitted.

The instances are written into the frame's linear pool as a contiguous array. The draw root carries the GPU device address and instance count.

## Monospace assumption

V1 assumes monospace text. This simplifies the cache to advance × count. But the instance format already supports proportional text if kerning is enabled later. The `GlyphInstance` structure has a `glyph_index` field — there is no constraint that all glyphs have the same advance.

Kerning for proportional text is a flat i16 table from `stb_truetype`. The layout loop checks the kerning pair and adjusts the advance. This is a layout-loop change, not an architecture change.

## What's next

Article 6 covers incremental syntax highlighting: the resumable C-family lexer, sparse checkpoint storage, and how style invalidation is separate from geometry invalidation.

The font system is the bridge between the document's bytes and the GPU's pixels. It is deterministic, bounded, and retireable — every resource has a defined lifetime.
