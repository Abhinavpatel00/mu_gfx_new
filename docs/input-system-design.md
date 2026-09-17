# Data-oriented input system

Status: proposed subsystem. The RGFW/Nuklear migration supplies a platform/UI adapter, not the complete action, replay, and fixed-tick system described here.

## 1. Boundaries

RGFW owns native windows and acquisition. A main-thread `InputSystem` owns normalized events, device state, bindings, and routing. Applications consume action IDs and committed text, not RGFW globals. Nuklear consumes input for tools, menus, and small fields; it does not own editor document editing.

Use one ingestion path: translate each event once, rather than duplicating callback and polling delivery. The vendored declarations use `RGFW_checkEvent(&event)`, `RGFW_keyChar`, and `RGFW_window_createSurface_Vulkan`. Older RGFW examples have different signatures.

Native pointers stay in a cold window table. Events carry generation-bearing `WindowId` and `DeviceId`; ingestion rejects delayed events for reused slots. Start with keyboard/mouse and one window. Gamepads and multiple windows require explicit adapters and tests.

## 2. Storage

| Data | Layout and lifetime |
|---|---|
| Ordered events | Dense growable AoS array: timestamp, sequence, IDs, tag, compact union payload |
| Text/drop data | Per-batch byte arena; events store offsets/lengths, never native pointers |
| Physical state | Down/pressed/released bitsets; contiguous pointer position, raw delta, wheel records |
| Bindings | Packed records grouped by context/trigger; compile lookup ranges on configuration changes |
| Actions | Dense table indexed by `ActionId`, plus ordered command events |
| Routing | Small context stack and capture/focus owner IDs, no per-entity listener allocation |
| Replay | Owned serialized events and bytes, independent of temporary arenas |

AoS suits sequential whole-event consumption; bitsets suit state queries. Reserve common capacities, grow at preparation boundaries, and record peaks. Reuse mu containers; add reusable missing primitives to `external/mu`. Pass mu spans/byte spans by value, larger system objects by pointer. Borrowed batches expire at reset; delayed consumers copy payloads.

## 3. Semantics

Physical keys, committed Unicode text, and editing commands are separate. Translate key codes through a table, not character arithmetic. `RGFW_keyChar` carries committed scalars; validate and encode UTF-8. Never manufacture printable text from physical presses. IME preedit is a future protocol.

Clear transient state once per pump. Down/up within one pump sets both edge bits and leaves down false. Repeat events do not create physical press edges. Preserve repeat markers for navigation; screenshot/record commands accept non-repeat presses only. Event-time modifiers matter: the final modifier snapshot cannot interpret earlier chords reliably.

Use engine monotonic receipt timestamps plus sequence numbers. These are not original hardware timestamps. Positions are window-local; convert to framebuffer coordinates only at the render/hit-test boundary. Keep raw motion separate and preserve fractional scroll. Do not double-apply DPI scaling.

Focus loss/disconnection emits cancellation/release for owned down inputs, clears drag/relative state, and releases capture. Focus return does not synthesize text. Close requests are application commands so unsaved changes can be confirmed before window destruction.

## 4. Routing and Nuklear

For each event: update raw state, resolve owner, feed owner, then emit permitted actions. Consumption never blocks raw-state release updates. Cancel routed actions when their context loses ownership.

Priority is explicit: modal dialog, focused UI/text field, editor viewport, application/gameplay. Only documented global shortcuts bypass focus. Pointer capture established on press retains motion/release until completion. Keyboard text has one owner. Retain route/consumed metadata rather than deleting events.

Mirror Nuklear input between `nk_input_begin`/`nk_input_end`, then build widgets. Nuklear has no universal ImGui-style capture contract. Maintain application focus/capture and prior committed hit regions; defer ambiguous click-through actions until the current UI build resolves ownership. A click must not activate both a button and the scene behind it.

Nuklear summarizes input and has finite text capacity. Clipboard paste uses an owned payload and widget paste API, not thousands of synthetic events. Main document editing consumes the ordered stream directly, preserving fast transitions that immediate-mode summaries can collapse.

## 5. Scheduling, overload, and replay

Drain events even when minimized. Render scheduling is independent. Use `RGFW_waitForEvent` with a deadline while idle; worker completion requires an audited wake mechanism. Until one exists, use bounded waits while jobs are pending, not indefinite sleep.

Audit RGFW's native queue limits as well as the engine queue. Coalesce only adjacent replaceable motion/resize events with the same owner and no intervening transition. Never silently drop text or transitions. Grow or drain ordered chunks; detected native loss triggers diagnostics and down-state reconciliation, without claiming lost text can be recovered.

Fixed simulation ticks consume each event once by timestamp/sequence cutoff. Several ticks in one render frame must not each replay its press edges. Retain unread events if no tick runs. UI uses application cadence; simulation receives separately routed commands. Replay records normalized events, owned bytes, configuration version, and tick assignments, not native struct bytes/pointers.

## 6. Implementation and acceptance

1. Pure C reducer with injected batches; test without a window.
2. RGFW translation and ownership routing; test X11 and Wayland separately.
3. Compiled bindings and actions, then fixed-tick consumption/replay.
4. Measure warmed typing, dense motion, large paste, focus loss mid-drag, modal activation, DPI changes, and stalled rendering.

Tests cover down/up in one batch, repeats, modifier order, press/release ownership, multiple/empty simulation ticks, queue growth, stale IDs, malformed text, and click-through. Record allocations, queue depth, copied bytes, dispatch duration, and event-to-command latency. Work scales with incoming events and matching bindings, not every registered action/entity.
