# Engineering Doctrine

Optimize for simple, data-oriented, predictable code.

## Design Priorities

1. Data locality and low memory bandwidth
2. Simple ownership and lifetimes
3. Contiguous storage and predictable access
4. Minimal pointer chasing and branching
5. Compile-time knowledge
6. Measurable performance
7. Simplicity over abstraction
8. keep gpu and cpu cache locality in mind 
## Data-Oriented Design

Data layout is a primary design decision. Design around access patterns, iteration, and lifetime.

Prefer IDs/handles over long-lived pointers:

```c
TextureId  texture;
MeshId     mesh;
MaterialId material;
```

Use dense tables, sparse sets, fixed-capacity arrays, pools, and arenas where appropriate.

Prefer:

```c
Arena    arena;
Texture  textures[MAX_TEXTURES];
Entity   entities[MAX_ENTITIES];
```

Minimize struct size. Keep frequently accessed data together and separate hot/cold data when useful.

When `mylib` lacks a required data-oriented primitive, add it to `external/mu` rather than introducing an ad-hoc solution.

## Memory

Prefer static, fixed-capacity, or arena allocation when appropriate.

Do not blindly use static storage. Excessive static allocation wastes memory and creates unnecessary limits.

Treat stack space as limited. Avoid large local allocations, oversized temporary objects, and unnecessary stack-heavy call chains.

Avoid hidden allocations, especially in hot paths.

## Performance

Prioritize:

* Reduced memory bandwidth
* Cache locality
* Contiguous access
* Reduced pointer chasing
* Batch processing
* Predictable control flow
* Fewer branches
* Compile-time computation

Optimize data movement before instruction count.

Prefer predictable memory access over clever abstractions.

Keep hot loops simple and free from unnecessary validation, allocation, and indirection.

## APIs and Invariants

Design APIs so invalid states are difficult to represent.

Do not remove null checks by ignoring errors. Remove them by establishing stronger invariants.

Validate and resolve resources at system boundaries. Inner loops should operate on already-valid data.

If failure is possible, make it explicit through:

* Invalid IDs
* Boolean results
* Error values
* Output parameters

Avoid APIs that force callers to repeatedly reconstruct the same validity assumptions.

## Preparation vs Execution

Separate preparation from execution where practical.

Preparation may handle:

* Validation
* ID resolution
* Sorting
* Culling
* Batching
* Command generation

Execution should operate on prepared data with minimal branching and indirection.

## Functions

Prefer meaningful, substantial functions.

Do not create tiny functions solely for abstraction, indirection, or stylistic purity.

Extract functions when they provide meaningful reuse, improve reasoning, establish a useful boundary, or enable testing.

Prefer straightforward code over fragmented code.
prefer long functions if it is called once avoid to not make it function
## Global State

Minimize global state and hidden side effects.

Prefer explicit parameters and return values:

```c
render_frame(Renderer *renderer, Frame *frame);
```

Avoid hidden dependencies and implicit mutation.

Important state should be visible in the function interface.

## Naming

Types use `PascalCase`.

Functions use `snake_case`.

Variables and fields use `snake_case`.

Align related declarations for readability:

```c
TextureId  texture_id;
MeshId     mesh_id;
MaterialId material_id;

uint32_t   width;
uint32_t   height;
uint32_t   stride;
```

## Data Structures

Prefer simple structures whose cost and behavior are obvious.

Good defaults:

* Dense arrays
* Sparse sets
* Fixed-capacity arrays
* Pools
* Arenas
* IDs/handles
* SoA when fields are processed independently
* AoS when complete records are processed together

Choose based on access pattern, lifetime, mutation, locality, and capacity.

Do not introduce a complex data structure without a concrete need.

## External Code

External libraries should not dictate engine architecture.

Keep third-party code isolated from engine-specific ownership and data models.

When extending `mylib`, prefer reusable primitives over one-off abstractions.

## Optimization Discipline

Measure first.

```text
measure → identify bottleneck → change → measure again
```

Do not optimize hypothetical bottlenecks.

Prefer changes that improve memory behavior, locality, and data movement before micro-optimizing individual instructions.

## Code Quality

* Keep ownership obvious.
* Keep lifetimes predictable.
* Keep control flow straightforward.
* Avoid hidden work.
* Avoid unnecessary indirection.
* Avoid unnecessary allocation.
* Avoid abstraction for its own sake.
* Use assertions to enforce important invariants in debug builds.
* Comments explain why, not what.
* Prefer simple machinery with visible costs.
- Do not add obvious comments such as "arguments must be live objects". C/C++ programmers already understand that using destroyed objects is invalid.
 Do not add asserts or comments describing 32-bit overflow cases. The existing 32-bit ranges (about 4 billion elements or 4 GB) are sufficient.
- Do not use C++ standard-library headers or facilities in project code, including utilities, examples, and tests.

- Avoid adding named variables for trivial expressions, especially when the value is used only once.

- Perform error checks as early as possible. Check application initialization, resource loading, and Vulkan object creation immediately. Avoid error checking after initialization; normal code should not fail.
- Perform error checks as early as possible. Check application initialization, resource loading, and Vulkan object creation immediately. Avoid error checking after initialization; normal code should not fail.
- Treat the library as a low-level, thin wrapper, not as a validation layer. Validate only inputs and state whose misuse could make the wrapper itself crash.
  Document those preconditions and enforce programmer errors with asserts.
- Data and parameters passed directly from the user to Vulkan are the user's responsibility. Do not duplicate Vulkan validation or maintain shadow state
  solely to validate them; users should enable the Vulkan validation layer during development.
- MU gfx api resource destruction is immediate. In applications, examples, and tests, destroy resources only after no recorded or executing GPU frame
  uses them. Wait for the submission timeline value covering the final use, or use the optional  `DeleteQueue` to defer destruction.
- At shutdown, call `wait_idle`, drain every  `DeleteQueue`, and then destroy resources and the device.
- Wait for every submitted frame to drain before calling `destroy_device`.
- Group APIs logically in public headers; for example, keep all command buffer APIs together.
- Keep each public resource creation function next to its matching destruction function. Keep the shared lifetime policy in one place rather than repeating it
  for individual resource types.
- Do not abort for programming errors. Enforce their documented preconditions with asserts and leave release builds free of those checks.
- Use error codes and error messages only for invalid external input data and initialization failures.
- Do not check for memory allocation failures. We cannot recover from running out of memory; managing memory usage properly is the user's responsibility.


- Avoid copying large user data structures. Prefer references to structures, and use spans for array data in structures and function parameters.

use custom span from mu library 
- Always pass `Span`, `ByteSpan` etc function parameters by value. This allows the compiler to pass their pointer-and-size fields in registers instead of forcing a memory store/load round trip. 
- prefer to use designated initializer 
- Always review code for performance issues before considering work complete.
- ask question about decisions rather than assuming this and that user is a programmer not a novice 

- this is very intelligenth way to avoid switch cases to to avoid branches in code
 typedef enum LoadOp {
   NEW_API     = OLD_API,

 } SOMETHING;

- never waste time in writing tests
- do not introduce too much additional files and if necessary ask before introducing 
- be talktive ask question about implemention of algorithm and how you should approach the problem rather than just blindly taking decision yourself ask as much as to me about 
changes you do 
- do not inspect git history and other branches they will bloat you with useless information 