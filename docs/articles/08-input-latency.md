# Article 8: Input System and Latency Architecture

## The input pipeline

Input is the editor's first subsystem. Every keystroke, mouse movement, and scroll event flows through the same pipeline:

```
GLFW callback → timestamped event queue → command dispatch → transaction → layout → render
```

GLFW key events trigger commands. Committed character events insert text. The editor does not synthesize printable input from physical key codes. This is a deliberate design choice: it means the editor handles all text input through the character callback, which provides correct UTF-8 codepoints regardless of the keyboard layout, IME state, or modifier combination.

## The event queue

Callbacks append events with timestamps into a per-frame event queue:

```c
typedef struct {
    uint64_t timestamp;  // mu_time_now when the event occurred
    InputType  type;     // KEY_PRESS, KEY_RELEASE, TEXT_INPUT, MOUSE_MOVE, ...
    union {
        struct { int key; int scancode; int action; int mods; } key;
        struct { uint32_t codepoint; } text;
        struct { double xpos, ypos; } mouse;
        struct { double xoffset, yoffset; } scroll;
    } data;
} InputEvent;
```

The queue has fixed capacity. A dropped-count assert turns back-pressure into a loud bug instead of a silent design state. Coalesce replaceable motion/resize events — never coalesce text or key transitions. Text is never coalesced: every character must be inserted.

**Input processing is replayed over (document, cursor, viewport) at frame start.** The pipeline runs after all events are consumed. This means the editor always processes the complete set of events for the frame before rendering.

## Command dispatch

Commands are the layer between raw input and document mutation:

```c
typedef enum {
    CMD_MOVE_LEFT, CMD_MOVE_RIGHT, CMD_MOVE_UP, CMD_MOVE_DOWN,
    CMD_WORD_LEFT, CMD_WORD_RIGHT, CMD_HOME, CMD_END,
    CMD_PAGE_UP, CMD_PAGE_DOWN,
    CMD_INSERT_CHAR, CMD_DELETE_BACKWARD, CMD_DELETE_FORWARD,
    CMD_DELETE_WORD_BACKWARD, CMD_DELETE_WORD_FORWARD,
    CMD_NEWLINE, CMD_TAB, CMD_BACKSPACE,
    CMD_SELECT_ALL, CMD_COPY, CMD_CUT, CMD_PASTE,
    CMD_UNDO, CMD_REDO,
    CMD_SAVE, CMD_OPEN,
    CMD_FIND, CMD_REPLACE,
    CMD_SCROLL_UP, CMD_SCROLL_DOWN,
    // ...
} EditorCommand;
```

Each command is handled by a function that operates on `EditorState`. The command handler either creates a `ReplacementTransaction` (for mutations) or updates `Viewport`/`TextCursor` (for navigation).

Clipboard paste is one owned payload and one history group. The paste buffer is received from GLFW's clipboard API, decoded as UTF-8, and inserted as a single replacement transaction.

## Latency: event timestamp to photon

The input-to-photon budget is measured, not hoped. Every event carries a timestamp. The editor logs:

```
frame_start_timestamp − last_event_timestamp
```

This value is P50/P99. The number to beat is "one vsync interval." At 60 Hz, that is 16.7 ms. At 144 Hz, 6.9 ms.

The latency chain:

1. **Event occurrence**: GLFW callback timestamp.
2. **Event queue**: pushed to the per-frame queue (nanoseconds).
3. **Frame start**: all events replayed, transactions applied (microseconds to milliseconds depending on edit size).
4. **Layout**: visible lines laid out (microseconds on cache hit, milliseconds on cold miss).
5. **GPU submission**: `begin_pass` + `cmd_draw` (microseconds).
6. **Presentation**: swapchain present, vsync.

Steps 3–4 are the variable part. A single-character insert should be < 1 ms for P99. A large paste may take several milliseconds — acceptable, because the user expects a delay for a large operation.

## Scrolling and animation

Scrolling accumulates in pixel units, not lines. The target is set immediately; the visual value smooths toward it. This means:

- **Scrollbar jumps are instant**: the target moves instantly, the visual follows.
- **Wheel scrolling is smooth**: exponential relaxation gives weight.
- **Cursor-follow is automatic**: when the cursor leaves the viewport, the target adjusts.

The same exponential relaxation helper serves both cursor and scroll. One shared function, two parameters.

Caret animation is display-only. The cursor position is exact the instant a key is pressed. The renderer maintains `visual_cursor_px` (float) and relaxes it toward the pixel position of `cursor`. Same helper, different parameters.

## The event loop

When clean, the editor waits for events until the next blink/animation deadline. A tick:

1. Process events (input promptly; never schedule full-file work ahead of it).
2. Apply transactions (edit → index → relex → layout).
3. Spend bounded time on syntax/search/checkpoints (0.5 ms per active tick).
4. Render if dirty or animating.
5. Present.

Worker completion wakes the loop. Hidden/minimized windows do not submit frames, but I/O and command completion still progress. Pending incremental work schedules another bounded tick without requiring constant GPU rendering.

Focus loss cancels transient drag/key state and reduces animation activity.

## What's next

Article 9 covers the memory architecture: how the editor achieves zero-allocation steady state, what the memory budget looks like, and how every subsystem contributes to or subtracts from the footprint.

Input latency is the editor's reputation. A fast editor is one where every keystroke produces pixels within one frame. This is achieved not by fast code, but by bounded work — never doing more per frame than the frame budget allows.
