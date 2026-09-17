# Cozy Builder: game roadmap and proposed 3D API

Status: **approved direction; proposed APIs and game systems, not implemented**.
Reviewed against the workspace on 2026-09-17. This is the canonical plan for a
Tiny Glade-inspired building game on mu_gfx, not a claim about Tiny Glade's
internal techniques. Use original assets and an original visual identity.

## Contents

1. [Vision and scope](#1-vision-and-scope)
2. [Existing engine and dependencies](#2-existing-engine-and-dependencies)
3. [Legacy 3D renderer review](#3-legacy-3d-renderer-review)
4. [Foundation work](#4-foundation-work)
5. [Architecture and ownership](#5-architecture-and-ownership)
6. [Resource and scene API](#6-resource-and-scene-api)
7. [Frame preparation and rendering API](#7-frame-preparation-and-rendering-api)
8. [Construction API and world data](#8-construction-api-and-world-data)
9. [Procedural geometry](#9-procedural-geometry)
10. [Lighting and visual style](#10-lighting-and-visual-style)
11. [Input and editing](#11-input-and-editing)
12. [Publication and GPU lifetimes](#12-publication-and-gpu-lifetimes)
13. [Persistence](#13-persistence)
14. [First-scene tutorial specification](#14-first-scene-tutorial-specification)
15. [Wall-tool tutorial specification](#15-wall-tool-tutorial-specification)
16. [Milestones and migration](#16-milestones-and-migration)
17. [Tests and performance](#17-tests-and-performance)
18. [Decisions and deferred work](#18-decisions-and-deferred-work)
19. [Documentation and validation status](#19-documentation-and-validation-status)

## 1. Vision and scope

Build a small editable architectural diorama. The player shapes a clearing,
draws walls, places buildings and towers, adjusts roofs, adds openings, paints
paths, and decorates the result. Editing should feel direct and forgiving.

Visual priorities, in order:

1. Appealing building proportions and readable silhouettes.
2. Soft architectural edges, roof thickness, and restrained variation.
3. Warm sunlight, cool ambient shade, and grounded contact shadows.
4. Muted stone, plaster, wood, and roof palettes.
5. Dense but controlled grass, bushes, and trees.
6. Smooth camera and construction interactions.

A sophisticated renderer will not make arbitrary assets match this style.
Establish a reference cottage before building a large toolset.

### First useful release

- One bounded grassy world, one primary viewport, existing Linux/RGFW backend.
- Orbit/pan/zoom camera; perspective with a relatively narrow field of view.
- Straight walls, rectangular buildings, and round towers.
- Gable, hip, and conical roofs for explicitly supported footprints.
- Doors and windows hosted on walls.
- Heightfield terrain brush and terrain-following paths.
- Trees, bushes, grass clumps, and a small prop catalog.
- Selection, drag preview, cancel, undo/redo, save/load, and screenshots.
- Sun/ambient light, filtered shadows, gentle fog, tonemapping, and SMAA.

No economy, combat, multiplayer, infinite terrain, caves, general CSG, arbitrary
roof intersections, or destruction simulation in the first release. Skeletal
animation, ray tracing, a general ECS, and a generic editor framework are not
prerequisites. The first game is a creative sandbox, not a complete clone.

Provisional performance goal: 60 FPS at 1080p on declared reference hardware.
This is not a measured result or guarantee. Choose hardware before setting scene
limits. Use milestone gates rather than calendar estimates until prototypes
establish the geometry, lighting, and interaction workload.

## 2. Existing engine and dependencies

Source is authoritative; older proposals describe more than is implemented.

| Facility | Status and intended use |
|---|---|
| Vulkan dynamic rendering, bindless resources | Implemented in [main.c](../main.c); retain as backend |
| Pass declarations and `LoadOp`/`StoreOp` | Implemented; extend for concrete 3D requirements |
| Fullscreen pipeline preset | Implemented; leave fire/SMAA behavior unchanged |
| HDR postprocess and SMAA | Implemented; reuse after scene rendering |
| Buffer pools, staging, timeline, deletion queue | Implemented mechanisms; audit new usage and ownership |
| RGFW and polling input | Implemented; follow [input layering](input-api-layers.md) |
| Nuklear | Current UI; do not reintroduce GLFW or ImGui |
| Slang, cglm, stb, meshoptimizer, mu containers | Present dependencies; use where relevant |
| Static scene API, procedural building tools | Proposed here; not implemented |
| glTF importer | Legacy reference only; integration unresolved |
| Geometry workers and animation | Deferred; not implied by existing type names |

The [Makefile](../Makefile) does not compile [threedrenderer.c](../threedrenderer.c).
Its old `renderer.h`, `passes.h`, `tinytypes.h`, and `vk.h` dependencies were not
found in the inspected workspace locations. Its mesh/skinning shaders are absent
from the current shader directory. The cgltf include in [ext.c](../ext.c) is
commented out. Resolve that dependency explicitly when the static importer needs
it; do not silently introduce a new library now.

The [current renderer reference](rendererdesign/README.md) is newer than
[the historical renderer reference](rendererdesign.md), but includes outdated
platform/UI and upload-lifetime descriptions. The text-editor proposals are not
construction-editor implementations and do not define this game's architecture.

## 3. Legacy 3D renderer review

Source-level review, not a successful build or runtime audit. References are to
`/home/lk/myprojects/voxelfun/mu_gfx/threedrenderer.c` on 2026-09-17; use function
names when line numbers change.

### 3.1 Keep the architecture

```text
immutable assets -> scene instances -> prepared frame data -> GPU execution
```

Keep generation-bearing handles, dense active iteration, persistent/transient
separation, and preparation before recording. Separate CPU/GPU layouts only
where their access patterns justify conversion. Do not port the demo loop,
globals, allocation macros, or animation scaffolding wholesale.

### 3.2 Findings and migration

| Priority | Evidence | Finding and replacement |
|---|---|---|
| P0 | Includes 32-35; Makefile sources | File is not integrated. Port tested units, not the entire translation unit |
| P0 | `render_frame`, 2775-2804 | One model supplies geometry/material addresses. Build scene-wide compatible batches |
| P0 | `rendering_system_upload_frame`, 2528-2579 | Frees GPU buffers, reallocates, and submits uploads each frame. Use frame-slot storage |
| P0 | `asset_release_model`, 2231-2260 | Refcount-zero immediately frees GPU resources without visible final-use tracking. Separate ownership from retirement |
| P1 | `scene_spawn`, 709-778; `scene_destroy`, 781-798 | Weak asset handles are stored without retain/release. Choose an explicit application-owned resource policy |
| P1 | Static import, 1906-1939; `frame_emit_draws`, 2387-2407 | Primitive-local positions receive only the scene instance transform. Preserve glTF node-to-mesh occurrences |
| P1 | `model_asset_evaluate_animation`, 988-1045 | Shared asset TRS and palettes are mutated. Move pose state to animated instances when animation is implemented |
| P1 | Same evaluator, 998-1025 | Base TRS is not restored; clip switching can retain prior values. Preserve immutable base transforms |
| P1 | Same evaluator, 1028-1034 | Parent globals are read in source-array order. Precompute parent-before-child order |
| P1 | `asset_node_init_from_gltf`, 909-935; evaluator | Matrix-only nodes are later reconstructed from default TRS. Preserve base matrices |
| P1 | `animation_sample_values`, 938-985 | Non-step modes use component lerp. Cubic-spline layout and shortest-path quaternion interpolation need dedicated handling or rejection |
| P1 | `frame_emit_skin_job`, 2338-2368 | Instances share asset palette/output ranges; first skin supplies joint count. Not an independent multi-instance pose system |
| P1 | `asset_upload_rgba_texture`, 81-91; materials 1661-1688 | All images use sRGB despite normal/metallic-roughness usage. Import by color-space semantic and supply mipmaps |
| P1 | `asset_import_model`, 2184-2223 | Cache uses path hash without path equality or import options. Hashes accelerate equality, not replace it |
| P2 | `rendering_system_build_frame`, 2410-2438 | Rebuilds visible transforms despite dirty sets. Cache transforms and world bounds |
| P2 | `frame_build_gpu_instances`, 2451-2487 | Copies nearly identical records into new scratch. Prepare upload records directly where possible |
| P2 | `build_indirect_commands`, 2703-2742 | One command per matching instance; always LOD 0. Batch compatible instances and advertise only implemented behavior |
| P2 | `rendering_system_prepare_indirect_draws`, 2745-2771 | Allocates/frees command scratch on each call. Reuse frame capacity |

Missing one-time submission helpers may wait internally. This is not a reproduced
GPU use-after-free finding: safety is either hidden behind waits or absent from
the visible contract. Neither should carry into frequent editing. Recheck buffer
barriers with the actual backend and shaders during migration.

`frame_emit_draws` stores mesh-local bounds. Transform them conservatively before
world-space culling, including nonuniform scale. Do not treat the field name as
proof that it already contains a world-space sphere.

### 3.3 Initial importer contract

Support static indexed triangles, selected-scene hierarchy, repeated mesh
occurrences, transforms, bounds, base color, roughness, and masked foliage.
Preserve explicit matrices; do not silently decompose shear into lossy TRS.
Document missing-normal generation and unsupported primitive behavior. Reject
unsupported skins, morphs, compression, and material features with diagnostics
before publishing resources. A failed import leaves no published model ID.

Separate CPU import from GPU publication. Cache identity includes canonical path,
content-affecting options, and collision-safe path comparison. Texture identity
includes color-space usage, not just image identity. Treat the importer as an
external-data boundary; the render loop consumes already-valid data.

## 4. Foundation work

### 4.1 Pass correctness

`begin_pass` in `/home/lk/myprojects/voxelfun/mu_gfx/main.c:3199` treats zero color
attachments as compute even when depth exists. Fix depth-only graphics and its
pipeline creation path before shadows. Compute-only passes have no rendering
scope; `end_pass` pairs only with graphics scopes. Initially exclude
attachment-less raster passes rather than making scope ownership ambiguous.

Audit upload-to-vertex/index/storage reads, upload-to-indirect reads, shadow
writes-to-sampling, attachment loads/blending, and consecutive image writes.
`rt_transition_all` skips identical states; equal layout/stage/access alone does
not eliminate a dependency required by a later write. Add focused usage tests,
not a duplicate Vulkan validation layer or unrelated enum wrappers.

### 4.2 Frame storage

`frame_start`, lines 4727-4751 in the inspected main file, waits for one frame
slot and resets a shared CPU linear pool. Do not assume this protects new data
read by other outstanding frames. Give the 3D path frame-slot upload storage, or
timeline-retired ranges. Reuse only after that range's final submission completes.
Keep persistent geometry out of transient storage. Normal edits must not use
`wait_idle` to make replacement safe.

### 4.3 Build and API boundary

The current Makefile shares object paths across configurations and lacks header
dependency generation. Before regression gates, separate debug/release/sanitizer
outputs and add compiler dependency tracking. Verify that sanitizer binaries
contain instrumented project objects instead of relying on target names.

Extract the minimum reusable backend declarations/implementation needed by the
static example. Preserve the demo entry point and behavior. Do not migrate to
NoGraphicsAPI or redesign every low-level resource in the same change.
Initialization/resource failures stay explicit and checked immediately. No new
allocation-failure branches; no aborts for programmer errors. Use debug asserts
for wrapper invariants and external-input errors at loading boundaries.

## 5. Architecture and ownership

| System | Owns | Does not own |
|---|---|---|
| `BuildWorld` | Semantic objects, terrain, persistent IDs, history | GPU buffers or renderer handles |
| `BuildGeometry` | Dirty work, CPU results, generator revisions | Authoritative construction intent |
| `Scene3D` | Render instances, transforms, bounds, visibility | Building rules, undo, asset files |
| `RenderResources` | Mesh/material tables and GPU allocations | World semantics |
| `Renderer3D` | Frame storage, pipelines, targets, prepared batches | Asset parsing or construction tools |
| Optional `ModelLibrary` | Imported CPU content and its resource bundle | Global game state |

```text
Input -> tool transaction -> BuildWorld -> dirty geometry
                                           |
                                           v
                             owned CPU result -> GPU publication
                                                       |
                                                       v
Scene3D -> resolve/update/cull/group -> PreparedScene -> record -> submit
```

The application owns these systems and passes them explicitly. Runtime handles
are generation-bearing wrappers around mu handle machinery; zero means invalid.
A persistent `WorldObjectId` is separate from a recyclable runtime slot. A game
object can map to several render instances through a dense render-binding table.
The renderer does not know that a mesh is a tower or that a window belongs to a wall.

Use dense records for full-record iteration and separate hot transform/bounds
arrays where culling benefits. Keep paths, names, and source metadata cold.
Reserve capacity at initialization/preparation boundaries. No huge static world
arrays, per-brick objects, per-draw heap work, or long-lived pointers into growable
storage. Add missing reusable primitives to external/mu, not an ad-hoc container.

Proposed modules, created incrementally rather than as empty scaffolding:

| Absolute path | Responsibility |
|---|---|
| `/home/lk/myprojects/voxelfun/mu_gfx/src/render3d.h` | Resource, scene, and rendering API groups |
| `/home/lk/myprojects/voxelfun/mu_gfx/src/render3d.c` | Static 3D implementation; split after concrete growth |
| `/home/lk/myprojects/voxelfun/mu_gfx/src/build_world.h` | Semantic world and transactions |
| `/home/lk/myprojects/voxelfun/mu_gfx/src/build_world.c` | World mutation, history, persistence boundaries |
| `/home/lk/myprojects/voxelfun/mu_gfx/src/build_geometry.c` | Local procedural generation and publication |
| `/home/lk/myprojects/voxelfun/mu_gfx/examples/cozy_builder.c` | Application wiring and tools |

No general scene hierarchy is required initially: the game computes world
transforms; imported models retain immutable internal node transforms.
