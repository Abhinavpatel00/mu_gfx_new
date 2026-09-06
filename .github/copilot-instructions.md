# Copilot Instructions

- This renderer uses a single descriptor set layout and a single pipeline layout for all pipelines (bindless model).
- New buffers are suballocated from larger GPU buffers; pass offsets via push constants and manage ranges with the offset allocator.
I don't use vertex buffers and index buffers. I use offsets to buffer slice via push constant.
- I use a single descriptor set for all resources, and bindless descriptors to access them. This simplifies resource management and allows for more flexible binding.
- i use Pascalcase for types and snake case for functions 
- minimize global state and side effects; prefer explicit parameter passing and return values for clarity and testability.


# Copilot Instructions

## Core renderer model (must follow)

## C code style and change strategy

- Keep changes minimal and local; avoid broad refactors unless requested.
- Match existing naming/style in touched files.

- PascalCase for types and snake_case for everything else 
- Prefer straightforward C over abstraction-heavy patterns.
- No new dependencies unless clearly justified.
- Fix root cause, not superficial patches.
- use cglm for math operations and types (vec3, mat4, etc.) to keep code clean and efficient. 
## Performance priorities

- Optimize for reduced memory bandwidth, fewer texture reads/writes, and fewer barriers.
- Avoid redundant transitions and unnecessary full-resolution passes.
- Prefer half-resolution/intermediate reuse for expensive post effects when quality allows.
- Keep hot-path branches and per-pixel work minimal.
- remember to keep push constant  sixteen byte aligned 
	

## Where to look first

- Shared GPU/CPU structs: `src/slangtypes.h`
- Shaders: `shaders/*.slang`
- refer to docs/rendererdesign.md for docs about structure of code and update when something new is added 
## Validation workflow for agents

- After shader edits: run `./compileslang.sh`.
- After C/Vulkan edits: run project build (`make` / existing build command).
- If changing descriptor bindings or push constants, verify both shader and host-side structs/layouts.

## Do / Don’t quick rules

- Do: use `GlobalData` for view/projection/camera/time.
- Do: use bindless IDs + offsets for resource access.
- Do: keep patches focused and performance-aware.
- Don’t: introduce per-shader custom descriptor layouts.
- Don’t: reintroduce vertex/index binding model for draw submission.
- Don’t: duplicate common descriptor declarations across shaders.


add helpers in helpers.h/.c for common tasks 
