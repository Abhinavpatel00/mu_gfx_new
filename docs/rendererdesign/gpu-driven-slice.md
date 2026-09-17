# GPU-driven 3D vertical slice

Implemented in `/home/lk/myprojects/voxelfun/mu_gfx/renderer3d.c` and
`/home/lk/myprojects/voxelfun/mu_gfx/shaders/scene3d.slang`.

The application renders two colored cubes; a third instance is outside the
camera frustum. One indexed cube mesh and three instance records are published
once. Compute tests bounding spheres against six Vulkan clip planes and writes
three native indexed indirect records. Invisible records have zero instances.
There is no command compaction, GPU count buffer, or CPU visibility readback in
the application. Each reclaimed frame slot owns its own writable command buffer.
A compute-write to indirect-read barrier precedes one multi-draw call.

The camera is fixed, perspective, right-handed, with zero-to-one depth and
LESS testing (clear depth 1). Flat face lighting feeds the existing HDR,
postprocess, SMAA, and Nuklear chain. This replaces the fire pass at the app
call site; its implementation remains available.

## Build and test

Run from `/home/lk/myprojects/voxelfun/mu_gfx`:

```sh
make -j4
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
make test-3d
make test
```

The new scene shaders build automatically; override `SLANGC` if Slang is not at
`/opt/shader-slang-bin/bin/slangc`. Existing application shaders/assets are still
required. Tests need a Vulkan device and an X11 display, even though the 3D
correctness test renders to offscreen images.

The offscreen test checks a CPU-written single indirect draw, a two-draw CPU
reference, byte-identical GPU-driven output, all command fields, and an
all-culled frame following a visible frame. Pixel checks distinguish both cube
colors. Shared C/Slang definitions and C layout assertions cover the indirect
stride, instance layout and push payload. The test includes the implementation
to access private reference helpers without adding test switches to the API.

## Ownership and limits

Create/destroy are paired; wait for final GPU use before destroying the scene.
`renderer3d_record` does not submit, present, allocate, upload instances, or wait.
The application reclaims frame slots through its existing submission timeline.
Scene pipelines are owned directly by the module, not by the backend pipeline
registry, so scene shader hot reload is not implemented in this slice.

This is a correctness milestone, not a general scene API or performance claim.
It has fixed instances/camera, no materials/textures, model import, editing,
LOD, occlusion, shadows, or packed geometry. Geometry and command allocations
use the existing mapped buffer helper; device-local preference is not a
promise of unmapped VRAM on discrete hardware. Benchmarking and staged GPU-only
storage belong to a later measured workload. Working data is 368 bytes of
immutable geometry/instances plus 60 bytes of commands per frame, excluding
allocator granularity and existing render targets. No per-frame heap work is
introduced.
