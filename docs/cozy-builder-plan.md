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

## 6. Resource and scene API

**Specification only.** These types/functions do not exist yet. Tables describe
signatures and contracts, not a compilable header. Implement only the milestone
subset with full definitions and tests; do not add empty implementations.

### 6.1 Common representation and defaults

All array views use custom mu spans, passed by value. Typed span names below
mean read-only element views generated through the shared mu primitive. The
current renderer has a local `ByteSpan`, not a confirmed shared typed-span API.
Promote/consolidate it in external/mu when introducing typed spans; never add a
competing local span definition. `ByteSpan` counts bytes; typed spans count
elements. If `GpuRange` is introduced later, pass it by value too; do not expose
allocator ownership as a render-API parameter.

| Proposed type | Fields/meaning |
|---|---|
| `MeshId`, `MaterialId`, `InstanceId`, `ModelId` | Distinct generation-bearing handle types, zero invalid |
| `Transform3D` | cglm-compatible position, quaternion `(x,y,z,w)`, positive three-axis scale |
| `Bounds3D` | Minimum/maximum corners; descriptor bounds are mesh-local |
| `MeshVertex` | Float position, normal, UV initially; establish a tested packing boundary later |
| `MeshSection` | First index, index count, material slot; no GPU address |
| `MeshDesc` | Read-only vertex/index/section spans and local bounds |
| `MaterialDesc` | Linear base-color factor, roughness, metallic, emissive factor, texture IDs, alpha mode/cutoff, double-sided flag |
| `InstanceDesc` | Mesh, transform, material-binding span, linear tint, visibility/shadow flags |
| `InstanceSnapshot` | Copied mesh/transform/tint/flags, not a mutable storage pointer |
| `TransformUpdate` | Instance ID plus transform for batched updates |

Initial indices are `uint32_t`; one vertex layout and triangle topology keep the
API narrow. Mesh sections identify material slots, not owning material objects.
An instance supplies one valid material per referenced slot. Mesh data is copied
at creation; later mutation of caller arrays has no effect. Empty geometry emits
no instance rather than a partially valid mesh resource.

`transform3d_identity()` returns zero position, identity quaternion, unit scale.
`material_desc_default()` returns white, rough nonmetallic opaque material,
no emission, and documented fallback texture references. Choose and document a
single missing-texture sentinel at integration; do not assume existing zero and
`UINT32_MAX` conventions are interchangeable. `instance_desc_default()` supplies
identity transform, white tint, visible/casts-shadow flags; mesh and material
bindings remain mandatory. Zero-initialized transforms are not identity.

Geometry-generated normals must be finite and normalized. Initially reject
nonpositive instance scales at the scene boundary; mirrored imports are baked
with corrected winding or explicitly rejected. Normal matrices use inverse
transpose for nonuniform scale. Mesh bounds and material-slot invariants are
checked at preparation/creation boundaries, not reconstructed inside draw loops.

### 6.2 Shared resource lifetime policy

Application ownership is explicit. Scene membership does not retain resources.
Resources must outlive attached instances, prepared packets, recorded commands,
and submitted GPU uses. Destruction is immediate. Detach future scene uses, then
wait for final use or enqueue destruction through the optional deletion queue.
Do not hide GPU waits/refcounts in setters. See section 12 for publication order.

| Proposed signature | Result and contract |
|---|---|
| `bool render_resources_init(RenderResources *resources, Renderer *backend, const ResourceLimits *limits)` | Initialization failure checked immediately; caller owns system |
| `void render_resources_shutdown(RenderResources *resources)` | All scene/prepared/GPU uses ended; destroys remaining owned resources |
| `MeshId mesh_create(RenderResources *resources, const MeshDesc *desc)` | Copies CPU inputs into owned pending-upload storage; invalid ID for resource-creation failure |
| `void mesh_destroy(RenderResources *resources, MeshId mesh)` | Immediate; shared lifetime preconditions apply |
| `MaterialId material_create(RenderResources *resources, const MaterialDesc *desc)` | Copies descriptor and resolves texture references; invalid ID for creation failure |
| `void material_destroy(RenderResources *resources, MaterialId material)` | Immediate; does not destroy borrowed textures |
| `bool model_load(ModelLibrary *library, const ModelLoadDesc *desc, ModelId *out_model, LoadError *error)` | Optional static import; on failure output is invalid and no asset is published |
| `void model_unload(ModelLibrary *library, ModelId model)` | Releases library-owned bundle after scene/GPU use ends |

`ModelLibrary` borrows the backend/resource systems established at its own paired
init/shutdown boundary. A model owns its mesh/material/texture bundle; users borrow
resource IDs and remove their instances before unloading. Introduce model deduplication
only with an explicit ownership protocol, not the old implicit cache refcount.

Created meshes become GPU-readable only after `renderer3d_prepare` records their
pending uploads and visibility dependencies on the recording command buffer.
An ID denotes owned storage, not completed GPU work. CPU source views expire at
creation return; pending upload storage survives until consumed and retired.

No public in-place `mesh_update` initially. Create a replacement and retire the
old one. Material edits similarly publish replacements, while per-instance tint
is a frame parameter. Add fixed-topology subrange updates only after measurement.

### 6.3 Scene operations

| Proposed signature | Contract |
|---|---|
| `void scene3d_init(Scene3D *scene, const SceneLimits *limits)` | Reserve CPU tables; limits are reservations, not silent truncation thresholds |
| `void scene3d_shutdown(Scene3D *scene)` | Release CPU tables after prepared users expire; does not destroy borrowed resources |
| `InstanceId scene3d_add(Scene3D *scene, const InstanceDesc *desc)` | Copy descriptor and material-binding contents; mark caches dirty |
| `void scene3d_remove(Scene3D *scene, InstanceId instance)` | Remove from future frames; already-prepared frames remain immutable |
| `void scene3d_set_transform(Scene3D *scene, InstanceId instance, const Transform3D *transform)` | Dirty world transform and bounds |
| `void scene3d_set_mesh(Scene3D *scene, InstanceId instance, MeshId mesh)` | Requires compatible material slots; dirty bounds and batches |
| `void scene3d_set_materials(Scene3D *scene, InstanceId instance, MaterialIdSpan materials)` | Copy bindings; every mesh slot must have a material |
| `void scene3d_set_flags(Scene3D *scene, InstanceId instance, uint32_t flags)` | Visibility and shadow participation |
| `void scene3d_set_tint(Scene3D *scene, InstanceId instance, const vec4 tint)` | Linear color multiplier; no geometry rebuild |
| `void scene3d_apply_transforms(Scene3D *scene, TransformUpdateSpan updates)` | Batched mutation; span by value |
| `bool scene3d_get(const Scene3D *scene, InstanceId instance, InstanceSnapshot *out)` | Stale query returns false; no writable storage pointer |

Setters require valid instances; debug assertions enforce misuse. Resource
handles are resolved at preparation. Missing resources are programming errors,
not reasons to silently skip draws. For changed material-slot structure, remove
and add the render instance in one publication transaction before preparation.
The persistent game ID is unchanged. Avoid unrestricted `scene_write` mutation.

## 7. Frame preparation and rendering API

### 7.1 Proposed interface

| Proposed signature | Contract |
|---|---|
| `bool renderer3d_init(Renderer3D *renderer, Renderer *backend, RenderResources *resources, const Renderer3DDesc *desc)` | Create pipelines, targets, frame storage; check initialization failure immediately |
| `void renderer3d_shutdown(Renderer3D *renderer)` | After GPU drain, destroy owned state, not borrowed backend/resources |
| `bool renderer3d_resize(Renderer3D *renderer, uint32_t width, uint32_t height)` | Safe target recreation boundary; report recreation failure immediately |
| `void renderer3d_prepare(Renderer3D *renderer, Scene3D *scene, const View3D *view, const Environment3D *environment, FrameContext *frame, VkCommandBuffer cmd, PreparedScene *out)` | Reclaimed slot: update, cull, group, stage, record uploads/dependencies |
| `void renderer3d_record(Renderer3D *renderer, VkCommandBuffer cmd, const PreparedScene *prepared)` | Fixed 3D pass sequence; command buffer already recording; no submit/present |
| `void renderer3d_stats(const Renderer3D *renderer, Render3DStats *out)` | Copy counters and completed timings |

`Renderer3DDesc` specifies initial capacities, output format, and shadow quality.
`View3D` contains matrices, camera position, near/far distances, and framebuffer
viewport. `Environment3D` contains sun direction/color/intensity, ambient sky and
ground colors, fog, and exposure. Copy needed descriptor values into frame storage.

`PreparedScene` refers to resolved batches, per-view constants, upload ranges,
and output targets owned by the frame slot. It expires when that slot is reclaimed.
Do not retain mutable world arrays or caller descriptors. No scene/resource
mutation between prepare and record. Use the same frame and command buffer for
both. Application code owns submission/presentation and records UI exactly once
after tonemapping/composition.

### 7.2 Preparation and shader contract

1. Update dirty transforms and conservative world bounds.
2. Resolve handles once at the boundary.
3. Cull the camera view and build a separate shadow-caster list.
4. Use LOD 0 initially; later select projected-size LOD with hysteresis.
5. Group by pipeline, mesh section, and material compatibility.
6. Write contiguous instance records and batch ranges into frame storage.
7. Stage persistent/frame uploads and emit transfer/read dependencies.

Begin with CPU culling and direct instanced draws through a narrow backend draw
helper. Multiple models/materials must work before indirect rendering. Repeated
compatible meshes share a draw. Add indirect commands only when submission time
justifies them. Execution never resolves an asset table per draw.

Proposed game coordinates: right-handed, Y up, meters, radians. Freeze camera
forward in the first test. cglm is column-major; choose Slang layout and multiply
convention explicitly. Test translated/rotated/nonuniformly scaled asymmetric
geometry. Account for the Vulkan viewport Y convention once, not in both camera
and shader. Use inverse-transpose normals under nonuniform scale.

Use depth [0,1], initially reversed-Z camera depth, clear 0, GREATER comparison.
Shadow projection, clear, comparison sampler, and bias sign must agree; do not
copy signs from forward-Z examples. Shared GPU layouts need compile-time
size/offset checks and shader readback tests. Retain the current root-payload
limits. Begin with float vertices and matrices; measure before packing vertices
or introducing affine 3x4 GPU transforms.

## 8. Construction API and world data

World intent, not triangles, is authoritative. `WorldObjectId` remains stable
across geometry replacement and undo; render instance IDs may change. Separate
dense object tables by concrete type. Variable control points/openings live in
owned contiguous mu arrays, referenced through offsets/ranges rather than pointers.

| Record | Semantic fields |
|---|---|
| `WallDesc` | Centerline point span, base height, wall height, thickness, style, seed |
| `BuildingDesc` | Footprint point span, elevation, wall height, roof kind/pitch/overhang, style, seed |
| `OpeningDesc` | Host wall ID, stable segment ID, local distance/elevation, width/height, shape |
| `PathDesc` | Control point span, width, edge softness, material/style, seed |
| `TerrainBrush` | Center, radius, strength, mode, falloff, target height when relevant |
| Decoration record | Asset/style reference, transform, placement seed, ground attachment |
| `EditTransaction` | Original semantic values, changed IDs, owned variable payloads |

No universal component registry is needed. Rectangular footprints are the initial
building subset. Overlapping or self-intersecting footprints receive a tool-level
invalid-preview result, not a renderer error.

| Proposed operation | Contract |
|---|---|
| `build_world_init` / `build_world_shutdown` | Caller-owned world and initial capacities |
| `build_edit_begin` / `build_edit_commit` / `build_edit_cancel` | One active transaction; one committed drag equals one history entry |
| `build_add_wall` / `build_set_wall` / `build_remove_wall` | World + descriptor/ID; copy input views and mark local dependencies |
| `build_add_building` / `build_set_building` / `build_remove_building` | Semantic footprint/roof changes; no Vulkan calls |
| `build_add_opening` / `build_remove_opening` | Attach/detach opening from stable wall feature |
| `build_set_path` / `build_remove_path` | Copy path points; invalidate terrain-following mesh and vegetation exclusion |
| `build_sculpt_terrain` | Apply brush to affected samples; record reversible sample changes |
| `build_undo` / `build_redo` | False if history empty; restore semantic data and dirty dependencies |
| `build_save` / `build_load` | Explicit external I/O/data error output |
| `build_geometry_prepare` | World + dirty selection + budget + owned result storage; CPU-only generation |
| `build_geometry_publish` | Resources + scene + owned results + render bindings; frame-boundary publication |

Full C signatures for construction operations are finalized with their first
concrete tool, not an unused generic command framework. Every array parameter
uses a mu span by value; descriptors use const pointers. Growth is allowed at
preparation boundaries; never truncate an edit silently.

Transactions copy semantic payloads, never retain caller spans. Repeated previews
update the working edit while preserving the original before-image. Cancel
restores it. Commit captures the final after-image and drops redo history. A new
edit after undo does not reuse a persistent object identity for a different object.

History is bounded by an explicit memory policy; eviction preserves current world
state and saved/dirty identity. Undo of deletion restores the same persistent ID
with new runtime handles. Start with before/after records, not a command scripting
language. Terrain strokes store touched samples once plus final values.

## 9. Procedural geometry

### 9.1 Walls and openings

Start with straight centerline segments. Offset each side by half thickness,
construct side/top/end surfaces, and generate bounded joins. Acute joins use a
bevel fallback instead of unbounded miters. Reject zero-length segments in the
tool before mesh generation. Define a world-unit geometric tolerance, test it
across supported building scales, and avoid frame-dependent epsilon decisions.

Place openings in wall-local coordinates. Split side panels around rectangular
openings, generate reveal thickness, then add trim. Arch openings are a later
bounded tessellation variant, not arbitrary booleans. Adjacent openings must
respect a minimum pier width; the tool shows constrained/invalid placement.
When a wall shortens, clamp opening placement under a documented policy or keep
the edit invalid; never silently delete an opening.

Generate most brick detail in materials. Add geometry for silhouette stones,
corner trim, window surrounds, roof edges, and selected close-up details.
Do not allocate a scene instance per brick. Chunk geometry by rebuild unit and
material section rather than creating one world-wide mesh or thousands of tiny
independent objects. Inspect normals, caps, and winding in a debug view.

### 9.2 Buildings, towers, roofs

1. Rectangular walls and a gable roof establish the first cottage.
2. Add hip roofs with explicit ridge/eave construction.
3. Add round towers from bounded radial segments and conical roofs.
4. Add supported compound footprints only with explicit join rules and tests.

Roof thickness, overhang, and ridge shape are semantic parameters. Keep tile
detail subordinate to the silhouette. Footprint validation occurs before
triangulation. A simple footprint triangulator may be reusable mu geometry work;
do not add an unconfirmed third-party geometry library by accident.

Initially buildings may stand beside each other without automatic merging.
Later roof/wall intersections need local dependency edges and deterministic
intersection rules. General watertight CSG and arbitrary roof networks are not
promised extensions of the rectangle generator.

### 9.3 Terrain and paths

Use heightfield chunks with shared border sampling rules. Brush updates expand
dirty regions by the normal/filter radius. Neighbors must compute identical
shared-edge heights and normals. Store authoritative samples on CPU for picking,
undo, and saves. No volumetric terrain initially.

Paths are terrain-following ribbons with bounded segments, consistent UV scale,
edge softness, and local vegetation exclusion. Prevent z-fighting with an explicit
surface/decal strategy, not arbitrary large offsets. Rebuild affected ribbon
sections after terrain edits. Path crossing under a wall does not automatically
cut an arch in v1; that is a separate semantic interaction feature.

### 9.4 Dependency and variation rules

| Edit | Dirty work |
|---|---|
| Wall endpoint | Wall plus adjacent joins |
| Wall height | Wall, hosted openings, attached roof boundary |
| Footprint | Walls, roof, footprint-dependent decoration |
| Terrain brush | Affected chunks, intersecting paths, ground attachments |
| Path | Ribbon and nearby vegetation exclusion |
| Tint | Instance/material parameters only |

Start with direct adjacency and a uniform spatial grid for affected-region
queries. Deduplicate dirty IDs. Budget generation work; prioritize the selected
object, then nearby dependencies. Use coarse preview geometry while expensive
detail is pending. Completion eventually converges to a full reference rebuild.

Variation key: world seed + persistent object ID + stable semantic feature key.
Do not use global random iteration order, floating-point memory bytes, or a
frame counter as identity. Stable feature keys prevent an unrelated edit from
reshuffling every brick. Generator version is part of cache identity. Determinism
means identical semantic output on supported builds; do not promise cross-platform
bit-identical floating-point vertices without testing that separately.

## 10. Lighting and visual style

Create a style sheet before many tools: cottage proportions, bevel widths,
roof thickness, stone/plaster palettes, foliage shape, and detail size at normal
camera distance. Avoid noisy normals and high-frequency albedo contrast. Use
geometry where it changes shape and materials where it changes surface detail.

Initial lighting: directional sun, sky/ground ambient, rough nonmetallic surfaces,
filtered depth shadows, gentle height/distance fog, existing tonemapping/SMAA.
A bounded world can start with one shadow map. Add cascades only when the camera
zoom range makes resolution unacceptable. Stabilize the shadow projection to
avoid shimmering; evaluate acne, detached shadows, thin walls, and foliage.

Foliage uses instanced grass/bush/tree meshes, alpha masking, distance density
reduction, and seeded wind. Share deformation between color and shadow shaders.
Limit distant grass shadow cost. Dense alpha-blended grass is not the default:
overdraw/sorting cost should not dominate the first scene.

Initial pass order:

1. Upload persistent edits and frame data.
2. Sun shadow depth, including relevant off-camera casters.
3. Opaque terrain and architecture into HDR/depth.
4. Alpha-masked vegetation with depth testing.
5. Tool preview and selection overlay under an explicit depth policy.
6. Existing tonemapping and SMAA, once each.
7. Composite to swapchain, Nuklear UI, present.

For later contact AO, choose a depth/normal prepass plus ambient-light modulation,
or a documented deferred composition path. Do not indiscriminately darken direct
sunlight by multiplying the final lit image. Avoid strong SSAO halos. Temporal AA,
complex volumetrics, GI, transparent water, and photo-mode DOF follow stable
editing and a measured visual need, not precede them.

## 11. Input and editing

One RGFW poll per frame; layering rules follow [input-api-layers.md](input-api-layers.md).
The application owns tool modes and drag state, and forwards events to Nuklear
when UI is active. A click over UI must not place geometry behind it. Focus loss
cancels active drags and releases capture. Camera: right-drag orbit, middle-drag
pan, wheel zoom; left-drag belongs to the active construction tool. Clamp
pitch/zoom and keep motion frame-rate independent. Optional smoothing must not
add noticeable tool latency. Gamepad control is deferred.

Picking is CPU-side initially: terrain heightfield ray march, analytic rays for
tool handles, bounds-first triangle tests for props. Selection uses a small
tolerance for thin walls. Optional GPU ID picking stays deferred until CPU tests
prove insufficient. Tools operate on semantic objects; a texture-paint feature
would be a separate future proposal, not part of the construction API.

Tool transactions: capture original semantics, update preview per move, commit
on release, cancel on Escape/focus loss. Drag preview rebuilds only affected
dependencies. Show invalid-preview feedback. Terrain strokes preview against
the stroke's before-image and commit once per stroke.

## 12. Publication and GPU lifetimes

### 12.1 Publication

1. Generate replacement CPU geometry into owned storage.
2. Create replacement resources, copying input into owned pending-upload storage.
3. Publish their IDs to future scene preparation before command recording.
4. During prepare, record pending copies and read dependencies before draws.
5. Retire replaced storage after its final submitted use completes.

Scene bindings change when new IDs publish; GPU visibility follows upload and
synchronization in the consuming submission. Publication happens before prepare,
never during record. Unsupported tool edits keep the previous mesh visible; an
empty result is a normal state, not an error. A persistent object may map to zero
instances when it has no geometry, but never to a partially valid one. Backend
resource-creation failures are checked at creation, not retried inside draw loops.

A submission's retirement value must include work recorded in that submission,
including already-recorded final uses, not merely the last submitted value at
some earlier point. Deletion queues never defer work past explicit application
shutdown. At shutdown: stop producers, drain the submission timeline, drain every
deletion queue, then destroy resources and device state.

### 12.2 Buffer and image transitions

Buffer barriers needed at minimum: upload-copy to vertex/index reads, to
shader-storage/indirect reads, and shadow/geometry image writes to sampling.
Image transitions require matching layouts, stages, accesses, and queue ownership.
Do not skip an image dependency solely because source and destination states are
equal; two later writes still need ordering. Compute-write-then-graphics-read is
an explicit test. Define the image-retirement rule for intermediate targets once,
relying on the validation layer for Vulkan parameter confidence, not on broad
optimistic checks.

### 12.3 Frame-slot storage

Frame data and transient uploads are frame-slot owned or timeline-retired.
Reuse after the final covering submission completes. Persistent geometry lives
outside transient pools. Compare the existing ring staging design against
frame-slot staging during integration. Any change to shared pool reset behavior
carries a synchronization test before other systems depend on it.

### 12.4 Workers, optional and later

If generation moves off-thread: jobs receive immutable snapshots; results carry
object ID, generation, and source revision. Only the render thread publishes GPU
resources. Stale/deleted results are discarded; the previous valid mesh stays
visible while a replacement is pending. A bounded publication queue rejects or
merges superseded jobs. Threaded generation is deferred until measured CPU time
justifies it; use data-race detection when threads exist, not before.

## 13. Persistence

Versioned semantic format: header with format/generator versions, world seed,
object tables, terrain samples, decoration placements. Store seeds and
parameters, not vertex buffers. References use stable IDs and offsets into owned
variable arrays, never runtime handles or raw padded structs.

Load into a temporary world; validate; replace the active world only on success,
then rebuild scene bindings. Save to a temporary file and replace atomically
with explicit error output. On load failure the previous world remains active.
Serialize by hand into a small growing buffer; no reflection or schema codegen.
Never derive validity from file size; never trust declared counts without bounds.
Add backward migration only for real format breaks, with converter tests. The
default starter world is authored content, not an embedded generic template.
Screenshots and camera state are optional user settings, not world data.

## 14. First-scene tutorial specification

This becomes a buildable example with tests during Milestones 1 and 2. It is an
implementation recipe, not an executable snippet for today's engine.

### 14.1 Startup and content

1. Initialize existing window/backend/UI and check each initialization result.
2. Initialize `RenderResources`, `Scene3D`, then `Renderer3D`. Add `ModelLibrary`
   only when imported props are needed; procedural geometry needs no importer.
3. Create stone, plaster, roof, and grass materials from explicit defaults;
   provide linear factors and correctly interpreted color/data textures.
4. Generate CPU terrain, cottage, and tower meshes in reusable arena storage.
   Fill vertex/index/section spans and compute local bounds. Call `mesh_create`
   and check its result immediately. Reset generation scratch only after the
   function has copied the source data.
5. Create instances from identity defaults, valid mesh IDs, and section materials.
   Place two instances of one mesh plus another distinct mesh/material set.
6. Add a small authored tree after importer tests pass. Before that, use an
   explicitly temporary procedural tree silhouette, not a fake asset loader.
7. Set orbit target, near/far range, sunlight, and ambient lighting. Keep the first
   baseline free of animation, random camera motion, and auto-exposure changes.

### 14.2 Per-frame order

1. Poll input once, update UI ownership and camera.
2. Reclaim the chosen frame slot only after its completion value.
3. Publish ready geometry and update scene bindings before preparation.
4. Begin command recording. Call `renderer3d_prepare`: record uploads and build
   the packet. Call `renderer3d_record` on that packet without scene mutation.
5. Tonemap/SMAA/composite using the existing application integration; these passes
   must have one owner, never be run both inside and outside the 3D recorder.
6. Record Nuklear once, submit/present, and associate retirement with this submit.
7. Read completed profiling results without waiting on the just-submitted frame.

The initial 3D recorder owns shadow/opaque/masked/preview passes. The application
owns existing postprocessing and presentation. Its output is HDR/depth suitable
for those existing passes. This concrete split resolves the broader pass list
in section 10 without introducing a render graph.

### 14.3 Observable checks

- All three distinct geometry/material groups appear, not just the first model.
- Repeated meshes share compatible instance batches.
- Translation, rotation, and nonuniform scale are correct in color and shadows.
- Off-camera casters can shadow visible ground; camera culling does not hide them.
- CPU vertex input can be overwritten after creation without changing the mesh.
- Adding/removing instances and resizing does not invalidate outstanding frames.
- Fixed camera views include close facade, wide clearing, grazing sun, and foliage.

Shutdown: stop generation, end/cancel unsubmitted recording safely, call the
backend idle wait, drain all deletion queues, remove scene uses, unload models,
destroy 3D targets/resources, then destroy the device/window. Every submitted
frame must drain before device destruction. No queued callback may retain a
pointer to an already-destroyed owner.

## 15. Wall-tool tutorial specification

1. Add one straight wall with semantic dimensions and stable ID to `BuildWorld`.
2. Generate wall geometry independently of Vulkan; validate indices, normals,
   bounds, caps, and deterministic output with CPU tests.
3. Publish mesh/material bindings into the scene; store the world-to-render map.
4. On world pointer press, ray-pick the terrain or existing wall endpoint. UI
   ownership blocks this step. Start one transaction and capture original data.
5. During drag, convert the ray to the tool's constrained plane, snap if enabled,
   and validate minimum segment length before calling `build_set_wall`.
6. Deduplicate wall/join dirty IDs; regenerate a coarse preview within budget.
   Publish replacement geometry at the next frame boundary. Retire older meshes.
7. On release, commit one history entry and schedule final detail. On Escape or
   focus loss, restore the original data and invalidate any pending preview job.
8. Add an opening in wall-local coordinates. Test resize limits and host deletion.
9. Undo/redo the drag and deletion. Verify semantic equality and valid new runtime
   handles, not equality of GPU allocations.
10. Save/reload; compare semantic records and generated reference output.

Acceptance: a long drag creates one undo entry, unrelated walls do not rebuild,
no normal edit waits for device idle, and geometry replacement remains correct
with several frames in flight. Interleaved stale jobs cannot resurrect a deleted
wall or overwrite its newest revision. Worker tests begin only when workers exist.

## 16. Milestones and migration

All milestones below are planned, not delivered by this documentation change.

| Milestone | Deliverable | Exit gate |
|---|---|---|
| 0: foundation | Build variants/dependencies, minimum public backend boundary, safe frame uploads, depth-only passes | M0 tests below pass; existing demo still builds/runs |
| 1: static slice | Mesh/material/scene API, CPU preparation, multi-model draws, orbit camera | M1 CPU/GPU tests pass; no dependency on legacy animation |
| 2: visual benchmark | Cottage/tower/clearing/trees, sun/shadows/fog, constrained static importer | Fixed-camera references reviewed; CPU/GPU baseline recorded |
| 3: first tools | Walls, rectangle buildings, tower/roof parameters, local rebuilds | Drag/cancel correctness, bounded preview work, safe replacement |
| 4: usable sandbox | Openings, paths, terrain brush, undo/redo, save/load | Round-trip equality, no UI click-through, malformed-load preservation |
| 5: detail and scale | Vegetation, trims, density/LOD, publication budgets | Dense-scene timings, stable variation, no normal edit-time device waits |
| 6: polish | Supported joins, lighting controls, camera feel, photo tools | Visual regression and manual usability review |

### Small-change implementation sequence

1. Fix build variant isolation/header dependencies; prove clean rebuild behavior.
2. Add depth-only regression and fix graphics/compute scope classification.
3. Establish frame-slot upload ownership and synchronization regressions.
4. Extract only the backend interface needed for the static example.
5. Establish shared mu spans and resource handles with CPU tests.
6. Add immutable static meshes/materials and scene instance storage.
7. Add prepare/record using direct instanced draws and one vertex layout.
8. Add the static scene, then shadow rendering and art benchmark.
9. Resolve importer integration separately with a strict supported subset.
10. Add the first wall tool, then history/persistence and further construction.

Retain `/home/lk/myprojects/voxelfun/mu_gfx/threedrenderer.c` unchanged as a reference.
Do not make compiling it the migration objective. After replacements cover useful
behavior, a later explicit cleanup may archive it with provenance. Avoid mixing
unrelated Vulkan enum renames, backend migrations, UI rewrites, or warning cleanup
into these changes. Preserve existing uncommitted workspace work.

## 17. Tests and performance

### 17.1 Milestone 0 tests

| Test | Fixture/action | Required observation |
|---|---|---|
| Build variants | Build debug, release, sanitizer consecutively without manual clean | Distinct object paths and expected compiler flags; sanitizer objects instrumented |
| Header dependencies | Touch a shared header in a test checkout | All dependent objects rebuild; unrelated objects do not |
| Depth-only | Rasterize known geometry into depth with zero colors, then sample/read back | Known covered/uncovered depth values; graphics scope paired; no validation errors |
| Compute scope | Dispatch existing postprocess without attachments | No graphics rendering scope begun/ended for compute |
| Attachment preservation | Clear a target then draw a partial region with `LOAD_KEEP` | Untouched pixels remain; explicit read/write dependency validated |
| Repeated writes | Two passes use same image state, then read back | Correct ordered result even when layouts match |
| Frame uploads | Distinct patterns for at least three outstanding frames; delay completion | No earlier frame observes later frame data; slots reset only after completion |
| Transfer visibility | Upload vertices, indices, storage, then indirect commands when supported | GPU reads expected data with synchronization validation enabled |
| Retirement | Replace/remove while earlier submissions remain outstanding | Old allocations persist until final-use completion; callbacks execute once |
| Shutdown/resize | Graceful exit and resize after several submissions | All frames drained before destruction; no validation lifetime errors |

### 17.2 Milestone 1 tests

CPU: generation slot reuse; dense remove/swap bookkeeping; descriptor input-copy
ownership; dirty transform/bounds propagation; conservative nonuniform-scale
bounds; default transforms; material-section binding; scene removal and queries;
culling against each frustum plane; batch grouping across multiple meshes.

GPU: three distinct meshes/materials in one frame; two instances sharing a mesh;
multiple sections with distinct materials; transform/normal ABI readback; camera
and off-camera shadow caster lists; repeated replacement across frames; empty
scene; resize/minimize/restore; deterministic graceful termination.

Importer gate: parent-after-child node order, repeated mesh occurrences, matrix-only
nodes, unsupported features, malformed buffers, color versus data textures, mip
sampling, and collision-safe cache identity. Animation tests are deferred with
animation rather than claiming static tests validate skinning.

Proposed test sources live under `/home/lk/myprojects/voxelfun/mu_gfx/tests/`:
`render3d_cpu_test.c`, `render3d_gpu_test.c`, `build_geometry_test.c`, and
`build_world_test.c`. Create each with its implementation, not empty files now.
Wire deterministic finite-run GPU tests and CPU test targets into the Makefile.
Actual commands and logs belong beside tests once those targets exist; do not
publish fictitious `make test-render3d` commands today.

### 17.3 Construction and persistence tests

CPU fixtures cover wall joins, minimum dimensions, caps/winding, opening limits,
roof topology, terrain seams, paths, picking, and deterministic variation. Compare
incremental rebuilds against full rebuilds of identical semantic worlds.
History fixtures cover canceled drags, restored deletions, branching after undo,
history eviction, and repeated terrain sample touches. Save tests cover round-trip
equality, truncated files, unknown versions, invalid references, and failed writes.
Failed loads preserve the active world. Worker tests later inject stale completion
order, deletion, and shutdown.

Run CPU tests under ASan/UBSan and finite GPU tests with Vulkan/synchronization
validation. Retain logs. A timeout smoke run does not verify shutdown or images.
Readback tests complement manual visual review, not replace it.

### 17.4 Performance workloads

Record hardware, driver, build mode, resolution, quality, seed, camera, warm-up,
and sample duration. Separate validation correctness runs from release profiling.

| Workload | Purpose |
|---|---|
| Empty/one-mesh scene | Fixed overhead |
| Cottage reference | Visual baseline and edit latency |
| Same-mesh instance sweep | Culling/batching scalability |
| Distinct-material sweep | Pipeline/material grouping cost |
| Dense vegetation close-up | Alpha-mask overdraw and shadow cost |
| Continuous wall drag | Generation, uploads, retirement memory |
| Unchanged world | No regeneration or warmed per-frame heap allocations |
| Terrain brush on chunk boundary | Local dependency extent and normals |
| Long edit session and resize | Lifetime correctness and memory plateau |

Measure CPU preparation/generation, P50/P95/P99 frame time, input-to-publication
latency, upload bytes, visible/culled instances, triangles, batches/draws, GPU
shadow/vegetation time, pending retirement bytes, arena peaks, and allocations.
Report workload counts with timings; drawing less is not automatically an
optimization. Provisional targets: 16.7 ms frame budget, about 2 ms CPU preparation,
2 ms budgeted generation, selected-object preview within 50 ms, and no ordinary
edit-time device idle. Validate on declared reference hardware; CPU/GPU work
overlaps, so these budgets cannot simply be added. Final detail may finish later.

Optimize local rebuilds and duplicate uploads first, then record size/data
movement, batching, foliage cost, LOD, indirect commands, and workers. Track the
peak overlap of old/new resources. API renames alone are not performance wins.

## 18. Decisions and deferred work

| Decision | Reason | Revisit when |
|---|---|---|
| Semantic world and disposable mesh cache | Simple undo/save/local editing | Retain this boundary |
| Application-owned resources | No hidden refcounts or waits | A concrete shared-asset lifecycle requires more |
| Immutable mesh replacement | Predictable in-flight lifetimes | Fixed-topology updates show excess copying |
| CPU preparation/direct instancing | Simple measured baseline | Submission/culling CPU bottleneck |
| Bounded heightfield | Simple editing/picking/persistence | Product scope requires streaming/caves |
| One sun shadow map | Bounded scene | Camera range exposes quality limits |
| Static importer subset | Avoid inherited animation problems | Characters become a requirement |
| Existing RGFW/Nuklear/Slang/mu | Preserve working integration | A demonstrated capability gap |
| Fixed pass sequence | Visible cost and ownership | Multiple real workflows need a graph |

Integration choices to resolve with tested implementation: exact shared mu span
spelling, texture sentinel, backend header boundary, reference hardware, style
palette, shadow resolution, and save-file field schema. Update this document when
resolved. Arbitrary roof merging, path-created arches, water, skeletal characters,
temporal rendering, and multiplayer remain deferred.

## 19. Documentation and validation status

This documentation-only delivery specifies API contracts, the legacy review,
construction/rendering strategy, tutorials, milestones, and test gates. It does
not add headers, compile the legacy renderer, implement a game, run proposed
tests, or establish visual/performance results.

Renderer guides link here with status caveats. Keep API examples labeled proposed
until headers and buildable examples land. Each milestone must add actual paths,
commands, supported features, measured results, and limitations. Remove proposal
labels only for verified capabilities.

Documentation checks cover TOC/sections, local links/anchors, balanced fences,
whitespace, by-value span signatures, and ownership/publication consistency.
Compiling the current demo would not validate unimplemented API signatures;
compile tutorials when implementation exists. Next is Milestone 0, beginning
with build variants and dependency tracking, not a wholesale legacy port.
