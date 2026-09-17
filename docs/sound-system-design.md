# Data-oriented sound system

Status: design proposal; SoLoud is vendored but this migration does not enable audio playback. Use its existing C API behind an engine-owned data model. SoLoud implementation code is C++; engine integration remains C and does not introduce C++ standard-library facilities.

## 1. First scope and ownership

Start with preloaded short effects, one-shot/loop playback, pause/stop, gain/pan, master/category buses, and one listener. Add spatial emitters after lifetime and voice-budget tests. Defer streaming, DSP graphs, occlusion, and sample-accurate gameplay scheduling.

`SoundSystem` is explicitly passed to sound functions. The main/control thread owns all engine sound tables and makes SoLoud control calls. SoLoud owns its mixing/device thread. Do not add another engine audio thread merely to wrap every call in a queue. Backend calls can lock and allocate; batching reduces overhead but does not turn SoLoud into a lock-free hard-real-time engine.

Workers read asset bytes into owned buffers; installation/decode initially happens at loading boundaries on the control thread. A later decoder worker may build unpublished sources only after its thread-safety/lifetime contract is audited. No filesystem I/O, logging, gameplay callbacks, or engine allocation occurs in an engine-supplied mixer callback.

## 2. Data layout

| Table | Fields and access |
|---|---|
| Sound assets | Slot generation, readiness, source kind/pointer, active/pending reference counts; cold path/hash/diagnostics separately |
| Voices | Dense active records with engine `VoiceId`, backend handle, `SoundId`, bus, priority, flags, age |
| Spatial emitters | Dense SoA position/velocity/gain and voice IDs, because these columns update in batches |
| Buses | Small stable table of category IDs, gain/mute, backend bus/source handles |
| Commands | Ordered compact tagged AoS records with IDs and scalar parameters; owned byte storage only for loading requests |
| Completion | Dense result events with request/voice IDs and completion reason |

Use generation-bearing handles and a sparse-to-dense mapping for swap removal. Native SoLoud pointers/handles stay inside the backend boundary. An asset may play through many voices; assets and voices cannot share an identity. Keep names/paths out of per-frame spatial loops. Reuse mu ID pools, arrays, spans and allocators; any missing reusable generation/sparse primitive belongs in `external/mu`.

Choose AoS for full voice lifetime records and SoA only for independently processed spatial fields. Small nonspatial projects need not allocate spatial arrays. Reserve capacities during preparation instead of enormous globals or stack command arrays.

## 3. Preparation and execution

Proposed API operations are initialize/shutdown, load/unload asset, play, stop/pause voice, set voice parameters, set listener, and submit prepared updates. Parameters containing mu spans or byte spans are passed by value; larger descriptors by pointer. External file/device failures return status plus diagnostic at the boundary. Document and assert programmer preconditions without duplicating backend validation.

A play request resolves a ready `SoundId`, reserves an engine voice slot, and establishes an asset reference before submission. Return an invalid ID or explicit rejected status for the documented voice-capacity policy; do not pretend admission guarantees audible hardware output. Pending voices have explicit state until their command executes. Stop-before-play is ordered cancellation, not a stale backend call.

Prepare commands on the main thread from gameplay/UI requests. Preserve play/stop/order dependencies. Coalesce final gain/position updates only within intervals that do not cross a lifetime command. If producer workers are added, they submit owned batches at a synchronization boundary; they never mutate sound tables.

Execution resolves IDs once, calls the SoLoud C functions, and stores backend handles. `Soloud_playEx` supports bus routing; `Soloud_set3dSourceParametersEx` and listener updates are followed by one `Soloud_update3dAudio` per spatial batch. Skip unchanged parameters. Backend execution is still a sequence of library calls, not a promised SIMD bulk API.

Poll active handles through `Soloud_isValidVoiceHandle` at control ticks to retire ended/stolen voices. This check is required asynchronous reconciliation, not redundant programmer validation. Do not inspect a backend handle twice after it has been reconciled dead. Swap-remove dense records and update the sparse mapping. Emit completion into a result array; never invoke gameplay from the mixer thread.

## 4. Lifetime and asset bytes

`Wav_loadMemEx` has explicit copy/take-ownership flags. Do not rely on its defaults or hand a mu allocation to a foreign delete operation. Initially use copy=true, takeOwnership=false for encoded bytes and release the input after load completes; decoded PCM belongs to the `Wav` source. Check load errors immediately. Account for overlapping encoded and decoded buffers.

Unloading marks the asset unavailable to new plays. Cancel pending plays or retain their references, stop active voices, reconcile stop completion, then destroy the source on the control thread. Keep sources/buses alive through every referring voice and queued command. SoLoud source destruction can stop instances, but engine lifetime policy must not rely on that hidden behavior. Hot reload publishes a new generation; old voices finish or stop against the old source.

Future `WavStream` sources may retain file/memory access during playback and decode on the audio path. Streaming is not just replacing a class name: define backing-buffer lifetime, decoder cost, buffering, underflow policy, and disk-I/O isolation first.

## 5. Budgets and timing

Separate engine voice capacity, SoLoud logical voices, and backend active mixing voices. Configure the latter through `Soloud_setMaxActiveVoiceCount`; a valid voice can be virtual or inaudible. Prefer priority/category quotas and deterministic oldest/quietest selection for admission, with reserved critical/UI capacity. Never scan every asset to find an active voice. Critical stops must not be dropped when a command batch is full; drain/grow on the control thread or reserve a control lane. Record rejected plays.

Decoded float PCM is approximately frames x channels x four bytes, plus source and voice state. Report decoded assets, encoded/loading overlap, streaming buffers, command reserves, buses and filters separately. Default capacities are settings tuned against workloads, not promises of unlimited polyphony.

Keep master/music/effects/UI gains separate; use backend fades for smooth changes rather than per-render-frame gain stepping. Audio continues when rendering pauses. Muting on focus loss is an application policy, not a device teardown. Do not multiply positional velocity by frame delta twice; use consistent world units and seconds.

Choose backend/sample rate/buffer size at initialization and report actual negotiated values. Device-open failure may disable optional audio with an explicit status. Device loss needs a defined disabled/reinitializing state and replay policy; do not promise seamless hot-plug without backend tests. Command-to-device latency includes control tick delay, backend buffering, and hardware latency. Vulkan submission timestamps are not an audio clock.

For later offline capture, use a separately initialized no-output mixer and drive `Soloud_mix` with exact sample-frame counts derived from capture time, carrying fractional remainders. Do not call manual mixing concurrently with an active device backend on the same instance. Deterministic command timing does not guarantee bit-identical floating-point output across platforms.

## 6. Initialization, shutdown, and validation

Build only selected SoLoud sources/backends plus its C glue when implementation is scheduled; avoid indiscriminately compiling every demo/decoder/backend. Pin source/license versions. Initialization checks backend and bus creation immediately before exposing a ready system. Keep foreign C++ code isolated from engine ownership.

Shutdown stops accepting commands, cancels/joins loaders, stops voices, reconciles pending references, and destroys sources/buses while the mixer object still exists and backend synchronization is available. Then `Soloud_deinit` stops the backend and `Soloud_destroy` releases the instance. Audit the exact source destructor/backend contract during implementation; do not race a worker installing an asset with teardown.

Milestones:

1. Pure control model with fake backend: generations, admission, ordered play/stop, swap removal, asset references, cancellation.
2. Optional SoLoud device plus loaded effects, category buses, completion reconciliation and error tests.
3. Batch spatial updates, voice budgets/fades, measurements; streaming only after separate latency tests.

Test unload during playback, stop-before-submit, natural completion, voice stealing, slot reuse, rejected loads/plays, hot reload, bus destruction, idle/minimized playback, shutdown during load, and device failure. An offline no-output backend can verify silence/gain/pan/fade/sample counts without speakers. ASan/UBSan cover lifetime; concurrency tests cover worker/control handoff. Measure command counts, active/virtual voices, copied bytes, source memory, control-call lock time, backend underflows where observable, and end-to-end audio latency. No hard-real-time or allocation-free claim is accepted without measurements inside the chosen backend.
