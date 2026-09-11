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
