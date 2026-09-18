# Article 11: Smooth Cursor Animations, Cursor Trails, and Smooth Scrolling

## The philosophy: instant logic, smooth visuals

The editor's input and document logic is always instant. When a key is pressed, the cursor moves immediately, the document mutates immediately, and the scroll target jumps immediately. What the user perceives, however, should be smooth — not a hard snap from one position to another.

This article covers the three animation systems that bridge instant logic and smooth visuals:

1. **Smooth scrolling** — the viewport glides to the target position.
2. **Cursor animation** — the caret eases to its pixel position and blinks independently.
3. **Cursor trail** — a fading afterimage that shows the cursor's recent path.

All three share the same underlying easing primitive: **exponential relaxation**. One function, multiple parameters.

---

## Exponential relaxation: the shared primitive

The core easing function is frame-rate independent by construction:

```c
static inline float smoothstep(float current, float target, float speed, float dt)
{
    float k = 1.0f - expf(-speed * dt);
    return current + (target - current) * k;
}
```

**Why exponential?**

| Property | Value |
|---|---|
| Frame-rate independent | Yes — `k = 1 − e^(−λ·dt)` is derived from the continuous solution `y(t) = 1 − e^(−λt)` |
| Constant settle time | ~3/λ (reaches 95% of target in 3 time constants) |
| Target changes mid-flight | Correctly handles it — the easing always converges to the current target |
| No overshoot | Monotonic approach — the visual never exceeds the target |
| Zero allocations | Pure arithmetic — no heap, no state |

The `speed` parameter (λ) controls how fast the easing converges. A higher λ means faster convergence and a snappier feel. A lower λ means a more languid, visible glide. The default `scroll_speed` and `cursor_speed` are tuned separately because the editor expects scrolling to feel heavier and cursor movement to feel lighter.

This function is the **single shared helper** for both cursor and scroll. The difference is only the parameters passed.

---

## Smooth scrolling

### The dual-viewport model

The viewport separates the **target** position from the **visual** position:

```c
typedef struct {
    float scroll_y_px;   // target: document-space px of viewport top
    float visual_y_px;   // what's rendered this frame
    float scroll_x_px;
    float visual_x_px;
    float viewport_w_px, viewport_h_px;
    float line_height_px;
} Viewport;
```

- `scroll_y_px` is set immediately by wheel deltas and cursor-follow logic. The scrollbar thumb jumps instantly to its new position.
- `visual_y_px` is what the renderer actually draws. It relaxes toward `scroll_y_px` every frame using the exponential helper.

```c
viewport->visual_y_px = smoothstep(
    viewport->visual_y_px,
    viewport->scroll_y_px,
    viewport->scroll_speed,
    dt
);
```

### Pixel accumulation, not line accumulation

Scroll input accumulates in **pixel units**, not lines:

```c
viewport->scroll_y_px += event->delta.y * viewport->line_height_px;
```

This means the smoothing behavior is identical regardless of DPI, font size, or zoom level. A wheel notch always produces the same pixel delta and the same visual glide. The editor never converts between lines and pixels during the easing step — it stays in pixels throughout.

### Cursor-follow

When the cursor moves outside the visual viewport, the scroll target is set to bring the cursor `margin_px` inside the viewport:

```c
if (cursor_y < viewport->visual_y_px)
    viewport->scroll_y_px = cursor_y - margin_px;
else if (cursor_y > viewport->visual_y_px + viewport->viewport_h_px)
    viewport->scroll_y_px = cursor_y - viewport->viewport_h_px + margin_px;
```

The cursor-follow target adjusts **instantly**. The visual viewport then eases toward it. This produces the characteristic "gentle glide" where the text slides smoothly to follow the cursor rather than jumping abruptly.

### Scrollbar behavior

The scrollbar has two behaviors by design:

- **Scrollbar jumps are instant**: the `scroll_y_px` target moves immediately, and the scrollbar thumb follows it without delay. The user sees the scrollbar jump and the content glide — this is intentional and feels responsive.
- **Wheel scrolling is smooth**: the target is accumulated from wheel deltas and the visual viewport eases toward it. Exponential relaxation gives weight to fast wheel movements and a gentle deceleration at the end.

### Zero residual and sleep

When the visual position is within a pixel of the target, the residual is snapped to zero. This is important: it lets the event loop sleep when there is nothing to animate. The editor does not render a frame for a sub-pixel drift.

```c
if (fabsf(viewport->visual_y_px - viewport->scroll_y_px) < 0.5f)
    viewport->visual_y_px = viewport->scroll_y_px;  // snap to target
```

---

## Cursor animation

### Display-only visual cursor

The cursor position stored in the document is exact — the instant a key is pressed, `cursor` points to the correct byte offset. There is no latency between the logic and the data.

What the renderer draws is `visual_cursor_px`, a floating-point pixel position that relaxes toward the pixel position of the logical cursor:

```c
float cursor_target_px = cursor_to_pixel(cursor);
visual_cursor_px = smoothstep(visual_cursor_px, cursor_target_px, cursor_speed, dt);
```

This is the **same exponential helper** as scrolling, with a different `speed` parameter. The cursor speed is typically higher than the scroll speed so that the caret snaps quickly to its position — the user should not perceive a delay between their input and the caret appearing.

### Caret blinking

Caret blinking is **display-only**. It is a visual property of the rendered caret rectangle, not a modification of the cursor position. The caret rectangle is drawn at `visual_cursor_px` and toggled on/off based on a blink timer:

```c
float blink_phase = fmodf(blink_timer, blink_period);
bool caret_visible = (blink_phase < blink_visible_fraction);
```

The blink period is typically 1.0–1.2 seconds with a 50% duty cycle. The `caret_visible` flag only affects the renderer — it does not affect editing, selection, or any other logic. The cursor position is always exact regardless of the blink state.

### Why display-only matters

Making caret animation display-only means:

- A keystroke produces a cursor position change that is immediately correct in the document model.
- The visual position eases to the new pixel position.
- The blink continues unaffected by typing — no re-synchronization needed.
- The editor can skip rendering the caret entirely when the window is minimized or the frame budget is tight. No logic consequence.

### Cursor speed tuning

The cursor animation speed parameter is tuned so that:

1. **Local typing** (cursor moves one character): the visual cursor catches up in ≤ 2 frames at 60 Hz.
2. **Page jumps** (cursor moves thousands of pixels): the visual cursor glides smoothly, never overshooting.
3. **Rapid consecutive edits**: the easing always converges to the latest target, never oscillating or lagging behind.

If the feel is wrong, the adjustment is a single `speed` constant — a 5-line change, as the decision log records.

---

## Cursor trail

### What is a cursor trail

A cursor trail is a fading afterimage that shows the recent positions of the cursor. As the cursor moves, it leaves a series of progressively fading rectangles along its path. This provides:

- **Visual continuity**: the user can see where the cursor came from.
- **Orientation aid**: in a large file, the trail helps track cursor movement across the screen.
- **Aesthetic quality**: the trail gives the editor a polished, modern feel.

### Implementation

The trail is a circular buffer of recent cursor positions, each with an age and alpha value:

```c
#define TRAIL_LENGTH 12

typedef struct {
    float  position_px;   // pixel position along the scroll axis
    float  age;           // seconds since this position was recorded
    bool   active;
} TrailPoint;

typedef struct {
    TrailPoint points[TRAIL_LENGTH];
    int        head;       // next write position
    int        count;      // number of active points
    float      dt_accum;   // accumulated time since last trail point
} CursorTrail;
```

Each frame, the trail ages:

```c
for (int i = 0; i < TRAIL_LENGTH; ++i) {
    if (cursor_trail.points[i].active) {
        cursor_trail.points[i].age += dt;
        if (cursor_trail.points[i].age > trail_lifetime)
            cursor_trail.points[i].active = false;
    }
}
```

A new trail point is recorded when the cursor has moved a minimum distance since the last point:

```c
cursor_trail.dt_accum += dt;
if (cursor_trail.dt_accum >= trail_sample_interval) {
    float dist = fabsf(current_cursor_px - last_trail_px);
    if (dist >= trail_min_distance) {
        // Push new point, shift oldest out
        cursor_trail.points[cursor_trail.head] = (TrailPoint){
            .position_px = current_cursor_px,
            .age = 0.0f,
            .active = true,
        };
        cursor_trail.head = (cursor_trail.head + 1) % TRAIL_LENGTH;
        cursor_trail.dt_accum = 0.0f;
    }
}
```

The rendering draws the trail points with exponential alpha decay:

```c
for (int i = 0; i < TRAIL_LENGTH; ++i) {
    if (!cursor_trail.points[i].active) continue;
    float alpha = expf(-trail_fade_rate * cursor_trail.points[i].age);
    if (alpha < 0.02f) { cursor_trail.points[i].active = false; continue; }
    draw_caret(cursor_trail.points[i].position_px, alpha);
}
```

### Trail parameters

| Parameter | Default | Description |
|---|---|---|
| `trail_length` | 12 | Maximum number of trail points |
| `trail_lifetime` | 0.6s | How long a point remains visible |
| `trail_min_distance` | 4px | Minimum cursor movement to record a new point |
| `trail_sample_interval` | 0.02s | Minimum time between trail points |
| `trail_fade_rate` | 3.0 | Exponential alpha decay rate |

The trail is **entirely display-only**. The cursor position data is untouched. The trail buffer is allocated once at startup from the per-frame pool and reset each frame — zero allocations during steady-state editing.

### Performance

The trail adds at most `TRAIL_LENGTH` draw calls per frame (typically 5–10 active points). Each draw is a single caret-sized rectangle with alpha blending. The cost is negligible compared to the text pass. The trail points are stored in a fixed-size array — no heap allocation, no fragmentation, no GC pressure.

---

## The shared animation helper

All three systems — scroll, cursor, and trail — share the same easing primitive. The helper is a single inline function:

```c
// animation.h
static inline float ease_exponential(float current, float target, float speed, float dt)
{
    float k = 1.0f - expf(-speed * dt);
    return current + (target - current) * k;
}
```

Usage across subsystems:

```c
// Scrolling: heavy, slow
viewport->visual_y_px = ease_exponential(
    viewport->visual_y_px, viewport->scroll_y_px,
    VIEWPORT_SCROLL_SPEED, dt);

// Cursor: light, fast
visual_cursor_px = ease_exponential(
    visual_cursor_px, cursor_target_px,
    CURSOR_ANIM_SPEED, dt);

// Trail alpha: fade out
alpha = expf(-TRAIL_FADE_RATE * point.age);
```

The exponential function is chosen over alternatives (linear, ease-in-out, cubic) because:

1. **Constant relative rate**: the visual always covers a fixed fraction of the remaining distance per frame, regardless of how far it is from the target.
2. **No overshoot**: monotonic approach means the visual never flings past the target.
3. **Target switching**: if the target changes mid-flight, the function immediately starts converging to the new target. There is no "resetting" animation.
4. **Frame-rate independence**: derived from the continuous exponential decay equation, it works correctly at any refresh rate.
5. **Cheap**: two multiplications, one subtraction, one expf call. At 144 Hz, the cost is negligible.

### Why not linear easing?

Linear easing has a fatal flaw for editor animations: when the target changes, the visual must traverse the new distance at the same speed, which means it can never truly settle. Exponential easing always makes progress toward the current target, and the visual velocity is proportional to the remaining distance — it naturally decelerates as it approaches the target. This is the "weight" that makes scrolling feel physical.

### Why not cubic ease-in-out?

Cubic ease-in-out overshoots at the beginning and end, which can cause the visual cursor or viewport to briefly exceed the target position. For a text editor, overshoot is disorienting — the cursor should never appear to "roll past" its destination. Exponential relaxation is monotonic: the visual approaches from one direction only.

---

## Integration with the event loop

The animation systems are tied to the editor's main loop. When the editor is clean (no input, no edits, no rendering needed), it waits for events until the next animation deadline.

```
1. Process events (input promptly; never schedule full-file work ahead of it).
2. Apply transactions (edit → index → relex → layout).
3. Spend bounded time on syntax/search/checkpoints (0.5 ms per active tick).
4. Update animations: smoothstep scroll, cursor, trail aging.
5. Render if dirty or animating.
6. Present.
```

The loop checks whether anything is still animating (scroll not settled, cursor not settled, trail points still active). If all animations are settled and the document is clean, the loop sleeps until the next event or the next blink tick. This means:

- **Idle rendering while animating**: the editor still renders frames while the scroll or cursor is easing, so the animation is smooth.
- **Idle rendering while trailing**: trail points continue to fade and are drawn until fully expired.
- **True sleep**: when nothing is animating and no events are pending, the loop sleeps until the next blink deadline.

Focus loss cancels transient drag/key state, stops recording trail points, and reduces animation activity. The cursor and scroll continue to their targets but no new trail points are added.

---

## The decision log

These decisions were made for the animation system:

1. **Exponential relaxation for cursor/scroll** — the shared easing function. Revisit if the feel is wrong (5-line change).
2. **Scroll in pixels, not lines** — never revisit. Pixel accumulation makes smoothing identical at every DPI.
3. **Caret animation display-only** — the cursor position is always exact. The blink and visual position never block editing.
4. **Trail is display-only** — the trail buffer never affects document state, cursor position, or editing.
5. **Snap sub-pixel residual to zero** — allows the event loop to sleep when animations settle. Never renders a frame for sub-pixel drift.
6. **One shared `ease_exponential` helper** — not three separate easing implementations. One function, two parameters.
7. **Trail length bounded at 12** — enough for visual continuity, cheap enough to render and age every frame.
8. **Trail alpha is exponential decay** — matches the easing function. The fade is consistent with the visual language of the editor.

---

## Tuning guide

The animation parameters are tuned for a comfortable, responsive feel. They are all single constants and can be adjusted without any code changes beyond recompilation.

### Scrolling

| Parameter | Recommended | Effect |
|---|---|---|
| `scroll_speed` | 3.0–5.0 | Higher = faster convergence, snappier feel |
| Snap threshold | 0.5px | Below this, visual snaps to target |

### Cursor

| Parameter | Recommended | Effect |
|---|---|---|
| `cursor_speed` | 8.0–12.0 | Higher = cursor catches up faster |
| Blink period | 1.0–1.2s | Total cycle time |
| Blink visible fraction | 0.5 | Duty cycle (50% on, 50% off) |

### Trail

| Parameter | Recommended | Effect |
|---|---|---|
| `trail_length` | 12 | Max trail points |
| `trail_lifetime` | 0.5–0.8s | How long a point is visible |
| `trail_min_distance` | 4–8px | Minimum movement to record a point |
| `trail_sample_interval` | 0.015–0.02s | Time between trail samples |
| `trail_fade_rate` | 2.5–4.0 | Alpha decay speed |

---

## What's next

The animation systems described here are rendered through the single `cmd_draw` call detailed in Article 4: the caret, selection rectangles, trail overlays, and scrollbar are all quads drawn in the same pass, same shader, same draw call as the text itself.

Smooth cursor animations, cursor trails, and smooth scrolling are the editor's polish layer. They are not features — they are the difference between an editor that feels mechanical and one that feels alive. Every animation is derived from the same exponential relaxation, every parameter is tunable, and every system is display-only so that the logic layer stays instant and deterministic.