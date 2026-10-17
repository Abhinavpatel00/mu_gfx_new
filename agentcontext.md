# agentcontext.md — quick context for AI agents

> **Read this first.** It is the persistent context so a fresh agent does not
> have to re-scan the whole repository. Keep it up to date when the architecture
> or the important commands change. For C/DOD style rules, `AGENTS.md` is the
> contract — this file is *where things are and how to run them*, not a style guide.

---

## 1. What this project is

A small Vulkan game/engine in C99 (`main.c`) with a C renderer layer
(`src/renderer.c`) and a data-oriented scene/ECS-lite subsystem
(`src/three_d/scene.c`). Graphics are authored in **Slang** (`shaders/*.slang`),
compiled to SPIR-V and embedded into the binary at build time. The look is a
town of GPU-simulated slimes in a streamed, chunked open world, with a PBR path
and a stylized toon path.

Sibling docs:
- `AGENTS.md` — the DOD doctrine and C style. **Read it for any coding task.**
- `renderer.txt` — the authoritative per-frame render-flow reference (pass order,
  buffers). Update it when you add a pass.
- `dataflow.txt` — the companion data reference (who owns each table, who reads
  it, lifecycles, push-struct consumers, material-flag routing invariants).
  Update it when you add a table, a buffer slice, or a push field.
- `data/dodnotes.txt`, `.agents/skills/*` — deeper DOD material.

---

## 2. Build & run

```sh
bash build.sh          # compileslang.sh release -> embed_shaders.py -> make -j4
./build/app x11        # or: ./build/app wayland
```

- `build.sh` runs **three** steps: (1) `compileslang.sh release` compiles
  `shaders/*.slang` -> `compiledshaders/*.spv`; (2) `scripts/embed_shaders.py`
  regenerates `build/generated/shaders.h` from `compiledshaders/*.spv`;
  (3) `make -j4` links `build/app`.
  - **But the linked binary loads `.spv` from disk at runtime** — embedding
    only matters for a `make release` build. See §7.8 for the implications.
- `compileslang.sh` is a **per-stage entry-point finder**: a file is compiled as
  vertex/fragment/compute iff it *defines* `vs_main` / `fs_main` / `cs_main`
  (an anchored grep, not a substring match). A new shader stage = define the
  entry point. Push-constant-only compute (no entry match) is skipped by design.
- **Shader compile cache gotcha:** `compile_stage` only rebuilds when
  `src -nt out` (source mtime newer than the .spv). Editing a shader updates its
  mtime, so it normally rebuilds — but if a build ever seems to ignore a shader
  edit, `rm -f compiledshaders/<name>.*.spv` and rebuild. Always confirm the
  `fragment → ...` line appears in the build output.
- `slangc` lives at `/opt/shader-slang-bin/bin/slangc` (not on `PATH`).
- Debug build flags: `-w -O0 -g` (warnings suppressed, unoptimized). The app is
  built `DEBUG` for day-to-day work.

### Headless verification (no window interaction)

The app opens a real window; drive it headless with env vars and read
`perf_report.txt` / logs (never read screenshots back into context — token waste;
measure with a script and print only numbers).

| Env | Effect |
|-----|--------|
| `MU_PERF=<N>` | write `perf_report.txt` after N frames (EMA settled) and exit |
| `MU_SHOT=<N>` | write `captures/screenshots/mu_shot.png` after N frames, keep running |
| `MU_RES=WxH` | request a window size (the WM may override it) |
| `MU_SLIMES=<N>` | crowd size (note: `0` is ignored — see §7 pitfalls) |
| `MU_TOON=1` | initial toon flag (the UI/`post.toon` wins per frame) |
| `MU_TOON_STYLE=0\|1\|2` | headless toon-style override (cel/soft/painterly) |
| `MU_TOON_OUTLINE=1\|0` | headless toon-outline override (UI/`post.toon_outline` wins per frame) |
| `MU_FLYCAM_TEST` | TEMP scripted eastward world-transit sweep (chunk streaming test) |
| `MU_SHADER_DEBUG` | draw push `counts[5]` -> GPU Debug Printf |
| `MU_SLIME_DEBUG` | `sp.counts[6]` -> gated printf in the slime shaders |
| `MU_FREEZE_TIME` | **added for verification**: zero `global_data.time` and `delta_time` so screenshots are deterministic (see §7) |

`DISPLAY=:0` (X11) and `WAYLAND_DISPLAY=wayland-1` are available. `Xvfb` /
`xvfb-run` exist but the Vulkan surface path under Xvfb was not reliable; prefer
the real display + `MU_FREEZE_TIME`.

---

## 3. Repo map (the files that matter)

```
main.c                     game loop, camera, world streaming, slime spawn, GameHooks
src/renderer.c             Vulkan renderer: passes, pipelines, PostSettings, UI bridge
src/renderer.h             Renderer API (renderer_toon, renderer_set_camera, ...)
src/slangtypes.h           C structs shared with Slang (GlobalData, ...). KEEP IN SYNC
src/constant.h             VALIDATION / GPU_VALIDATION / GPU_PRINTF, capture dirs
src/nuklear_profiler.inl   the profiler/Color-Grade UI (toon switch + toon style combo)
src/three_d/scene.c        scene_frame(): upload, slime compute, cull, draws, obstacles
src/three_d/scene.h        scene_* API (scene_static_clear, scene_slime_obstacles, ...)
src/three_d/scene_shared.h C+Slang shared layout structs (push constants!) + static asserts
shaders/*.slang            Slang shaders (one file may define several entry points)
compiledshaders/*.spv      build output (do not edit)
build/generated/shaders.h  generated embedded SPIR-V (do not edit)
renderer.txt               per-frame pass reference (update when passes change)
dataflow.txt               per-table data reference (update when tables/slices/pushes change)
vk_toon_shader/            vendored NVIDIA Vulkan-Samples "toon shading" reference (see §8)
post_settings.json         live post/color-grade + toon settings (saved by the app)
perf_report.txt            last MU_PERF dump
todo.txt, usage.txt, README.md, slimes.txt, slime-rancher-article/  design notes
```

`external/` holds vendored deps (`mu` = mylib, glfw, nuklear, vulkan, stb,
meshoptimizer, tree-sitter). Prefer `mylib` (`mu.h`) primitives; add small
reusable primitives there rather than inventing local ones (see `AGENTS.md` §13).

---

## 4. Render flow (high level; details in `renderer.txt`)

`main.c` GameHooks -> `Renderer` -> `scene_frame()` each frame is one command
buffer: a chain of compute passes then graphics passes. Every pass declares the
exact buffer slices it reads/writes (`begin_pass`/`end_pass` + `BufferAccess`),
so barriers only touch those ranges.

Compute: upload (when dirty) -> slime sim/grid/resolve -> cull -> compaction.
Graphics: depth prepass (if used) -> scene (PBR **or** toon) -> sky/grass/fire ->
sparkle -> SSAO -> SMAA -> bloom -> post (exposure/tonemap/grade) -> UI -> present.

`GlobalData` (in `src/slangtypes.h`, mirrored in the Slang copy) is the per-frame
constant block every shading path reads: view/proj matrices, `camera_pos/dir`,
`time`, `delta_time`, `frame_count`, `toon_style`, `screen_params`, environment
(`env_sky/horizon/ground`, `sun_dir`, `sun_color`), mouse. The renderer fills it
in `update_global_data()`.

---

## 5. Scene / slime subsystem (current state)

- GPU-simulated slime crowd; **no per-frame CPU slime work**. Shaders:
  `slime_sim.slang` (integrate hops, uses `global_data.delta_time`; `dt<=0`
  freezes), `slime_grid.slang` (camera-relative hop grid, counting sort),
  `slime_resolve.slang` (collision push, writes instances/squash/stretch/face),
  `sparkle.slang` (additive transparent pass).
- **Chunk-streamed open world** (done, verified): `CHUNK_SIZE 32`, `CHUNK_GRID 8`,
  `CHUNK_R 4`, `CHUNK_SIDE 9` -> a `9x9` window (81 chunks, 288 m) around the
  player, regenerated on chunk cross (not persisted). In `main.c`:
  `chunk_scatter[]`, `chunk_hash`/`crand`/`crange`, `chunk_build`,
  `world_instance_total`, `world_stream`, `g_loads*`, `g_chunk_cx/cz/done`. Init
  order: `slime_build` (enables sim) then `world_stream` (rebuilds obstacles).
  `scene_static_clear(Scene*)` retires the streamed window; obstacles are rebuilt
  in `scene_frame` when `slime_obs_dirty`.
- Spawn: `slime_build` (scale 0.9..1.4, 2048 by default, annulus [10,76]).
  Uses `frand`/`frange`/`g_seed`.
- Slime overlap is a **known open issue** (single-pass Jacobi push + a vertical
  hop gate can leave airborne slimes overlapping grounded ones).

---

## 6. Two shading paths: PBR and toon

- `shaders/scene.slang` = **PBR** (metallic/roughness, split-sum ambient via
  `env_brdf_approx`, `env_diffuse_irradiance`, `env_reflection_color`,
  `env_specular_occlusion`, multi-scatter `f_ms`, plus a gel rim for slimes).
- `shaders/toon.slang` = **toon** (standalone; the vertex stage is a verbatim
  copy of scene.slang's — keep the two in sync when you edit either).
  `global_data.toon_style` selects one of three shared-sun/ambient looks:
  `0` cel (hard 2-band, cool shadow), `1` soft stylized (4 soft bands),
  `2` painterly (wide warm bands). Gel flag adds an albedo fresnel (capped
  against the style rim, never stacked). **The silhouette ink is the post
  edge pass now** (see §8); no style keeps an in-shader fresnel outline.
- Both share `shaders/lighting.slang` (`env_*` helpers) and feed **linear HDR** —
  exposure/bloom/tonemap happen in post. `i.bloom` alpha carries per-instance
  bloom strength.
- The renderer picks the pipeline at runtime: `renderer_toon(r)` returns
  `post.toon`; `main.c` mirrors it with `scene_set_toon()`.
- `PostSettings` (in `src/renderer.c`) holds `toon`, `toon_style` and
  `toon_outline`; JSON save/load in `post_settings.json`; UI in
  `src/nuklear_profiler.inl` ("Toon shade" + "Toon outline" checkboxes +
  "Toon style" combo); headless overrides `MU_TOON_STYLE` and
  `MU_TOON_OUTLINE`.

### Toon brightness (closed — full integration parity with PBR)
- Root causes, in order of impact — all fixed in `shaders/toon.slang`:
  1. The toon ambient used `kd = (1 - f0)` while the PBR path dims its ambient
     diffuse by `(1 - fss_ess)` from the split-sum BRDF
     (`fss_ess = f0*ab.x + ab.y` via `env_brdf_approx`; ~0.15–0.35 for
     dielectrics). Toon ambient ran up to ~30% brighter over the whole frame.
     Toon now performs the same `env_brdf_approx` call and formula.
  2. Toon `kd` ignored Fresnel; it now uses the same
     `fresnel_schlick(f0, vdh)`-aware `kd = (1 - fres) * (1 - metallic)` as PBR.
  3. Lambert `1/pi` was missing from the direct diffuse (fixed earlier; combine
     is now `diffuse * LIGHT_INV_PI * sun + ambient + specular * sun + rim`).
  4. Per-style specular was not gated by the light band; PBR multiplies direct
     spec by `ndl`. Toon now multiplies each style's spec by `ramp`, and the
     direct spec no longer carries `* ao` (PBR reserves ao for ambient spec via
     `spec_ao`; the old `spec * sun * ao` double-occluded highlights).
  5. Style palettes: soft shadow tint `1.0,0.97,0.93` -> `0.80,0.82,0.90` (a
     near-white shadow band means unlit surfaces run at full Lambert);
     painterly shadow `0.86,0.76,0.60` -> `0.64,0.53,0.40`; painterly rim mixed
     `env_sun_radiance()` (HDR, ~1.2) which alone pushed silhouettes past the
     bloom threshold — now `lerp(sky, sun_radiance, 0.25) * 0.35`, and the soft
     rim scaled 0.5 -> 0.35.
  6. Rim double-stack on gel materials: the gel wet rim (`albedo*(1-ndv)^3
     * 0.8`, the PBR-matched single fresnel add) had the per-style rim added
     on top at the same grazing band — a second light halo on every slime
     silhouette, and the gel crowd fills the frame. Fixed to
     `rim = max(rim, gel_rim)` on gel materials (PBR keeps one fresnel add).
  7. No subtractive term in styles 1/2: cel matches PBR partly because its ink
     (`col *= 1 - outline`, 0.45 at silhouettes) pays for its own additive
     extras. Soft/painterly were PBR budget + additive rims with nothing
     subtractive, so they could only sit above parity. Both now carry a light
     ink: style 1 `pow(1-ndv,5) * 0.25`, style 2 `* 0.18`.
- The earlier build showed a missing `fragment -> .../toon.frag.spv` line while
  still exiting 0: the `src -nt out` check did not fire for a same-second edit.
  When a shader edit must land, `rm -f compiledshaders/<name>.*.spv` before
  rebuilding and confirm the compile line + new `build/generated/shaders.h`
  mtime (see the cache gotcha in §2).
- Audit method (per user preference): term-by-term logic comparison of
  `toon.slang` against `scene.slang` rather than screenshot statistics.
- **PITFALL for any comparison runs:** `post.toon` in `post_settings.json` wins
  over the `MU_TOON` env var every frame (`pets_render` mirrors
  `renderer_toon(r)` -> `scene_set_toon`); `MU_TOON=1` only sets the initial
  value when no JSON exists. `MU_TOON_STYLE` (env) does override `toon_style`.
  A "PBR baseline" run with the JSON still saying `"toon": true` silently
  renders the saved toon style instead. Also the app saves settings on
  `renderer_destroy`, which a SIGTERM (timeout kill) skips.
- Deterministic capture recipe if numbers are ever wanted again:
  `MU_FREEZE_TIME=1 MU_SHOT=N` + a PIL script (mean/p90/frac>0.9). Reference
  figures at that recipe: PBR mean 0.496, p90 0.917, frac>0.9 0.124; toon cel
  0.500. Toon soft/painterly were re-tuned by audit after those stats were
  abandoned; re-measure if a style ever looks off again.

---

## 7. Pitfalls & gotchas (learned the hard way)

1. **Push-constant layout:** Slang 16-byte-aligns `float4`/`SceneVec4`. A
   `SceneVec4` after an odd number of 8-byte pointers shifts every trailing field
   by 8 and desyncs C/Slang. Keep the push tail scalar-only, or put the vec4
   first. Push structs must be `<= 256` bytes. `scene_shared.h` has the shared
   layout + `_Static_assert`s; `src/slangtypes.h` mirrors `GlobalData` for both
   languages — **change both sides together**.
2. **`MU_SLIMES=0` does nothing** — `main.c` only accepts `v > 0`, so `0` leaves
   the default crowd. Use a positive value (or freeze time) to reduce motion.
3. **Non-deterministic screenshots:** `global_data.time` (real seconds) drives
   fire/grass/sparkle/gel animation; `delta_time` drives the slime sim; the WM
   may also resize the window. Use `MU_FREEZE_TIME=1`; do not trust cross-run
   luminance comparisons without it.
4. **Stale shader build** — see §2 cache gotcha.
5. **`vk_toon_shader/` is a nested git repo** (its own `.git`): the parent repo
   sees `D vk_toon_shader` in the index + untracked `vk_toon_shader/`. Treat it as
   a vendored reference; don't accidentally commit its internals.
6. Hardware here is an Intel UHD (TGL GT2); perf numbers are very noisy
   (identical runs 10-30 ms). Prefer structure over single-shot timings.
7. Do **not** read screenshots back into context. Measure and print numbers.
8. **SPIR-V is loaded from disk at runtime.** `make`'s default target is the
   DEBUG config with `-DEMBED_SHADERS` commented out (Makefile ~line 128), so
   every `./build/app` run reads whatever `compiledshaders/*.spv` currently
   holds. Consequences:
   - a) Rebuilding `.spv` while an app runs also triggers the dmon hot-reload
     watcher (`src/renderer.c` watch_callback) → pipelines get rebuilt mid-run
     from the new files and `printf("Pipeline %d hot reloaded")` appears.
   - b) You cannot A/B two binaries against different shader states by copying
     binaries: each run's SPVs = current disk state. For a deterministic
     shader A/B, snapshot SPV sets (`cp compiledshaders/*.spv /tmp/...`) and
     `cp` the wanted snapshot into `compiledshaders/` *before* launching.
   - c) `compileslang.sh` rebuilds `.spv` only when `src -nt out`; editing an
     included header (e.g. `scene_shared.h`) does not bump the .slang mtimes of
     its includers, so half the kernels keep stale layouts. When changing ANY
     shared shader header: `rm -f compiledshaders/*.spv` then `bash build.sh`.
9. **run cwd = app dir? no: repo root.** `perf_report.txt` is written relative
   to the process cwd; running the app from another directory looks for assets
   there and misbehaves. Always run from the repo root:
   `DISPLAY=:0 MU_RES=678x747 MU_PERF=N ./build/app x11`.
10. **`MU_RES` is NOT reliable without setting it explicitly every run** — the
    WM sometimes resizes to 1362x749 and scales all numbers ~4x. Pin
    `MU_RES=678x747` on every measurement run and grep `resolution:` from the
    report to confirm.
11. **System-load noise dominates on this laptop** (`opencode`, `cline`,
    `freebuff` agent processes eat CPU and add multi-ms jitter to GPU times;
    run-to-run spread 2x for the same build). Mitigations: `renice -n 15` the
    agent PIDs before measuring; take 3+ runs; compare matched rows (cur/avg
    of `Scene Draw`) rather than whole-frame numbers; prefer instruction counts
    (see §10) as a noise-free signal.

---

## 10. GPU profiling + shader instruction analysis tooling (new)

### Per-pass GPU profiler
- `GPU_SCOPE(prof, cmd, "Name", STAGE_*)` in `src/helpers.h` brackets one pass
  with timestamp queries; the profiler lives at `vk->gpuprofiler[frame]`;
  `perf_dump()` in `src/renderer.c` prints/pass times into `perf_report.txt`.
- **Do not nest GPU_SCOPEs** — the report sums every scope into the frame
  total, so a nested scope double-counts. One scope per pass ("Sky", "Cull",
  "Scene Draw", ...), post passes keep their own ("SSAO", "Bloom", ...).
- Baseline per-pass budget at 678x747 (iGPU, mid-noise): Scene Draw ~9.5 ms of
  ~13.7 ms frame; then Tonemap ~1.3, SSAO ~0.8, SMAA ~0.9 total, Bloom ~0.4,
  rest ~1 ms combined.

### SPIR-V instruction audit (noise-free signal)
- `spirv-dis` (v2026.4, `/usr/bin/`) disassembles `compiledshaders/*.spv`.
- A counting script pattern lives at /tmp/opencode/spivcount.py (may not
  persist; recreate): counts `OpLoad/OpStore`, image samples, `OpExtInst`
  transcendentals (Pow/Exp2/Sqrt/FClamp/FMax), `OpFDiv` per stage-file.
- On Intel Gen, `pow()` maps to a log2/exp2 path (~30 cycles) — **integer-
  exponent pows were replaced with repeated-square helpers** (`pow2..pow48`,
  see `shaders/toon.slang` / `shaders/scene.slang`), cutting toon.frag from
  8 `OpPow` to 1 `Sqrt` and scene.frag from 3 `OpPow` to 1 (the GGX spec
  exponent is variable — must stay). SSAO/gamma pows have variable exponents;
  left alone. Frozen-time pixel diff of pow-expanded vs pow shaders:
  mean 0.14/255 (bit-identical to the eye).
- VS (toon/scene, ~560 instrs) is already 1 dependent row load + vertex
  stream; per-vertex `materials[gs.material].flags` FACE test could fold into
  `SceneGroupShade` (one more load removal candidate).

### Scene row consolidation (done, in working tree)
- The old per-vertex chain vis -> slot -> {instances, squash, stretch, wiggle,
  bloom, face} (4-7 dependent/random loads per VERTEX at ~1 M verts) became
  one 32-byte `SceneVsRow` written once per SURVIVOR by `cs_scatter`
  (~3.8k rows/frame; ~1/250th the volume). `src/three_d/scene_shared.h`
  documents the layout; the draw push struct and read lists shrank accordingly.
- Measured effect at this resolution: Scene Draw 9.5 -> ~9.2 ms avg (~3%,
  inside rig noise; structural win presumed real but under-measurable here).


---

## 8. Reference: `vk_toon_shader/` (NVIDIA Vulkan-Samples "toon shading")

A self-contained sample proving that **cel banding is trivial and the hard part
is the outline, which is a post-process** (NOT geometry extrusion). Key pieces:

- `shaders/share.glsl`
  - `toonShading(L, N, nbSteps)`: `NdotL = floor(NdotL * (1 + 0.5/nbSteps) * nbSteps) / nbSteps`.
    `nbSteps <= 0` = plain Lambert. Result color `= max(0.1, intensity) * baseColor`
    (the `0.1` is the ambient floor). Used in both `rasterizer.frag` and
    `raytrace.rchit`.
- **G-buffer data** written by the geometry pass into one RGBA32F target:
  `xy` = normal (Lambert azimuthal equal-area `encode`, see `share.glsl`),
  `z` = linear depth, `w` = object/instance id (int cast to float).
- `post_objcontour.hpp` -> `contour_objects.frag`: the **object outline**, a
  full-screen fragment doing a 3x3 `texelFetchOffset` around X (A..H). Four
  selectable methods on the object id in `.w`:
  `0 smaller` (X<neighbour), `1 greater`, `2 thicker` (X!=any neighbour, bold),
  `3 different` (weighted: edges 1/6, corners 1/3). Output is an R8 mask.
- `contour_normaldepth.frag`: interior **crease/detail** lines from the same
  data target:
  - normal edge `D = saturate(NdotV) < 1` gate, then
    `grad_x/grad_y` Sobel (k0=1,k1=2) -> `normalGradient = |grad_x|+|grad_y|`,
    `Ngrad = smoothstep(2,3, normalGradient*coeff)`.
  - depth edge (Saito & Takahashi, uses 1/z): Sobel `g=(|A+2B+C-F-2G-H| +
    |C+2E+H-A-2D-F|)/8`, Laplacian `l=(8X-A-B-C-D-E-F-G-H)/3`,
    `depthGradient=(l+g)*coeff`, `Dgrad=smoothstep(0.03,0.1, depthGradient)`;
    `edge = Ngrad + Dgrad`.
- `comp_depth_minmax.hpp` (compute) supplies nearest/farthest depth to
  normalize the `1/z` term.
- `post_compositing.hpp`: full-screen blend of the tonemapped color with each
  mask: `color.xyz = mix(color.xyz, lineColor, edgeMask.r)` (normal/depth inner
  lines, then object outline).
- `kuwahara*` / `kuwa_aniso` (optional painterly filter), `fxaa.frag`
  (`post_effect.hpp`) to anti-alias the jaggy lines, `tonemap.frag`.
- `example.cpp` / `example.hpp` orchestrate the pass chain.

### Status: BROUGHT IN (done)
Implemented, differing from the plan above in three deliberate ways:
1. **Data target**: a second color attachment (`r->outline_data[image]`, full-res
   RGBA32F `IMG_COLOR|IMG_SAMPLED`) on the scene/toon draw. Writes the
   *geometric* world normal in xyz and the outline key in w (`id 0 = sky`, kept
   by the draw pass's LOAD_CLEAR on that attachment). The outline key is
   computed in the vertex stage: `mesh = group/lod_count` for props (counts[6]
   of SceneDrawPush carries lod_count), and tagged `0x80000000 | (slot/1...)` —
   i.e. `0x80000000 | (slot >> 1)` — for gel slimes and their face decal, so a
   slime's body and face share one key (no line between them) while adjacent
   slimes still separate. Mesh granularity (not slot) is deliberate: plants of
   the same species read as one drawn mass.
2. **Edge detect lives inside `postprocess.slang`'s cs_main**, not a separate
   pass: it already runs full-res over the frame with depth at hand and writes
   the only downstream consumer, so a dedicated pass would cost a full-res
   intermediate buffer plus barriers for nothing. It reads outline_data + the
   depth target point-sampled (`Load`), composites `ink = object edges
   (weighted 1/6 sides / 1/3 diagonals) + normal Sobel (Kroon k0/k1) + Saito
   depth Sobel+Laplacian` and darkens linear HDR **before exposure**.
   Under reverse-Z with an infinite far plane the Saito 1/z term collapses to
   `F = 1 - d` (proof at the call site) — no depth-minmax compute pass needed.
3. **Ink replaced the per-style fresnel outline everywhere** — `toon.slang` no
   longer has the `outline` variable/`col *= 1-outline`; strength and ink color
   ride the PostPush tail per `toon_style` (cel 1.0 navy, soft 0.6 lighter,
   painterly 0.8 warm ink; PBR scenes get the cel ink), blend-mixed toward ink
   like the reference's compositing pass.
Plumbing: `GameHooks.render` gained the outline target parameter (renderer.h,
main.c); `scene_frame(scene, cmd, color, depth, outline_data)`; scene/toon
pipelines are `color_attachment_count = 2` (sparkle stays 1); toggle is
`post.toon_outline` (UI checkbox "Toon outline", JSON key `toon_outline`, env
`MU_TOON_OUTLINE`). Runtime-verified headless: 39 frames, ~14.6 ms on the
Intel iGPU, no validation complaints; `rt_create` shows the outline_data
target. FXAA on the lines NOT done (SMAA already runs after post, so the
composite is antialiased there anyway — post writes ldr which SMAA consumes... verify order if changing pass order).

Doing the outline as geometry extrusion (inverted hull) is the other common
technique, but the sample's post-process approach avoids a second draw of every
mesh and naturally produces interior crease lines.

---

## 9. Working agreement for agents

- Follow `AGENTS.md` (DOD: data layout first, IDs over pointers, existence over
  flags, measure before/after, prefer `mylib`).
- When you add/modify a pass, update `renderer.txt`. When you add/remove a
  table, buffer slice, or push field, update `dataflow.txt`. When you change
  shared structs, update both C and Slang sides and the `_Static_assert`s.
- When you finish a task, **update this file** with anything a future agent would
  otherwise have to re-discover (new env knobs, gotchas, feature state).
- Don't commit unless asked. Don't read screenshots back into context.
