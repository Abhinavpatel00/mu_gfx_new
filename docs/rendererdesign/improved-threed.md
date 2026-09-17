# Improved 3D Renderer: Performance and Memory Design

Status: **GPU-driven static core implemented; full design and performance gates incomplete**.

Implementation (GPU-only scope selected by the application author):
- Generation-bearing mesh/material/instance handles and swap-remove dense instances.
- Mesh-local half positions/UVs, octahedral SNORM16 normals, 16-byte GPU vertices;
  native 16-bit indices when eligible, otherwise native 32-bit indices.
- Stable geometry suballocations in the backend GPU pool; publication uploads are
  owned until preparation records them. No per-mesh Vulkan buffer or upload wait.
- 64-byte shading records (48-byte affine + vertex address/material/tint), separate
  16-byte conservative spheres, 8-byte candidate references, 4-byte visible IDs.
- Positive-determinant affine transforms including shear; cofactor normal transform.
- Frame-slot resident tables, deduplicated dirty instance ranges and material masks;
  unchanged scene records are not re-uploaded once all replicas are current.
- Membership changes rebuild mesh/material/index-type batches. GPU frustum culling
  atomically appends into candidate-sized batch ranges. Fixed 20-byte indirect
  commands are reset each frame; empty batches retain zero instance counts.
- One multi-draw per index-type group, split only at the device's indirect limit.
  No CPU visibility or direct-rendering fallback. Camera and sun are application data.

The application and shaders build, shared SPIR-V offsets/strides were inspected,
SPIR-V validation passes, and the application completed timed eight-second launches,
including an externally enabled Vulkan/synchronization validation run without a
reported Vulkan error. These launches do not establish image correctness, retirement
stress coverage, shutdown coverage, or performance. No new tests or benchmarks were
run for this implementation; the old internal-layout readback test needs migration.

Limits: bounded scene budgets allocated at creation, one shared geometry pool,
one opaque color-only material pipeline, and one mesh section per mesh ID. Uploads
currently use chunked vkCmdUpdateBuffer (driver-owned command-buffer upload storage),
not a large-upload staging path. CPU sorting occurs only for topology/dirty changes,
but library qsort scratch allocation is implementation-dependent. Meshoptimizer
publication ordering, texture materials, shadows, LOD, occlusion, streaming, and
renderer-managed final-use retirement are not implemented. Mesh range destruction
is immediate and requires the caller to detach instances and complete all recorded
and submitted uses; instance/material snapshots are frame-slot owned. The application
integration is a small procedural example, not a production workload. The staged
baseline comparisons below remain design history, not shipped fallback paths.


This document reviews fukuna's existing 3D path and proposes a replacement for
mu_gfx optimized for predictable CPU work, low memory use, and reduced data
movement. It complements the [renderer reference](README.md) and approved
[Cozy Builder roadmap](../cozy-builder-plan.md). The roadmap remains the authority
for milestone scope and public resource lifetime contracts.

## Contents

1. [Recommendation and scope](#1-recommendation-and-scope)
2. [Source evidence and findings](#2-source-evidence-and-findings)
3. [Architecture and ownership](#3-architecture-and-ownership)
4. [Geometry and native indexing](#4-geometry-and-native-indexing)
5. [Scene and GPU data layouts](#5-scene-and-gpu-data-layouts)
6. [Preparation, batching, and recording](#6-preparation-batching-and-recording)
7. [Persistent storage and uploads](#7-persistent-storage-and-uploads)
8. [Synchronization and retirement](#8-synchronization-and-retirement)
9. [Visibility, LOD, and GPU-driven evolution](#9-visibility-lod-and-gpu-driven-evolution)
10. [Materials, textures, and pixel bandwidth](#10-materials-textures-and-pixel-bandwidth)
11. [Memory and logical traffic budgets](#11-memory-and-logical-traffic-budgets)
12. [Backend integration and proposed interfaces](#12-backend-integration-and-proposed-interfaces)
13. [Migration and milestone gates](#13-migration-and-milestone-gates)
14. [Correctness and performance validation](#14-correctness-and-performance-validation)
15. [Decisions, alternatives, and open questions](#15-decisions-alternatives-and-open-questions)
16. [Delivery status](#16-delivery-status)

## 1. Recommendation and scope

**Keep compact geometry, bindless resources, and explicit preparation. Replace
per-frame allocation and one-command-per-instance rendering with safe reusable
storage and native indexed instancing. Add GPU visibility only when it beats the
CPU baseline on declared workloads.**

Priority order:

1. Correct ownership and synchronization with multiple frames in flight.
2. Avoid regenerating, copying, or uploading unchanged data.
3. Restore indexed vertex reuse and instance repeated geometry.
4. Keep hot data contiguous; separate visibility from shading data.
5. Reduce texture, shadow, and full-screen traffic as well as geometry traffic.
6. Add elaborate GPU work only after measuring a bottleneck.

GPU-driven execution is not itself a performance guarantee. Dispatches, counters,
compaction, indirect records, and barriers cost time and memory. A compact
CPU-prepared renderer is the first benchmark competitor, not a disposable demo.

### Compatibility with the approved roadmap

The roadmap starts with float position/normal/UV vertices, float matrices,
32-bit input indices, CPU visibility, and direct instancing. Preserve that
correctness baseline. Native indexing is compatible with direct instancing.

A tested 16-byte GPU vertex format, 48-byte affine transforms, persistent dirty
scene uploads, and optional 16-bit GPU indices are optimization stages. They do
not silently change the initial public mesh representation or roadmap order.
CPU input can remain convenient while import/publication packs the GPU format.

The initial application is a mostly static cozy builder: repeated props,
editable architectural meshes, terrain chunks, and vegetation. Skeletal
animation, mesh shaders, general streaming, and temporal occlusion are not
requirements of the first static slice.

## 2. Source evidence and findings

### Review provenance

The principal reviewed snapshot is:

- `/home/lk/myprojects/voxelfun/mu_gfx/fukuna_engine/main.c`
- `/home/lk/myprojects/voxelfun/mu_gfx/fukuna_engine/helpers.h`
- `/home/lk/myprojects/voxelfun/mu_gfx/fukuna_engine/renderer_gpu_driven.c`
- `/home/lk/myprojects/voxelfun/mu_gfx/fukuna_engine/shaders/gltf_minimal.slang`
- `/home/lk/myprojects/voxelfun/mu_gfx/fukuna_engine/shaders/compute_skinning.slang`

At review time the main source above and
`/home/lk/myprojects/voxelfun/mu_gfx/threedrenderer.c` had identical SHA-256:
`5079373ed173e16161ac613db548a232c69a4fd83f1f24eeb0a950eec874b1a6`.
The sibling `/home/lk/myprojects/voxelfun/fukuna/main.c` has local differences;
a third checkout exists at `/home/lk/myprojects/voxelfun/fukuna_engine`.
These findings describe the identified snapshot, not every checkout. Line
numbers below are navigation aids for that snapshot, not stable API references.

The backend reference is
`/home/lk/myprojects/voxelfun/mu_gfx/docs/rendererdesign/README.md`.
Its historical GLFW/ImGui terminology is not the current integration target;
mu_gfx uses RGFW/Nuklear. Its shared CPU-pool description is not proof of safe
new frame-data ownership.

### Findings

Evidence in the table uses the absolute source paths listed above.

| Priority | Evidence in reviewed snapshot | Consequence and action |
|---|---|---|
| P0 | Main source, lines 2515-2577: release GPU frame slices, allocate replacements, submit one-time upload | Replace with reusable slot-owned storage. Establish final-use retirement before removing serialization. |
| P0 | Upload helper, lines 227-234: calls `vkQueueWaitIdle` | Ordinary frame uploads serialize the queue. Record copies and dependencies in frame work instead. |
| P1 | Main source, lines 2703-2804: CPU builds `VkDrawIndirectCommand`, `instanceCount = 1`, then `vkCmdDrawIndirect` | Indirect drawing is not GPU visibility or batching. Build indexed instanced batches across models. |
| P1 | Vertex shader, lines 107-120: instance -> draw -> index -> packed vertex | Shader-side indexing of a non-indexed draw does not provide native indexed vertex reuse. Bind an index buffer. |
| P1 | Main source, lines 2462-2488: rebuild a temporary GPU instance array | Additional allocation and copy. Write the final upload layout directly into reclaimed frame storage. |
| P1 | Main source, lines 2703-2742: command generation takes one model and selects LOD 0 | Resolve mesh sections and resource bindings in a general multi-model preparation path. |
| P2 | Main source, lines 2451-2460: 96-byte GPU instance contains model, bounds, metadata | Separate culling bounds from shading data; evaluate compact transforms after baseline tests. |
| P2 | Main source, lines 323-373: 32-byte draw, eight inline LOD records per mesh, broad material layout | Remove unused hot fields; use actual LOD ranges and shading-specific material data. |
| P2 | Separate GPU-driven module, lines 57-95: one-shot uploads and placeholder record function | Do not treat this module as completed GPU-driven execution. |

The later queue-idle wait does not make a preceding free/reallocation safe by
itself. Conversely, source inspection alone does not prove a reproduced GPU
race. Retirement and synchronization need a validation test, not inference
from a demo appearing to render correctly.

The roadmap's legacy audit also covers node transforms, weak asset identity,
shared animation state, texture color space, and cache identity. Preserve those
correctness fixes rather than copying the importer wholesale.

### What is worth retaining

- A compact packed static-vertex direction.
- Bindless texture/sampler lookup instead of per-draw descriptor allocation.
- Explicit asset, scene, and frame concepts.
- Buffer suballocation and contiguous upload opportunities.
- Existing mu primitives, VMA, cglm, Slang, and meshoptimizer integration.

## 3. Architecture and ownership

Keep the renderer separate from construction semantics and GPU submission:

```text
BuildWorld / imported asset
    -> owned CPU geometry and validated material bindings
    -> RenderResources: immutable mesh versions, materials, textures
    -> Scene3D: handles, transforms, flags, dirty state
    -> renderer3d_prepare: resolve, cull, group, stage, dependencies
    -> PreparedScene: frame-owned resolved batches
    -> renderer3d_record: fixed passes and draws
    -> application submission / presentation / timeline retirement
```

These names describe proposed interfaces from the roadmap, not installed APIs.

| Owner | Owns | Lifetime |
|---|---|---|
| Application/resource layer | Mesh allocations, texture/material resources, public handles | Explicit creation to safe destruction |
| Scene | Dense live instances, handle-to-dense mapping, dirty state | Scene membership; does not retain resources |
| Renderer3D | Pipelines, render targets, reusable preparation storage | Renderer lifetime, with safe resize |
| Frame slot | Staging, prepared lists, transient commands, per-view constants | Reclaimed only after covering submission completes |
| Retirement mechanism | Destruction callback payloads or retired allocation records | Until final-use completion; drained before owner shutdown |

Use generation-bearing IDs at public boundaries. Resolve them when instances
are created/changed or prepared; keep execution packets free of asset lookups.
Dense indices are internal locations, not persistent IDs. Swap-removal must
update mappings and dirty GPU locations; avoid long-lived pointers into movable
tables. An old dense index must not silently become a different object's identity.

Use reusable mu dense arrays, sparse mappings, arenas, or pools where suitable.
If a required reusable primitive is missing, add it to
`/home/lk/myprojects/voxelfun/mu_gfx/external/mu`, not a renderer-local framework.
Avoid a separate heap allocation per instance, mesh section, or draw packet.

Keep hot runtime data distinct from paths, importer metadata, animation channels,
editor names, and procedural history. A culling loop should not chase an asset
object to find its bounds. A draw loop should not chase one to find its buffer.

### Resource invariants

Validate malformed external content at import and resource initialization.
Document programmer preconditions and assert wrapper-crashing misuse in debug
builds. Do not duplicate Vulkan validation or add release-path validation tables.
Resolve required bindings before recording; missing external assets fail early.
Do not add recoverable out-of-memory machinery to the frame loop.

Scene membership does not extend resource lifetime. A resource must outlive
attached uses, prepared packets, recorded commands, and executing submissions.
Immediate destruction remains the public policy; optional retirement is explicit.

## 4. Geometry and native indexing

### Native indexed instancing

The reviewed shader receives a sequential vertex ID from a non-indexed draw,
loads an index, and then loads the vertex. Repeated index values can benefit
from memory caches but do not express indexed vertex reuse to the draw frontend.

The replacement must change the whole contract together:

1. Allocate index storage with `VK_BUFFER_USAGE_INDEX_BUFFER_BIT` in addition
   to required transfer usage. Do not assume a storage-buffer pool supports it.
2. Bind the correct buffer slice and index type.
3. Use `vkCmdDrawIndexedIndirect option.
4. Supply `firstIndex` in index elements relative to the bound byte offset.
5. Apply the signed base vertex exactly once. Choose mesh-local indices plus a
   pooled vertex base, or a mesh-specific vertex pointer with zero base vertex.
6. Remove the shader-side index-buffer load. Fetch the packed vertex using the
   effective indexed vertex number under the tested Slang/SPIR-V convention.
7. Verify instance-base semantics independently; do not add `firstInstance`
   twice. Inspect generated built-ins and render multiple batches with nonzero
   base vertex and first instance.

Buffer-device-address vertex fetching can coexist with native indexing. Packed
fixed-function attributes are another option, not a prerequisite. Compare both
only after identical geometry and shader outputs are established.

An indexed indirect command is 20 bytes, compared with 16 bytes for a non-indexed
command. It contains index count, instance count, first index, signed base vertex,
and first instance. Preserve Vulkan's exact field offsets and stride contract.
Index type and buffer binding are batch state, not command fields. Multi-draw
must respect enabled device features and limits; retain a compatible direct path.

### Packed vertex candidate

Keep the roadmap's float baseline first. A later candidate retains fukuna's
16-byte budget without freezing every existing packing choice:

| Byte range | Candidate payload | Qualification |
|---|---|---|
| 0-7 | Three half position components plus one 16-bit auxiliary field | Local-space precision must pass visual tests |
| 8-11 | Packed normal, for example two signed normalized octahedral components | Define exact encode/decode and error bounds |
| 12-15 | Two half UV components | Validate tiled UV range and precision |

This is a proposed layout, not the declaration of a shared implemented type.
A tangent basis does not automatically fit in the spare 16 bits at acceptable
quality. Begin with materials that do not require stored tangents; choose a
separate format or tested reconstruction before enabling tangent-space normals.
Do not silently drop handedness or import attributes needed by a material.

Half positions have magnitude-dependent precision. Keep geometry mesh-local,
not world-space; test tiny trims, large walls, seams, and distant placement.
An alternative is mesh-bounds-relative UNORM16 positions with scale/bias, which
adds decode metadata and seam constraints. Keep float fallback or reject content
outside the documented packed-format quality envelope at import. Packing is
not a win if architectural seams become visible.

### Indices, optimization, and allocation

Public input remains 32-bit initially. Later narrow a mesh's GPU indices to
16-bit when every local index fits the chosen topology/restart contract. Do not
split every mesh solely to force 16-bit indices: duplicated vertices and extra
batches may outweigh index savings. Group by index type.

Use the existing meshoptimizer dependency during import or final procedural
publication: optimize cache order, evaluate overdraw ordering, then optimize
vertex-fetch order with the appropriate remap. Preserve section/material
boundaries and all vertex attributes. Deduplication must include discontinuous
normals/UVs, not just positions. Measure generation cost during interactive edits;
coarse previews may defer expensive final optimization.

Use persistent geometry pages/suballocations rather than a buffer allocation per
draw. Store resolved page/buffer, vertex range, index range/type, and section
metadata at preparation boundaries. Avoid one global arena that must be copied
in full whenever it grows. Page size and free-space policy need fragmentation
measurements. Relocation publishes replacement bindings and retires old storage;
it cannot invalidate already-recorded device addresses.

Store only actual LOD ranges rather than eight records in every hot mesh record.
Keep optional importer and LOD-generation data cold. Release intermediate CPU
geometry once publication and CPU consumers no longer need it; picking should
use deliberately owned collision data, not an accidental permanent import copy.

## 5. Scene and GPU data layouts

### Access patterns before compression

The reviewed GPU record is 96 bytes: a 64-byte model matrix, 16-byte sphere,
and 16-byte metadata. Its source declaration is evidence of logical layout;
actual shader accesses and external memory transactions require measurement.

Candidate optimized storage:

| Stream | Logical bytes per element | Consumer |
|---|---:|---|
| Affine transform | 48 | Vertex shading |
| Shading metadata budget | 16 | Vertex/material selection as needed |
| World sphere | 16 | Visibility |
| Visible index | 4 per visible occurrence | Vertex instance selection |
| Persistent public ID/mapping | Account separately | CPU boundary, optional picking |

The first three total **80 bytes**, not 64. An optional normal matrix, previous
transform, or picking payload must be added to the budget rather than hidden
inside it. Bounds need not reside on the GPU at all while culling is CPU-only.

A compact 64-byte shading record can consist of three explicit float4 affine
rows plus four uint fields. Define those fields only after material/tint/flags
requirements settle. This budget is not permission to introduce unused fields.
A material uniform per batch need not be repeated per instance.

Three explicit rows avoid assuming a host compiler's matrix matches Slang
packing. Define multiplication as three row dot products with `(p, 1)` and test
against the cglm column-major baseline. Verify size, offset, alignment, array
stride, and shader readback for every shared layout.

### Normal transformation is part of the cost

The roadmap allows positive nonuniform instance scales. Applying the position
matrix directly to normals is wrong under nonuniform scale. Options include:

- Baseline: prepare an inverse-transpose normal transform and count its bytes.
- For a strict orthogonal rotation-times-scale transform: derive the equivalent
  normal transform from its basis and squared axis lengths, then normalize.
  This trades bandwidth for ALU and is not valid for arbitrary shear.
- Bake imported static hierarchy transforms into geometry, with correct normals
  and winding, where doing so preserves intended instancing and memory use.

Hierarchy composition can create shear even from local TRS inputs. Do not apply
the orthogonal shortcut to arbitrary composed affine matrices. Keep a supported
fallback or reject unsupported import transforms explicitly. Smaller records
must never quietly weaken the approved transform contract.

Quaternion/translation/scale compression is deferred. It can reduce storage but
adds reconstruction and constraints. Compare its complete vertex cost against
48-byte affine storage; do not optimize record size in isolation.

### CPU locality and change tracking

Keep bounds and frequently tested visibility flags contiguous. AoS spheres are
a simple starting point; SoA components may help SIMD culling but require actual
CPU measurements. Transform and material data belong outside the bounds scan.

Maintain a deduplicated dirty list for changed transforms/bindings. Update world
bounds once per change rather than reconstructing model matrices on every draw.
Use conservative bounds under supported transforms. A max-axis scale sphere
expansion assumes orthogonal axes; for general affine transforms use a
conservative norm bound or transformed AABB method.

Avoid redundant mirrors without a consumer. Track CPU truth, GPU resident
versions, pending changes, and frame snapshots explicitly. One CPU record may
serve several mesh sections; do not duplicate its transform per section unless
a measured contiguous-packet path justifies the memory tradeoff.

## 6. Preparation, batching, and recording

### Preparation sequence

1. Reclaim the selected frame slot using its completion value.
2. Apply completed scene changes and publish new mesh IDs before preparation.
3. Resolve changed handles, update transforms, bounds, and material bindings.
4. Build camera visibility and a separate shadow-caster set.
5. Select LOD 0 initially; later use projected size with hysteresis.
6. Build compact sortable references, not copies of full transforms.
7. Group compatible mesh-section instances and write contiguous batch ranges.
8. Write final frame data directly into owned staging or reusable CPU scratch.
9. Record pending uploads and producer-to-consumer dependencies.
10. Freeze prepared packets until recording/submission is complete.

Execution binds prepared pipeline/index state, emits draw root data, and issues
indexed instanced draws. It does not allocate, resolve asset handles, mutate
scene state, submit queues, or present the swapchain.

### Batch compatibility

A batch key needs the state that cannot vary within the selected draw contract:

- Pass and pipeline variant, including alpha mode, cull mode, and vertex format.
- Bound index buffer/page and index type.
- Mesh section and selected LOD/index range.
- Vertex source/base convention.
- Material compatibility; initially group by material for predictable access.

Bindless textures remove descriptor rebinding, not pipeline or geometry
compatibility requirements. One indexed instanced draw repeats one index range;
unrelated unique wall meshes do not become one instanced draw merely because
they share a material. They can later share an indirect multi-draw group.

For opaque geometry, balance coarse front-to-back depth buckets with state and
mesh grouping. Strict global depth sorting can destroy instancing. Transparent
blending needs its own order policy; do not reuse opaque sorting blindly.
Masked foliage is not automatically transparent blending.

### Visible IDs versus packed visible records

Baseline preparation can gather contiguous full instance records for simple
shader indexing. Later persistent scene storage can gather only 32-bit visible
indices and let the shader load resident records. This reduces upload size but
adds indirection and may scatter reads after sorting.

Benchmark both on repeated props, unique geometry, and multi-pass views. Keep
persistent indices coherent where practical, without turning every edit into a
full reorder. Count visible entries per pass; camera and shadow lists can
contain the same instance independently.

## 7. Persistent storage and uploads

### Three storage classes

| Class | Examples | Reuse rule |
|---|---|---|
| Persistent | Geometry, material tables, optional resident scene records | Update only under explicit synchronization/version policy |
| Frame-slot | Staging, visible lists, commands, constants, CPU preparation arena | Wait for that slot's final GPU use |
| Retired | Replaced mesh ranges, grown buffers, old targets | Reclaim after every recorded/submitted use is resolved |

Persistent capacity is not the same as immutable content. Start with safe
frame-slot snapshots and no warmed per-frame allocation. Add dirty resident
updates only after choosing one of these explicit strategies:

- Slot-specific resident replicas: apply all changes since each slot's version
  when that slot is reclaimed. Costs multiple copies of resident state; avoids
  writing a replica still used by the GPU. Track missed changes across slots,
  not merely the current frame's dirty list.
- One resident table on a serialized queue: device copies occur after prior
  readers and before subsequent readers, with explicit dependencies. This can
  restrict overlap. Never host-write mapped destination records still in use.
- Copy-on-write/versioned ranges: retain old ranges until final use. Saves full
  replication but adds mapping and retirement overhead; adopt only when needed.

The document does not assume that persistent dirty updates are automatically
safe or cheaper. Benchmark total memory, update bookkeeping, and overlap.

### Single-write upload preparation

Write the final GPU representation once into owned upload storage where
practical. Avoid CPU scene -> temporary mirror -> second temporary -> staging.
A discrete-GPU copy still reads staging and writes device-local memory; calling
this single-write refers to CPU preparation, not zero physical data movement.

Mesh creation must preserve the roadmap contract: caller data may be changed
immediately after creation. Copy or pack it into owned pending-upload storage.
A future direct-fill interface would need a separate explicit ownership contract;
it cannot borrow caller pointers until an unspecified later frame.

Merge adjacent dirty upload ranges when saved command overhead exceeds harmless
extra bytes. Track both useful and padded/copied bytes. Avoid many tiny copies,
but do not turn one changed object into a whole-scene upload. Use mapped-memory
flush/invalidate rules from the actual allocation properties.

Allocate initial capacities from expected workloads, grow geometrically or in
pages outside the steady-state path, and track high-water marks. Avoid huge
static arrays and large stack temporaries. Keep growth peaks and fragmentation
visible; release excess capacity only at an explicit safe maintenance boundary.

On unified-memory hardware, benchmark mapped GPU-readable frame data against
staging plus device-local copies. On discrete hardware, repeated shader reads
from host memory can be expensive. Memory-type choice is a measured backend
policy, not a universal claim that one path is optimal everywhere.

## 8. Synchronization and retirement

### Mesh publication

Preserve the roadmap's five distinct steps:

1. Generate replacement geometry into owned CPU storage.
2. Create replacement resources and own their pending upload data.
3. Publish replacement IDs to future scene preparation before recording.
4. During prepare, record copies and read dependencies before consuming draws.
5. Retire old storage after its final submitted use completes.

Publishing an ID is not the same event as making uploaded bytes GPU-visible.
No resource mutation is allowed between preparation and recording. Prepared
packets remain slot-owned and valid until slot reclamation; they may not retain
mutable caller arrays. Discard stale worker results before publication using
object generation/revision, if workers are introduced later.

### Dependency checklist

| Producer | Consumer | Required dependency scope |
|---|---|---|
| Transfer copy | Native index fetch | Transfer write -> index-input/index read |
| Transfer copy | Fixed-function vertex input | Transfer write -> vertex-attribute-input/attribute read |
| Transfer copy | BDA/storage vertex fetch | Transfer write -> vertex shader/storage read |
| Transfer copy | Material sampling metadata | Transfer write -> actual shader stages/storage read |
| Transfer or compute command generation | Indirect draw | Producer write -> draw-indirect/indirect-command read |
| Compute culling | Visible-instance shader lookup | Compute storage write -> consuming shader/storage read |
| Compute skinning, if added | Vertex fetch | Compute storage write -> vertex consumer read |
| Shadow depth attachment | Shadow sampling | Depth write -> shader sampled read, matching image layout |

Use the actual Vulkan synchronization2 stages/accesses supported by the backend;
these are dependency descriptions, not a copy-paste barrier helper. Image layout
tracking alone does not establish all memory dependencies. Include write-after-
read ordering when updating persistent tables between frames.

Start on one graphics queue for a simple ownership model. A transfer/compute
queue later requires semaphore synchronization and queue-family ownership
handling where applicable. Do not introduce async queues merely because they
exist; overlap must compensate for extra coordination and contention.

### Final-use accounting

A retirement value must cover the submission containing final recorded uses,
not just the latest submission that existed when replacement was requested.
Handle recorded-but-unsubmitted work explicitly: submit it with tracked lifetime
or cancel it before releasing its resources. A suballocation free is reuse of
memory and obeys the same rule as destroying a buffer.

Buffer growth must preserve old device addresses used by pending work. Texture
replacement also retires image/view and bindless descriptor-slot reuse. Updating
a descriptor does not make an in-flight old texture reference safe automatically.

At shutdown: stop producers, resolve/cancel unsubmitted recordings, wait idle
and drain submitted frames, drain every deletion queue, destroy application and
renderer resources, then destroy the device. Deletion callbacks cannot reference
owners already destroyed. Normal editing must not call device/queue idle.

## 9. Visibility, LOD, and GPU-driven evolution

### CPU baseline

Scan dense conservative bounds, reject invisible instances, and build camera
and shadow lists separately. Off-camera objects can cast visible shadows.
Start with simple frustum tests; add coarse spatial chunks only when a full
scan is measured to be costly. Chunk metadata, maintenance, and false positives
must be included in the comparison.

LOD should reduce actual projected detail rather than only command count.
Introduce projected-size thresholds with hysteresis, material/section-compatible
LOD ranges, and separate shadow quality rules. Terrain and procedural buildings
need crack/seam-safe transitions. Do not claim working LOD from preallocated
metadata or a command builder always choosing entry zero.

### GPU progression

1. CPU visibility + direct indexed instancing: correctness/performance baseline.
2. CPU visibility + indexed indirect batches: isolate submission overhead.
3. GPU frustum/LOD selection + visible lists + indexed indirect execution.
4. Optional hierarchical occlusion only if hidden geometry is expensive enough.

At stage 3 compare two command policies:

- Fixed command slots per batch with zero instance count for empty batches:
  simple addressing, but command processor still encounters empty records.
- Compacted nonempty commands with a GPU count: less command traffic, but requires
  compaction, count support, synchronization, and worst-case storage management.

A simple first GPU scheme reserves each batch enough visible-ID capacity for its
candidate count, resets counters, appends surviving IDs, and finalizes instance
counts. This avoids a global variable-size allocator but reserves worst-case
space and can create hot atomic counters. Compare prefix-sum/scatter compaction
only if that cost becomes important. Record all scratch and pass duplication.
Never silently overflow a visibility list; preparation establishes capacity.

Occlusion adds depth-pyramid storage/builds, conservative tests, and temporal
correctness requirements. With reversed Z, choose reduction and comparison from
the actual depth convention. Camera cuts, newly published meshes, and moving
occluders must not disappear due to stale history. Meshlet/mesh-shader conversion
is deferred until native indexed execution is demonstrably insufficient.

## 10. Materials, textures, and pixel bandwidth

Geometry submission may cease to dominate once instancing works. A cozy scene's
foliage, sun shadows, and full-screen post-processing can consume more bandwidth
than its mesh records. Measure pass-level GPU time and attachment traffic.

### Initial shading policy

Use forward opaque/masked rendering with the roadmap's bounded lighting model,
one sun shadow map, fog, and existing composition path. Avoid a large G-buffer
without a lighting workload that benefits from it. Integrate with existing HDR,
tonemapping, and antialiasing rather than duplicating them inside Renderer3D.

Keep material data suited to the implemented shader. The reviewed 80-byte
material includes multiple textures and factors although the minimal shader
uses only base color. A base-color-only pipeline needs less data; a later lit
pipeline genuinely needs more. Do not claim a smaller universal material layout
before defining the supported shading features and texture encoding.

Separate rarely used material extensions from common fields only when the saved
traffic exceeds extra indirection. Prefer material-coherent batches. If bindless
texture indices vary nonuniformly across invocations, use the appropriate Slang
nonuniform indexing mechanism and enabled descriptor-indexing features; verify
emitted SPIR-V rather than assuming bindless access is always dynamically uniform.

### Textures and render targets

- Use sRGB sampling for authored color textures where appropriate; normal,
  roughness, metallic, and other data textures remain linear.
- Generate/use mips. Select supported block compression by quality and device
  capability; quantify resident bytes including all mip levels.
- Share repeated textures and samplers with collision-safe asset identity.
  Hash equality alone is not resource equality.
- Track texture dimensions and anisotropy against visible detail, not source
  asset defaults. Avoid loading unused material textures.
- Store an attachment only when a later operation needs it. Depth used by later
  sampling or testing cannot simply be discarded to save bandwidth.
- Count every HDR/LDR intermediate, depth/shadow target, and resize overlap.

A depth prepass is conditional. It repeats geometry work and depth traffic but
can save expensive overdraw. Compare no prepass, selected opaque occluders, and
full prepass on the same scene. Alpha masking can require texture fetches in
both passes. A separate position-only vertex stream for depth/shadow rendering
also needs measurement: it may reduce fetches but add geometry storage and binds.

### Vegetation and optional animation

Use density/LOD and shadow-distance policies to reduce foliage cost. Track alpha
coverage, triangle size, fragment invocations where available, and two-sided
shading. A low draw count does not imply low foliage cost.

If skeletal animation becomes necessary, share immutable input geometry but give
independently animated instances separate pose/output ownership. Cull before
skinning where correct, considering the union of camera and shadow requirements.
Compare vertex skinning with compute skinning across all consuming passes; compute
adds output writes and later reads but can amortize deformation across passes.
The reviewed compute shader repacks moved positions while retaining input normal
and tangent data. Correct deformation of shading attributes is a separate gate.
Its stale 64-vertex comment is not evidence of the actual limit: the shader uses
`groupID.y` chunks; dispatch coverage must be checked in the caller.

## 11. Memory and logical traffic budgets

All MB below are decimal (1,000,000 bytes). MiB means 1,048,576 bytes. These
examples exclude allocator padding unless stated. They are arithmetic models,
not measured DRAM traffic, cache misses, shader invocations, or speedups.

### Geometry reference traffic

Let `R` be index references for one instance/pass, `U` distinct referenced
vertices, `V` vertex bytes, and `I` index bytes. Let `E` be actual indexed vertex
executions expressed as equivalent full-record reads in this simplified model.

```text
Shader-side indexing model: R * (I + V)
Native indexing model:      R * I + E * V
Ideal indexed reuse:       E = U
Resident geometry payload: stored_vertices * V + stored_indices * I
```

Actual shaders may load only some fields; hardware caches transactions rather
than whole logical records. Indexed reuse depends on ordering and hardware;
`E = U` is an ideal reference point, not a guarantee. Native indexing does not
promise reuse across instances, draws, or passes.

Example: `R = 3,000,000`, `U = 500,000`, `V = 16`:

| Model | Arithmetic | Logical bytes |
|---|---|---:|
| Shader-side indexing, 32-bit | `3,000,000 * (4 + 16)` | 60 MB |
| Native indexing, 32-bit, ideal reuse | `3,000,000 * 4 + 500,000 * 16` | 20 MB |
| Native indexing, mesh-local 16-bit, ideal reuse | `3,000,000 * 2 + 500,000 * 16` | 14 MB |

The 16-bit example assumes multiple meshes whose local indices each fit, not a
single mesh addressing 500,000 vertices with 16-bit indices. Mesh-section and
instance repetitions must be counted in execution estimates. These figures omit
instance/material reads, shader outputs, rasterization, textures, and attachments.

For 500,000 stored vertices and 3,000,000 stored indices, a 32-byte float vertex
payload plus 32-bit indices occupies 28 MB; 16-byte vertices plus 32-bit indices
occupy 20 MB; packed vertices plus eligible 16-bit indices occupy 14 MB. Indexing
alone changes execution reuse, not the number of resident vertices.

### Instance and command examples

For 100,000 resident instances and one view with 25,000 visible occurrences:

| Payload | Logical size |
|---|---:|
| 96-byte records for every resident instance | 9.6 MB |
| Candidate 64-byte shading records | 6.4 MB |
| Separate 16-byte bounds for every resident instance | 1.6 MB |
| 32-bit visible list for this view | 0.1 MB |
| Gathered 64-byte records for this view | 1.6 MB |

Three full 96-byte resident replicas cost 28.8 MB. Three candidate 64-byte
shading replicas cost 19.2 MB, plus whichever CPU/GPU bounds copies are required.
A single 80-byte resident representation is 8 MB, but cannot be compared to a
safe three-slot snapshot without also accounting for synchronization/versioning.

At 1,000 changed shading records per frame, useful 64-byte dirty payload is
64,000 bytes versus 6.4 MB for a full shading-table upload. Add visible lists,
changed bounds, materials, geometry, alignment, and copies caused by coalescing.
An unchanged world can have zero scene-table uploads yet still rebuild/upload
visibility when the camera moves. It does not imply zero frame traffic.

For 10,000 identical compatible visible instances:

- One non-indexed indirect command per instance: 160,000 command bytes.
- One indexed indirect command per instance: 200,000 command bytes.
- One indexed instanced indirect command: 20 command bytes, plus 40,000 bytes
  if using a visible-ID list, plus resident/gathered transform data.
- Direct indexed instancing needs no GPU indirect buffer, although host command
  recording and driver command-stream storage still have costs.

Instancing reduces commands and duplicate geometry storage, not the number of
geometric instances shaded. A mesh rendered 10,000 times still incurs per-instance
vertex and fragment work. Material and pass splits can increase batch count.

### Framebuffer and total peak accounting

At 1920 x 1080, one 8-byte-per-pixel HDR attachment is 16.5888 MB and one
4-byte-per-pixel attachment is 8.2944 MB. An uncompressed logical full-image read
plus write of the HDR attachment is 33.1776 MB per pass. Actual physical traffic
can differ due to compression, caches, tiles, and stores. This illustrates why
post-processing and overdraw deserve attention alongside instance compression.

```text
Peak CPU memory = scene truth + handle mappings + import/generation working set
                + pending publication + frame scratch + retained collision data
Peak GPU memory = live geometry/textures/materials + resident scene versions
                + frame-slot device scratch + render targets + retired resources
Total allocation budget also includes host staging, allocator slack, alignment,
fragmentation, descriptor/query storage, and temporary old/new growth overlap.
```

Count allocations once by actual memory placement; mapped staging on a unified
heap must not be counted twice as separate physical CPU and GPU storage. Report
both logical payload and allocated bytes. During wall dragging, stale results
and retirement backlog need bounded publication budgets, not unlimited queues.

## 12. Backend integration and proposed interfaces

Use the roadmap's resource/scene API rather than exposing fukuna's `ModelAsset`
as the engine abstraction. All names in this section are **proposed contracts**.

| Boundary | Contract |
|---|---|
| Mesh creation/destruction | Adjacent public functions; copy input at creation; immediate destruction after final use |
| Material creation/destruction | Explicit application ownership; no hidden retention by scene membership |
| Instance creation/update/removal | Generation-bearing IDs, batched changes, resolved valid bindings |
| `renderer3d_prepare` | Reclaimed frame slot; updates, culling, grouping, pending copies and dependencies |
| `renderer3d_record` | Prepared immutable packets; fixed passes; no allocation, lookup, submit, or present |
| Statistics query | CPU counters plus completed asynchronous GPU measurements |

Use custom mu spans by value for array data. `Span`, `ByteSpan`, and `GpuRange`
parameters are passed by value; large descriptors by reference. Do not introduce
C++ standard-library containers or a competing renderer-local span abstraction.
Keep shared GPU layouts in one authoritative CPU/Slang definition where possible,
with compile-time checks and shader tests rather than duplicated assumptions.

Add the narrow native-indexed backend draw helper next to existing command APIs
in `/home/lk/myprojects/voxelfun/mu_gfx/main.c` or its extracted public boundary.
It must express index binding/type, index count, first index, base vertex,
instance count, first instance, and by-value root payload without per-call heap
work. Preserve the existing push-constant layout limit and bindless set model;
a 256-byte range does not mean every draw must upload 256 useful bytes.

Fix depth-only graphics-pass classification before the shadow path relies on
`begin_pass`. Audit index-buffer usage and upload barriers explicitly. Keep
backend initialization checks early, use the Vulkan validation layer for Vulkan
parameter correctness, and do not add shadow validation state to the wrapper.
The renderer reference is guidance; confirm exact exported APIs in source when
implementing because the reference is not fully synchronized.

## 13. Migration and milestone gates

These stages refine the existing roadmap; they do not replace its milestones.

### A. Ownership and correctness baseline

- Establish completion-based frame-slot reuse and owned pending uploads.
- Integrate ordinary copies into consuming submissions, without upload idle waits.
- Implement multi-model CPU preparation and native direct indexed instancing.
- Keep float vertices/matrices and 32-bit input indices for the first baseline.
- Verify index-buffer usage, base offsets, normal transforms, and depth-only passes.
- Make debug/release configurations reproducible before comparing performance.

Gate: multiple models, materials, transforms, and camera/shadow passes render
correctly. Replacement, resize, and shutdown pass lifetime tests with multiple
frames in flight. No normal-frame allocation after capacities warm up, except
explicit resource publication/growth, which is separately counted.

### B. Measured data-movement reductions

Change one variable at a time:

1. Reusable CPU preparation and direct final-layout writes.
2. Meshoptimizer ordering and measured material/mesh grouping.
3. Packed 16-byte vertex candidate versus the float reference.
4. Eligible 16-bit GPU indices without changing the initial input contract.
5. Affine transforms with a correct, explicitly costed normal policy.
6. Separate bounds and persistent scene updates using one chosen safe strategy.
7. Visible-ID lists versus gathered visible records.

Gate: image quality and correctness remain acceptable, useful upload bytes and
allocated peaks are reported, and improvements exceed timing noise on declared
hardware. A smaller record that slows the target scene is not automatically
accepted. Keep independent toggles for meaningful A/B experiments, not a large
permanent matrix of unsupported renderer variants.

### C. Conditional execution upgrades

- Compare CPU-generated indexed indirect batches against direct instancing.
- Add GPU visibility/LOD only if CPU preparation/submission is a bottleneck.
- Compare fixed command slots with compacted commands including all scratch.
- Add occlusion only when saved rendering exceeds pyramid/test/coordination cost.
- Evaluate async queues only if measurable overlap outweighs synchronization.

Gate: each upgrade improves the target workload without unacceptable memory,
latency, portability, or correctness regressions. Retain a simple baseline for
comparison. Mesh shaders, general streaming, and animation are separate projects,
not prerequisites for delivering the static renderer.

<!-- NEXT -->
