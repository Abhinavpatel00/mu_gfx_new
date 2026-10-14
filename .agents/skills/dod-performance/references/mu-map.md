# mylib (`mu`) API Map

Companion to `.agents/skills/dod-performance/SKILL.md`. Verified from the
headers in `mylib/`. Full documentation: `mylib/docs/`.

**Rule:** use `mu` for containers, allocators and primitives. If a primitive is
missing, add a small reusable one under `mylib/mu/` — do not add a competing
container or allocator family to engine code.

---

## Include map

```c
#include "mu.h"                    /* umbrella: core, containers, allocators,
                                      bitsets, spans, RNG, perf, hash tables */
#include "mu_ecs.h"                /* entities, pools, transforms, prefabs */
#include "mu_thread_system.h"      /* worker pool + atomics */
#include "mu_task_scheduler.h"     /* priorities, groups, parallel_for */
#include "allocators.h"            /* arena, fixed pool, buddy */
#include "offset_allocator.h"      /* two-level bin free-space allocator */
#include "mu_mmgr.h"               /* leak/corruption tracking malloc shim */
#include "mu_min_containers.h"     /* standalone stretchy buffer + hash64 */
#include "mu_geometric_algebra.h"  /* PGA / conformal math (needs cglm) */
#include "mu_noise_math.h"         /* value/perlin/simplex/worley/fBm/warp */
#include "mu_roguelike.h"          /* grid gen, FOV, pathing, tick scheduler */
```

Linkage: define `MU_STATIC` for single-TU static symbols; `MU_DLL_EXPORT` /
`MU_DLL_IMPORT` on Windows. Override `mu_malloc` / `mu_free` (and
`MU_TS_MALLOC`, `MUC_MALLOC`, `MU_ECS_MALLOC`) before including to redirect
allocation. `mu_malloc.h` overrides are macros, not vtables.

---

## `mu/mu_common.h` — macros and bit intrinsics

```c
MU_ASSERT(cond);              MU_PANIC();
MU_INLINE; MU_NOINLINE; MU_RESTRICT; MU_ALIGN(N);
MU_LIKELY(x); MU_UNLIKELY(x); MU_PREFETCH(addr);
MU_MIN(a,b); MU_MAX(a,b); MU_CLAMP(x,lo,hi); MU_SWAP(T,a,b);
MU_ARRAY_COUNT(a); MU_IS_POW2(x);
MU_KB/MB/GB(x); MU_CEIL(x,y);
MU_ALIGN_UP(x,a); MU_ALIGN_DOWN(x,a);
MU_BIT(n); MU_HAS_FLAG/SET_FLAG/CLEAR_FLAG(x, flag);
MU_OFFSET_OF(T, m); MU_CONTAINER_OF(p, T, m);
MU_STATIC_ASSERT(cond, msg);
MU_UNUSED(x); MU_NOOP();

int mu_trailing_zeroes_u64(uint64_t x);   /* 64 if x == 0 */
int mu_leading_zeroes_u64(uint64_t x);    /* 64 if x == 0 */
int mu_popcount_u64(uint64_t x);
```

---

## `mu/mu_array.h` — header-prefixed stretchy buffer

```c
float *a = NULL;                    /* NULL is a valid empty array */
array_push(a, 1.0f);                /* grows x2 from 16 */
array_size(a); array_capacity(a); array_full(a);
array_reserve(a, n);                /* exact reserve, bypasses doubling */
array_back(a); array_pop(a); array_clear(a); array_free(a);
```

Header layout: `size, capacity` sit immediately before the user pointer.
Gotchas: macros evaluate the argument multiple times (don't pass `foo()`);
`array_free` needs an lvalue and sets it to `NULL`.

---

## `mu/mu_span.h` — non-owning views

```c
mu_span_f32 s = mu_span_f32_make(v, 4);       /* typed aliases: count = ELEMENTS */
mu_span     b = mu_span_from_array(v);        /* plain mu_span: count = BYTES */
mu_span_f32_sub(s, 1, 3);                     /* clamped slice, never traps */
mu_span_f32_first(s, 2); mu_span_f32_last(s, 2);
mu_span_f32_count/empty/find/contains(s, v);
mu_span_f32_copy_to(s, dst, n);               /* memmove, clamped */
mu_span_equal(a, b);
MU_SPAN_OF(T, name); MU_SPAN_IMPL(T, name);
```

Rule: plain `mu_span.count` is **bytes**; typed aliases' `count` is **elements**.
A span is a borrow — invalidated by realloc/free of its backing store.

---

## `mu/mu_bitset.h` — packed bit set, full set algebra

```c
mu_bitset *bs = mu_bitset_create();
mu_bitset *bs = mu_bitset_create_with_capacity(130);
mu_bitset_set(bs, 63); mu_bitset_reset(bs, 63); mu_bitset_test(bs, 63);
mu_bitset_set_bit(bs, 63, true);
mu_bitset_clear(bs); mu_bitset_fill(bs); mu_bitset_copy(bs);
mu_bitset_resize_words(bs, n); mu_bitset_grow(bs, n); mu_bitset_trim(bs);
mu_bitset_shift_left(bs, k); mu_bitset_shift_right(bs, k);

size_t mu_bitset_count(bs); bool mu_bitset_empty(bs);
size_t mu_bitset_minimum(bs); size_t mu_bitset_maximum(bs);

mu_bitset_inplace_union/intersection/difference/symmetric_difference(a, b);
mu_bitset_union_count/intersection_count/difference_count(a, b);
mu_bitset_xor/or/and/not(out, a, b);
mu_bitsets_disjoint(a, b); mu_bitsets_intersect(a, b);
mu_bitset_contains_all(a, b); mu_bitset_equal(a, b);

/* iteration: CTZ-driven, one step per set bit, skips zero words */
size_t i = 0; while (mu_bitset_next_set_bit(bs, &i)) { /* use i */ ++i; }
mu_bitset_for_each(bs, visitor, user);            /* visitor returns false to stop */
size_t n = mu_bitset_next_set_bits(bs, buf, cap, &from);   /* batch extract */
mu_bitset_print(bs);
```

---

## `mu/mu_sparse_set.h` — O(1) membership over `[0, capacity)`

```c
mu_sparse_set_init(&set, MAX_ENTITIES);
mu_sparse_set_insert(&set, v);      /* false if out of range or duplicate */
mu_sparse_set_remove(&set, v);      /* O(1) swap-delete */
mu_sparse_set_contains(&set, v);
mu_sparse_set_clear(&set); mu_sparse_set_destroy(&set);

for (uint32_t i = 0; i < set.count; ++i)
    uint32_t v = mu_sparse_set_at(&set, i);      /* dense[i], packed */
```

`dense` = active values (tight iteration). `sparse[v]` = dense index.
Membership test is `sparse[v] < count && dense[sparse[v]] == v`.
Iteration order is not stable (swap-remove). Capacity is fixed at init.

---

## `mu/mu_bulk_storage.h` — slots with holes + generational handles

```c
typedef struct { uint32_t id; uint32_t generation; } mu_weak_handle;

mu_bulk_storage_init(&st, sizeof(Item), 64);
mu_bulk_storage_deinit(&st);
uint32_t id = mu_bulk_storage_alloc(&st);                  /* 0 on failure */
Item *it    = (Item *)mu_bulk_storage_ptr(&st, id);
mu_bulk_storage_free(&st, id);               /* false on invalid/dead */
mu_bulk_storage_is_live(&st, id);

mu_weak_handle h = mu_bulk_storage_make_handle(&st, id);
bool fresh = mu_bulk_storage_validate_handle(&st, h);
Item *live = (Item *)mu_bulk_storage_resolve_handle(&st, h);  /* NULL if stale */

mu_bulk_storage_visit_live(&st, visitor, user);
```

Slot 0 is freelist metadata. `deinit` frees bytes, not objects. IDs/handles stay
stable across growth; raw pointers do not. Iteration is a sparse scan — use
`mu_sparse_set` / ECS for dense iteration.

---

## Hash tables

```c
/* quick u64→u64, no count/remove/resize; keys must not equal 0 */
hash_t h; hash_init(&h, 1024);
hash_put(&h, key, value); uint64_t v = hash_get(&h, key, fallback); hash_free(&h);

/* owning u64→u32, fixed capacity, removal via backshift repair */
mu_hash32_t h32; mu_hash32_init(&h32, 1024);
mu_hash32_set/remove/clear/destroy(&h32, key);
uint32_t out; mu_hash32_get(&h32, key, &out);   /* true/false, no magic 0 */
mu_hash32_count/capacity(&h32); mu_hash32_contains(&h32, key);

/* non-owning static: caller-provided arrays, no alloc, no growth, no remove */
uint64_t keys[256]; uint32_t vals[256];
mu_hash32_static_t hs; mu_hash32_static_init(&hs, keys, vals, 256);
mu_hash32_static_set(&hs, key, value); uint32_t v = mu_hash32_static_get(&hs, key);

/* u64→u64 with tombstones + rehash at 70% */
mu_hash64_init(&h64, 16); mu_hash64_set(&h64, key, value);
mu_hash64_get(&h64, key, &out); mu_hash64_get_or(&h64, key, fallback);
mu_hash64_remove(&h64, key); mu_hash64_clear/free(&h64);
```

Notes: `hash_t` has no `count`/`remove`; `mu_hash32_set` asserts against a full
table (capacity planning, not resize); reserved keys must be avoided.

---

## `mu/mu_id_pool.h` — ranged ID allocation

```c
mu_id_pool_init(&pool, 4096);                     /* IDs [0, 4096) */
uint32_t id; mu_id_pool_create_id(&pool, &id);
uint32_t first; mu_id_pool_create_range_id(&pool, &first, 16);
mu_id_pool_destroy_id(&pool, id);
mu_id_pool_destroy_range_id(&pool, first, 16);
mu_id_pool_get_available_ids(&pool); mu_id_pool_is_id(&pool, id);
mu_id_pool_is_range_available(&pool, n);
mu_id_pool_get_largest_continuous_range(&pool);
mu_id_pool_destroy_all(&pool); mu_id_pool_deinit(&pool);
```

Free runs are sorted and coalesced; original use case: bindless descriptor
allocation.

---

## `mu/mu_multi_index.h` — one key → many values

```c
mu_multi_index_init(&idx, node_cap, map_cap);
uint32_t node = mu_multi_index_add(&idx, key, value);   /* NONE on OOM */
mu_multi_index_remove(&idx, node);                      /* O(1): unlink+recycle */

/* node = the node you started the traversal from (the first one) */
for (uint32_t n = mu_multi_index_first(&idx, key);
     mu_multi_index_node_valid(&idx, n);
     n = mu_multi_index_next(&idx, first, n))
{ uint32_t v = mu_multi_index_value(&idx, n); }

mu_multi_index_count_key(&idx, key);
mu_multi_index_visit_key(&idx, key, visitor, user);
```

---

## `mu/mu_chunked_array.h` — many small variable `uint32_t` lists

```c
mu_chunked_u32_pool_init(&pool, 256);
mu_chunked_u32_array arr; mu_chunked_u32_array_init(&arr);   /* cheap value struct */
mu_chunked_u32_array_push(&pool, &arr, 42);
mu_chunked_u32_array_pop(&pool, &arr, &v);
mu_chunked_u32_array_get(&pool, &arr, 0, &v);
mu_chunked_u32_array_visit(&pool, &arr, visitor, user);
mu_chunked_u32_array_clear(&pool, &arr);
```

Chunks are chained by indices, never pointers. The logical array is just
three integers, so it is cheap to copy/store/serialize. Chunk size override:
`MU_ARRAY_OF_ARRAYS_CHUNK_SIZE` (default 14).

---

## `mu/mu_string.h` — packed strings + intrusive lists

```c
mu_string_arena a; mu_string_arena_init(&a);
uint32_t idx; mu_string_arena_push(&a, "player", &idx);
const char *s = mu_string_arena_get(&a, idx);
mu_string_arena_count(&a); mu_string_arena_bytes(&a);
mu_string_arena_clear(&a); mu_string_arena_free(&a);

mu_pool_link_detach(links, node); mu_pool_link_is_detached(links, node);
mu_index_list_init(links, head); mu_index_list_empty(links, head);
mu_index_list_insert_after/before(links, at, node); mu_index_list_remove(links, node);

mu_freelist_init(links, head);
mu_freelist_push(links, head, node);
uint32_t n = mu_freelist_pop(links, head);       /* MU_INVALID_INDEX when empty */
```

`MU_INVALID_INDEX` (`UINT32_MAX`) is the universal "no such thing" sentinel.

---

## `mu/mu_length_index.h` — cumulative length / rank index

```c
MuLengthIndex idx; mu_length_index_init(&idx);   /* zeroed storage first */

/* find by element index (by_byte = false) or by byte offset (by_byte = true) */
MuLengthPosition pos = mu_length_index_find(&idx, value, /*by_byte=*/false);
uint32_t len = mu_length_index_get(&idx, pos);
mu_length_index_set(&idx, pos, new_len);
mu_length_index_rebuild(&idx);
mu_length_index_splice(&idx, first, removed_count, lengths_byte_span);
mu_length_index_destroy(&idx);
```

Positions are borrowed and expire on mutation. Byte lookup at a boundary selects
the following nonempty element; total length selects the last element.

---

## `mu/mu_bitpacking.h` — quantization / bit packing

```c
mu_quantize_unorm(x, bits); mu_dequantize_unorm(u, bits);
mu_quantize_snorm(x, bits); mu_dequantize_snorm(u, bits);
mu_quantize_half(f);        mu_dequantize_half(u16);
mu_quantize_float(x, min, max, bits);
```

Packs small bounded values (colors, normals, UVs, flags) into fewer bits.
The header explains the union/`memcpy` bit-reinterpretation technique.

---

## Allocators

```c
/* allocators.h: linear arena */
mu_arena a; a.base = buf; a.capacity = size; a.used = 0;
void *p = mu_arena_alloc(&a, size, align);
mu_arena_reset(&a);

/* allocators.h: fixed block pool */
mu_pool p; mu_pool_init(&p, buffer, block_size, block_count);
void *p1 = mu_pool_alloc(&p); mu_pool_free(&p, p1);

/* allocators.h: buddy */
uint32_t need = mu_buddy_metadata_size(total, leaf);
mu_buddy b; mu_buddy_init(&b, arena, arena_size, leaf, meta, meta_size);
void *p2 = mu_buddy_alloc(&b, size, align);
mu_buddy_free(&b, p2); mu_buddy_free_known(&b, p2, size);
```

```c
/* mu/mu_allocators.h: inline linear + ring */
mu_linear_allocator la; mu_linear_init(&la, mem, size);
void *p = mu_linear_alloc(&la, size, align); mu_linear_reset(&la);

mu_ring_allocator ra; mu_ring_init(&ra, mem, size);
uint32_t off; void *q = mu_ring_alloc(&ra, size, align, &off);
mu_ring_free_to(&ra, off); mu_ring_used(&ra);
```

```c
/* offset_allocator.h: two-level bin free-space allocator */
/* mu_mmgr.h: malloc shim with leak/corruption tracking for debug builds */
```

`MU_ALLOCATORS_IMPLEMENTATION` must be defined in exactly one TU for
`allocators.h`.

---

## RNG and noise

```c
mu_pcg32 rng; mu_pcg32_init(&rng, seed, seq);
uint32_t r = mu_pcg32_next_u32(&rng);
/* AVX2 batch variant (mu_pcg32x4) is present but commented out in the header */

/* mu_noise_math.h: value / perlin / simplex / worley / fBm / warp */
/* mu_roguelike.h: RNG, grid gen, FOV, pathfinding, tick scheduler */
```

Deterministic seeding is what makes JIT mementos work: `seed = hash(id)`.

---

## ECS (`mu_ecs.h`)

```c
/* entity manager: arg is min_free (capacity headroom), not a hard capacity */
mu_ecs_entity_manager em; mu_ecs_entity_manager_init(&em, min_free);
mu_ecs_entity e = mu_ecs_entity_create(&em);
mu_ecs_entity_index(e); mu_ecs_entity_generation(e); mu_ecs_entity_alive(&em, e);
mu_ecs_entity_create_batch(&em, count, out_entities);
mu_ecs_entity_destroy(&em, e);                /* queued, not immediate */

/* component pools: init takes only the element size; storage grows */
mu_ecs_pool pool; mu_ecs_pool_init(&pool, sizeof(Component));
void *c = mu_ecs_pool_add(&pool, e);  void *g = mu_ecs_pool_get(&pool, e);
mu_ecs_pool_has(&pool, e); mu_ecs_pool_remove(&pool, e);

/* transforms: SoA arrays + index hierarchy links; instance handles, not entities */
mu_ecs_transform_store tr; mu_ecs_transform_init(&tr);
mu_ecs_instance inst = mu_ecs_transform_add(&tr, e);
mu_ecs_transform_set_local(&tr, inst, matrix16);   /* float[16] */
mu_ecs_transform_link_child(&tr, parent_inst, child_inst);
mu_ecs_transform_propagate(&tr, root_inst);

/* convenience world bundles manager + transforms */
mu_ecs_world w; mu_ecs_world_init(&w, min_free);

/* resource registry + prefabs */
mu_ecs_resource_registry rr; mu_ecs_resource_registry_init(&rr);
mu_ecs_resource_registry_set_pool(&rr, ...);
mu_ecs_resource_registry_set_prefab(&rr, ...);
mu_ecs_prefab_spawn(&rr, prefab, &w);
```

Entities are packed index + generation handles. Components live in dense arrays
with an entity→dense map. Transform hierarchy uses indices, and writing a local
transform updates the subtree immediately.

---

## Threading and tasks

```c
/* ---------------- mu_thread_system.h ---------------- */
/* TaskFunc: void (*mu_ts_task_fn)(void *user, uint64_t thread_id);
   thread_id == UINT64_MAX when executed by assist()/dummy mode */

mu_ts_init_desc desc = MU_TS_INIT_DESC_DEFAULT;
desc.thread_count = 4;         /* 0 = dummy/deterministic single-thread mode */
desc.thread_name  = "worker";
mu_ts_handle ts;
mu_ts_init(&ts, &desc);                      /* false on failure */

mu_ts_add_task(ts, fn, user);                /* one task  */
mu_ts_add_tasks(ts, fn, count, sizeof(user_t), user_array);   /* count tasks */

mu_ts_wait_idle(ts);
mu_ts_wait_idle_timeout(ts, ms);
mu_ts_is_idle(ts);
mu_ts_assist(ts);
mu_ts_cpu_count();
mu_ts_get_info(ts, &info);
mu_ts_exit(ts, &exit_desc);

mu_ts_mutex_init/lock/unlock/exit(&m);
mu_ts_cond_init/wait/wake_one/wake_all/exit(&c);
mu_ts_atomic32_load_relaxed/store_relaxed/add_relaxed/cas_relaxed(&a, ...);
mu_ts_atomic64_* (same set);
mu_ts_memorybarrier_acquire/release();

/* ---------------- mu_task_scheduler.h ---------------- */
/* TaskFunc: void (*mu_sched_task_fn)(void *user, uint32_t worker_index);
   parallel_for: void (*mu_sched_parallel_for_fn)(uint32_t begin, uint32_t end,
                                                  uint32_t worker_index, void *user); */

mu_sched_init_desc sd = MU_SCHED_INIT_DESC_DEFAULT;
sd.worker_count = 4;
mu_task_scheduler *sched;
mu_sched_init(&sched, &sd);                  /* false on failure */

mu_sched_task_desc td = {
    .fn                 = fn,
    .user               = user,
    .priority           = MU_SCHED_PRIORITY_NORMAL,   /* HIGH=0, NORMAL=1, LOW=2 */
    .depends_on         = NULL,                       /* run when counter hits 0 */
    .completion_counter = &counter,                   /* inc on submit, dec on done */
};
mu_sched_submit(sched, &td);
mu_sched_submit_tasks(sched, descs, count);

mu_sched_parallel_for(sched, begin, end, grain, MU_SCHED_PRIORITY_NORMAL, fn, user);

mu_sched_counter counter; mu_sched_counter_init(&counter, /*initial=*/0);
mu_sched_counter_wait(&counter);
mu_sched_counter_wait_timeout(&counter, ms);
mu_sched_group group; mu_sched_group_init(&group, 0);   /* group == counter */
mu_sched_group_wait(&group);

mu_sched_wait_idle(sched);
mu_sched_worker_count(sched);
mu_sched_exit(sched, ...);
```

Group/counter rule: the counter increments on submit with a
`completion_counter` and decrements on completion; waiting on a counter/group
waits for all associated tasks. Use `mu_sched_parallel_for` for map/filter over
ranges; keep outputs thread-local and gather at the end.

---

## Perf (`mu/mu_perf.h`)

```c
MU_SCOPE_TIMER("physics");          /* prints "physics: 0.123 ms" on scope exit */
uint64_t t0 = mu_time_now();        /* nanoseconds on POSIX, QPC ticks on Windows */
double   f  = mu_time_freq();
```

On glibc, strict `-std=c99` needs `-D_POSIX_C_SOURCE=199309L` for
`clock_gettime`.

---

## Building a test

```sh
cc -std=c99 -O2 test.c mu/mu_bitset.c mu/mu_sparse_set.c mu/mu_id_pool.c \
   mu/mu_multi_index.c mu/mu_chunked_array.c mu/mu_bulk_storage.c -o test
c++ -std=c++17 -O2 -fpermissive -I. test.cpp mu/*.c -o test_cpp
```
