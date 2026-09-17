# Input API layering

Status: the minimal polling core and RGFW adapter are implemented and fed by the existing application event loop. Layer 2 remains a design proposal. This document supersedes the monolithic API proposal. The broader input-system-design.md describes possible advanced capabilities, not mandatory core features.

## Decision

Use the user's polling API as the core. Keep custom readable enums, independent of GLFW and RGFW values. Keep text exclusively in a separate optional event API. Editor commands, bindings and UI routing belong to the editor/application.

## Why the previous API was too complex

It combined physical state, text delivery, focus routing, device management, action bindings, replay and tick scheduling. These serve different consumers and have different lifetimes. Neither performance nor building an editor justifies requiring every game to use them.

Some parts were incorrect, not merely verbose: 64 bits cannot represent a full keyboard; a device ID is not a UI owner ID; a borrowed batch must not ambiguously own storage; a per-pump event view cannot promise retained fixed-tick history. Moving declarations to another header alone does not fix ownership or eliminate runtime costs.

Extra capabilities have valid purposes, but need not be in the core:

| Capability | Why polling cannot provide it | Location |
| --- | --- | --- |
| Committed Unicode | Physical keys do not determine characters | Optional event API |
| Ordered editing | Edge bits collapse repeated transitions | Optional event API |
| Event-time modifiers/repeat | Final state cannot reconstruct earlier chords | Optional event API |
| UI versus scene ownership | Both see the same raw press | Application routing |
| Remappable commands | Meaning is application-specific | Optional bindings |
| Replay/fixed-tick history | Requires persistent storage and consumption rules | Future subsystem |

Games can need text and bindings too. Separate by capability, not by application category.

## Layer 1: polling

Header: src/input.h. The implementation keeps the user's names and structure:

- InputKey and InputMouseButton: custom contiguous enums with count sentinels, including function keys and keypad keys. KEY_NONE represents unmapped keys and remains up.
- InputButton: down, pressed, released.
- Input: borrowed RGFW window, fixed key/button arrays, pointer position/delta and fractional scroll.
- input_init and input_update.
- key_down/pressed/released and mouse_down/pressed/released.
- mouse_x/y/dx/dy, scroll_x/y and input_axis.

Use enum counts instead of an arbitrary 512-key capacity. Start with three-boolean records, not an unmeasured bitset rewrite. Queries are indexed loads with debug assertions for enum bounds. No allocations, text buffer, event history, action registry or editor storage. No shutdown function is needed when the core owns no resources. The application owns its window.

Forward-declare the RGFW window. Translate keys using an explicit adapter table, including RGFW's different right/middle mouse ordering. Do not derive text from keys.

### Semantics

- Clear transients before draining each pump, including empty pumps.
- Down/up within a pump sets both edges and leaves down false. Multiple pairs intentionally collapse.
- Repeat keeps down true without setting a new pressed edge.
- Focus loss releases held buttons. Applications must distinguish this cancellation from an action triggered by a normal release.
- Preserve fractional scroll and window-local coordinates. Convert DPI only at render/hit-test boundaries.
- First observed pointer position establishes a baseline; reset that baseline on focus changes. Raw relative motion is distinct from positional delta.
- Pump while minimized. Do not replay one pump's pressed bits for every fixed simulation substep.

## One RGFW pump

The existing app already drains RGFW for Nuklear. A second pump would steal events.

Support two exclusive usage paths:

1. Standalone single-window game: input_update clears transients and drains RGFW.
2. Application-owned loop: an optional RGFW adapter exposes begin-pump and feed-one-event functions. The app drains once and feeds consumers; it does not also call input_update.

The convenience pump must be documented as single-window because RGFW has a shared event queue. Window close, resize, drop and renderer handling stay in the platform/application layer. An editor must handle close as a request to permit confirmation of unsaved changes.

## Layer 2: optional ordered events and text

Design src/input_events.h after polling tests pass. Use a separately owned recorder fed by the application pump, not hidden allocations inside every Input. Store events and UTF-8 bytes in reusable contiguous mu containers. Return borrowed read-only mu spans by value.

Text events reference exact byte ranges, not ambiguous C strings. Validate committed Unicode scalars. Preserve event order, event-time modifiers, repeats and cancelled releases. Views expire at reset/mutation; delayed consumers own copies. Grow or drain rather than silently truncate text. Native queue loss cannot be repaired by larger application buffers.

Clipboard paste and IME composition need explicit protocols; key-character delivery is not full IME support. Do not add replay, tick cursors or a UI hit-test callback to the initial event API. Borrowed views do not retain history; a hit test alone does not solve text focus, modal ownership or drag capture.

## Editor/application layer

The editor owns document commands, bindings, focus, selection/drag capture, modals, clipboard and Nuklear routing. Raw releases always update polling state even when consumed. A click must not activate both UI and the scene behind it. Introduce an editor-specific header when its concrete command and focus types exist, not a generic context stack in advance.

## Implementation sequence

1. Implement only polling state and explicit RGFW mapping. Compile a polling-only example without an event header.
2. Test same-pump edges, repeats, empty pumps, focus loss, unknown keys, mouse mapping, pointer baseline and fractional scroll.
3. Integrate with the existing app-owned pump without changing GPU lifetimes or UI ownership policy.
4. Add separately owned events/text; test Unicode, ordering, growth and borrowed-view lifetimes.
5. Add editor commands against concrete requirements. Defer generic bindings, replay, tick history and multi-device generations.

Performance target: zero core allocations, predictable contiguous reset work plus work proportional to events, and no per-entity dispatch. Measure before replacing simple arrays with bitsets or adding coalescing.

## Validation

The application builds with the polling core. tests/input_test.c passes under AddressSanitizer/UndefinedBehaviorSanitizer for the test and input implementation (the linked platform object is not instrumented). It creates an X11 window under Xvfb and sends native key events through RGFW_checkEvent. It checks press, empty-pump clearing, release, same-pump down/up, and injected repeat, focus, mapping, motion and fractional scroll cases.

A separate temporary probe verified delivery of the same motion/scroll events to polling state and Nuklear. A 12-second application smoke run reached rendering and ended at the timeout; this does not verify graceful shutdown or interactive UI behavior. The vendored RGFW header still emits undefined-inline warnings. Wayland and physical hardware input have not been tested.

The application now collects Input alongside its existing Nuklear processing. Existing raw-motion camera controls and UI routing have not been migrated. Layer 2 remains deferred. No mu changes are retained.

Reproduce the focused test from the repository root after building the platform object:

```sh
clang -std=gnu99 -g -fsanitize=address,undefined -Iexternal/vulkan/include \
    tests/input_test.c src/input.c build/src/platform.o \
    -lX11 -lXi -lXrandr -lXcursor -lXinerama -ldl -lm -lpthread \
    -o /tmp/mu-input-test
xvfb-run -a /tmp/mu-input-test
```
