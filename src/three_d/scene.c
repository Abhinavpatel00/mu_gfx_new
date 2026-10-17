/* scene.c — GPU-driven data-oriented 3D scene.
   T1 upload, T2 cull, T3 compact, T5 draw, T6 counters.
   See docs/gpu-driven-dod-redesign.md. */
#include "scene.h"

#include "../../common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#include "../../external/stb/stb_image.h"
#include "../../external/stb/stb_image_resize2.h"

#define MU_ALLOCATORS_IMPLEMENTATION
#include "../../external/mu/allocators.h"

/* Sparkles emitted per slime in the transparent pass. Purely procedural (see
   sparkle.slang), so this is the only knob: one draw of slime_count * this
   camera-facing quads. */
#define SCENE_SPARKLES_PER_SLIME 4u

/* Cap on blocking discs in the camera-relative obstacle grid. A streamed window
   of the world holds a few thousand trunks/boulders; the slices are sized once
   at this cap so a window change only re-uploads, never reallocates. */
#define SCENE_SLIME_OBS_CAP 16384u

/* ============================================================ small encoders */

static uint16_t f2h(float x) {
    uint32_t bits;
    memcpy(&bits, &x, 4);
    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t  e    = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t m    = bits & 0x7FFFFFu;
    if (e <= 0)
        return (uint16_t)sign;
    if (e >= 31)
        return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)e << 10) | (m >> 13));
}

static uint32_t pack_half2(float a, float b) { return (uint32_t)f2h(a) | ((uint32_t)f2h(b) << 16); }

static uint32_t pack_oct(const float n[3]) {
    float x = n[0], y = n[1], z = n[2];
    float l = fabsf(x) + fabsf(y) + fabsf(z);
    if (l < 1e-8f)
        return 0;
    float ox = x / l, oy = y / l;
    if (z < 0.0f) {
        float t = (1.0f - fabsf(ox)) * (ox >= 0.0f ? 1.0f : -1.0f);
        float u = (1.0f - fabsf(oy)) * (oy >= 0.0f ? 1.0f : -1.0f);
        ox      = t;
        oy      = u;
    }
    int32_t xi = (int32_t)lrintf(ox * 32767.0f);
    int32_t yi = (int32_t)lrintf(oy * 32767.0f);
    return (uint32_t)(xi & 0xFFFF) | ((uint32_t)(yi & 0xFFFF) << 16);
}

static uint32_t pack_quat(const float q[4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n = sqrtf(x * x + y * y + z * z + w * w);
    if (n < 1e-8f)
        return 0;
    x /= n;
    y /= n;
    z /= n;
    w /= n;
    if (w < 0.0f) {
        x = -x;
        y = -y;
        z = -z;
    }
    int32_t xi = (int32_t)lrintf(x * 511.0f);
    int32_t yi = (int32_t)lrintf(y * 511.0f);
    int32_t zi = (int32_t)lrintf(z * 511.0f);
    xi         = xi < -511 ? -511 : (xi > 511 ? 511 : xi);
    yi         = yi < -511 ? -511 : (yi > 511 ? 511 : yi);
    zi         = zi < -511 ? -511 : (zi > 511 ? 511 : zi);
    return ((uint32_t)xi & 0x3FFu) | (((uint32_t)yi & 0x3FFu) << 10) | (((uint32_t)zi & 0x3FFu) << 20);
}

void scene_pack_vertex(struct ScenePackedVertex *out, const float pos[3], const float normal[3], const float uv[2]) {
    out->position_xy = pack_half2(pos[0], pos[1]);
    out->position_z  = (uint32_t)f2h(pos[2]);
    out->normal_oct  = pack_oct(normal);
    out->uv          = pack_half2(uv[0], uv[1]);
}

const SceneInstanceDesc kSceneInstanceIdentity = {.pos = {0, 0, 0}, .quat = {0, 0, 0, 0}, .scale = 1.0f};

/* ============================================================ internal types */

typedef struct SceneCpuInstance {
    struct SceneInstance gpu;
    uint32_t             mesh;
    uint8_t              alive; /* slot holds a live instance; not read by any hot loop */
} SceneCpuInstance;

typedef struct SceneViewFrame {
    bool        ready; /* transient slices allocated for this view and lane */
    BufferSlice survivors;
    BufferSlice survivor_count;
    BufferSlice group_count;
    BufferSlice vis_base;
    BufferSlice cmd_index;
    BufferSlice cursor;
    BufferSlice vsrows;     /* max_survivors x SceneVsRow: scatter output, read by the VS */
    BufferSlice draws;
    BufferSlice draw_count;
    BufferSlice scan_aux;   /* 3 * SCENE_SCAN_BLOCK u32x2: block totals + offsets */
    BufferSlice cull_args;  /* VkDispatchIndirectCommand triple, INDIRECT usage */
    BufferSlice group_base; /* max_survivors x u32: vis_base -> group id */
    BufferSlice surv_sorted; /* max_survivors x u32: depth-sorted survivor list */
    BufferSlice sort_aux;   /* 128 u32: depth-bucket hist[64] | cursor[64] */

} SceneViewFrame;

#define SCENE_MAX_LODS 4

struct Scene {
    VkBackend *vk;

    uint32_t max_instances;
    uint32_t max_meshes;
    uint32_t max_lod_rows;
    uint32_t max_materials;
    uint32_t max_survivors;

    /* One backing allocation for every persistent CPU table below. Bump-allocated
       at create, released as a single block at destroy. See scene_create. */
    mu_arena arena;

    /* persistent gpu tables */
    BufferSlice instances;
    BufferSlice cull_meshes;
    BufferSlice lod_rows;
    BufferSlice materials;
    BufferSlice cull_rows;
    BufferSlice group_static; /* G x SceneGpuDraw templates */
    BufferSlice group_shade;  /* G x SceneGroupShade: rung vertex stream + material */
    BufferSlice vertex_arena; /* mesh-local packed vertices, every rung of every mesh */
    BufferSlice index_arena;  /* u16 indices, bound once per class */

    /* CPU staging for assets (uploaded once by scene_upload_scene) */
    struct ScenePackedVertex *staged_vertices;
    uint32_t                  staged_vertex_count;
    uint32_t                  staged_vertex_cap;
    uint16_t                 *staged_indices;
    uint32_t                  staged_index_count;
    uint32_t                  staged_index_cap;

    struct SceneCullMesh    *cpu_cull;
    struct SceneGroupShade  *cpu_group_shade; /* indexed mesh * SCENE_MAX_LODS + lod */
    uint32_t                *cpu_rung_vbase;  /* vertex base per rung, baked into the shade row at upload */
    struct SceneLodRow      *cpu_lod;
    struct SceneGpuMaterial *cpu_mat;
    struct SceneCullRow     *cpu_rows;
    struct SceneGpuDraw     *cpu_group_static;

    uint32_t mesh_count;
    uint32_t lod_row_count;
    uint32_t material_count;
    uint32_t lod_count;   /* rungs every mesh carries; == 1 until an LOD arrives */
    uint32_t group_count; /* mesh_count * lod_count, fixed at upload */

    /* CPU truth. One slot space, split by a scene constant: [0, dynamic_count)
       is the dynamic prefix rewritten every frame, [dynamic_capacity, ...) is the
       static suffix uploaded once. No per-row region flag exists. */
    SceneCpuInstance     *cpu_instances;
    struct SceneInstance *instance_upload_scratch;
    struct SceneCullRow  *candidate_upload_scratch;
    uint32_t             *compact_dst; /* scene_compact scratch, fixed capacity */
    uint32_t              dynamic_capacity;
    uint32_t              static_capacity;
    uint32_t              dynamic_count;    /* live dynamic slots, dense in [0, dynamic_count) */
    uint32_t              static_count;     /* live static slots, dense in [dynamic_capacity, +static_count) */
    uint32_t              instance_count;   /* dynamic_count + static_count                 */
    uint32_t              candidate_count;  /* == instance_count; candidates mirror slots   */
    bool                  uploaded_dirty;   /* static region changed; needs re-upload       */
    bool                  candidates_dirty; /* roster changed; candidate rows need re-upload  */
    /* in struct Scene */
    bool      cull_args_dirty[MAX_FRAMES_IN_FLIGHT];
    uint32_t *dirty_slots;
    uint32_t  dirty_count;

    /* static rows changed since the last upload; re-uploaded per row */
    uint32_t *static_dirty_slots;
    uint32_t  static_dirty_count;

    /* row_of[slot] is the instance's index in the dense candidate array, or
       UINT32_MAX when retired. Slot and row indices are separate spaces once
       static instances exist, and a destroy's swap-remove permutes rows, so the
       mapping has to be explicit rather than assumed. */
    uint32_t *row_of;

    /* Bloom membership: a mu_bitset for O(1) test/set plus a dense list of
       blooming slots rebuilt on upload. Strength is a parallel table uploaded
       whole to the GPU; the VS reads bloom[slot] and forwards it, and the FS
       writes it into HDR alpha where the bloom bright-pass gates on it. No
       hot loop branches on any of this. */
    mu_bitset   bloom_bits;
    float      *bloom_strength; /* max_instances, 0 = excluded from bloom */
    uint32_t   *bloom_dense;    /* blooming slots, dense in [0, bloom_count) */
    uint32_t    bloom_count;
    bool        bloom_dirty;
    BufferSlice bloom_table; /* max_instances x float on the GPU */

    /* Wiggle: per-instance VS jiggle amplitude. A parallel table to bloom, but
       far simpler — no membership set, no dense list. Amplitude is set once at
       spawn and never changes, so the animation is purely time-driven in the VS
       and the table is uploaded once (and again only if an amplitude changes). */
    float      *wiggle_amp;   /* max_instances, 0 = static */
    bool        wiggle_dirty; /* an amplitude changed; re-upload the table */
    BufferSlice wiggle_table; /* max_instances x float on the GPU */

    /* Squash: per-instance hop squash-and-stretch, same machinery as wiggle.
       1 = rest, <1 squashed, >1 stretched. Set every frame for hopping slimes;
       the table re-uploads whole only when dirty, like wiggle. */
    float      *squash_v;
    bool        squash_dirty;
    BufferSlice squash_table; /* max_instances x float on the GPU */

    /* Stretch: per-instance forward-axis stretch along the instance's facing,
       same machinery again. 1 = rest; >1 elongates the body along +Z (the
       movement direction the yaw already points at). */
    float      *stretch_v;
    bool        stretch_dirty;
    BufferSlice stretch_table; /* max_instances x float on the GPU */

    /* Face: per-instance slime-face atlas tile, same machinery as wiggle.
       Read only by FACE-material quads. 0 is the default tile. */
    uint32_t   *face_id;    /* max_instances */
    bool        face_dirty; /* a tile changed; re-upload the table */
    BufferSlice face_table; /* max_instances x u32 on the GPU */

    /* per view, per frame-in-flight transient tables */
    SceneViewFrame view_frame[SCENE_MAX_VIEWS][MAX_FRAMES_IN_FLIGHT];
    uint32_t       view_count;

    SceneViewDesc views[SCENE_MAX_VIEWS];
    float         clear[4];

    /* counters */
    BufferSlice   counters[MAX_FRAMES_IN_FLIGHT];
    Buffer        counters_host[MAX_FRAMES_IN_FLIGHT];
    SceneCounters last_counters;
    uint32_t      frame_serial;
    bool          has_counters;

    /* pipelines */
    PipelineID cs_cull, cs_count, cs_scan_block, cs_scan_blocks, cs_emit, cs_scatter;
    PipelineID cs_sort_clear, cs_sort_hist, cs_sort_scan, cs_sort_scatter;
    PipelineID cs_slime_sim, cs_slime_grid, cs_slime_resolve;
    PipelineID draw_pipeline;
    PipelineID toon_pipeline; /* cel-shaded variant of the scene draw */
    PipelineID sparkle_pipeline; /* additive transparent sparkle pass */
    bool       toon;
    bool       pipelines_ready;
    bool       uploaded;
    VkFormat   color_format, depth_format;

    /* GPU slime sim. When active, the dynamic instances in [0, 2*slime_count)
       are owned by compute, not the CPU upload path, and the three passes run
       once per frame before the cull. State is ping-ponged between two buffers
       so slime_resolve's neighbour reads see a stable snapshot. */
    bool               slime_active;
    uint32_t           slime_count;
    uint32_t           sparkles_per_slime; /* 0 disables the transparent pass */
    struct SlimeState *slime_state_init; /* CPU copy, freed once uploaded */
    BufferSlice        slime_state[2];
    BufferSlice        slime_grid_count; /* nx*nz u32, cleared per frame */
    BufferSlice        slime_grid_items; /* nx*nz*cap u32 */
    uint32_t           slime_grid_nx, slime_grid_nz, slime_grid_cap;
    float              slime_collide_radius;
    float              slime_cell_size;
    float              slime_inv_cell;
    uint32_t           slime_parity;

    /* Static obstacle collision: solid props (trunks, boulders) gathered once
       at plant time and bucketed into a camera-relative uniform grid. The
       resolve pass queries it with the same 3x3 neighbourhood it uses for
       slimes, so a grounded slime is pushed out of a trunk instead of merging
       into it. Obstacles are camera-relative like the hop grid because the
       streamed world scrolls: when the window changes the CPU re-buckets the
       live discs and re-uploads, so the grid always covers what the player can
       reach without ever holding the whole world. */
    struct SlimeObstacle *slime_obs_init;      /* CPU copy, freed once uploaded */
    uint32_t             *slime_obs_cpu_off;   /* CSR offsets, freed once uploaded */
    uint32_t             *slime_obs_cpu_items; /* CSR items, freed once uploaded */
    uint32_t             *slime_obs_cpu_cursor;/* CSR fill cursor, scratch */
    uint32_t              slime_obs_count;
    uint32_t              slime_obs_cap;       /* obstacle slice capacity (discs) */
    bool                  slime_obs_dirty;     /* world streamed: grid must rebuild */
    uint32_t              slime_obs_nx, slime_obs_nz;
    float                 slime_obs_min_x, slime_obs_min_z;
    BufferSlice           slime_obs_all;   /* flat SlimeObstacle[] */
    BufferSlice           slime_obs_off;   /* (nx*nz+1) u32 */
    BufferSlice           slime_obs_items; /* u32 */
};

/* ============================================================ helpers */

static uint64_t slice_addr(VkBackend *vk, BufferSlice s) { return vk->gpu_base_addr + s.offset; }

static BufferSlice alloc_slice(Scene *s, VkDeviceSize size, VkDeviceSize align) {
    return buffer_pool_alloc(&s->vk->gpu_pool, size, align);
}

static void pack_instance(struct SceneInstance *out, const SceneInstanceDesc *d) {
    out->pos[0]   = d->pos[0];
    out->pos[1]   = d->pos[1];
    out->pos[2]   = d->pos[2];
    out->quat     = pack_quat(d->quat);
    float scale   = d->scale == 0.0f ? 1.0f : d->scale;
    out->scale    = f2h(scale);
    out->lod_bias = (int16_t)lrintf(d->lod_bias);
}

/* ============================================================ create */

Scene *scene_create(VkBackend *vk, const SceneDesc *desc) {
    Scene *s = (Scene *)calloc(1, sizeof(Scene));
    if (!s || !vk)
        return NULL;
    s->vk            = vk;
    s->max_instances = desc->max_instances ? desc->max_instances : 4096;
    s->max_meshes    = desc->max_meshes ? desc->max_meshes : 64;
    s->max_lod_rows  = desc->max_lod_rows ? desc->max_lod_rows : 64;
    s->lod_count     = desc->lod_count ? desc->lod_count : 1;
    s->max_materials = desc->max_materials ? desc->max_materials : 64;
    s->max_survivors = desc->max_survivors ? desc->max_survivors : s->max_instances;

    /* The split point. A caller that gives only max_instances gets an all-dynamic
       scene, which is the old behaviour and still correct. */
    s->dynamic_capacity = desc->dynamic_capacity ? desc->dynamic_capacity : s->max_instances;
    s->static_capacity  = desc->static_capacity ? desc->static_capacity : (s->max_instances - s->dynamic_capacity);
    if (s->dynamic_capacity + s->static_capacity > s->max_instances)
        s->static_capacity = s->max_instances - s->dynamic_capacity;

    s->clear[0] = 0.05f;
    s->clear[1] = 0.07f;
    s->clear[2] = 0.10f;
    s->clear[3] = 1.0f;

    s->instances   = alloc_slice(s, (VkDeviceSize)s->max_instances * sizeof(struct SceneInstance), 16);
    s->cull_meshes = alloc_slice(s, (VkDeviceSize)s->max_meshes * sizeof(struct SceneCullMesh), 16);
    s->lod_rows    = alloc_slice(s, (VkDeviceSize)s->max_lod_rows * sizeof(struct SceneLodRow), 16);
    s->materials   = alloc_slice(s, (VkDeviceSize)s->max_materials * sizeof(struct SceneGpuMaterial), 16);
    s->cull_rows   = alloc_slice(s, (VkDeviceSize)s->max_instances * sizeof(struct SceneCullRow), 16);

    /* Counters are read back on the CPU, so their host-visible copy has to be
       host-visible: VMA_MEMORY_USAGE_CPU_ONLY is explicitly not mappable, and
       reading it through .mapping is undefined. */
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        s->counters[i] = alloc_slice(s, sizeof(uint32_t) * SCENE_COUNTERS, 16);
        if (!create_buffer(vk, sizeof(uint32_t) * SCENE_COUNTERS, BUF_XFER_DST,
                           VMA_MEMORY_USAGE_CPU_TO_GPU, &s->counters_host[i]))
            log_error("[scene] counters host buffer failed");
    }

    /* cull_args has exactly one writer: scene_frame. Seeding it here as well
       would put two vkCmdCopyBuffer on the same 16 bytes in the first frame's
       command buffer (the seed, then the reseed this flag forces), with no
       barrier between them — a WRITE_AFTER_WRITE the validation layer flags. */

    /* scene_create */

    /* One arena backs every persistent CPU table below. Capacity is the exact
       sum of their sizes plus per-allocation alignment slack; mu_arena_alloc
       does not zero, so each table is memset after allocation. */
    {
        uint64_t b = 0;
        b += (uint64_t)s->max_instances * sizeof(SceneCpuInstance);        /* cpu_instances */
        b += (uint64_t)s->max_instances * sizeof(struct SceneInstance);    /* instance_upload_scratch */
        b += (uint64_t)s->max_instances * sizeof(struct SceneCullRow) * 2; /* candidate_upload_scratch + cpu_rows */
        b += (uint64_t)s->max_instances * sizeof(uint32_t) * 4;      /* dirty + static_dirty + row_of + bloom_dense */
        b += (uint64_t)s->max_instances * sizeof(float);             /* bloom_strength */
        b += (uint64_t)s->max_instances * sizeof(float);             /* wiggle_amp */
        b += (uint64_t)s->max_instances * sizeof(float);             /* squash_v */
        b += (uint64_t)s->max_instances * sizeof(float);             /* stretch_v */
        b += (uint64_t)s->max_instances * sizeof(uint32_t);          /* face_id */
        b += (uint64_t)s->max_instances * sizeof(uint32_t);          /* compact_dst */
        b += (uint64_t)s->max_meshes * sizeof(struct SceneCullMesh); /* cpu_cull */
        b += (uint64_t)s->max_meshes * SCENE_MAX_LODS * sizeof(struct SceneGroupShade);
        b += (uint64_t)s->max_meshes * SCENE_MAX_LODS * sizeof(uint32_t);
        b += (uint64_t)s->max_meshes * SCENE_MAX_LODS * sizeof(struct SceneGpuDraw);
        b += (uint64_t)s->max_lod_rows * sizeof(struct SceneLodRow);
        b += (uint64_t)s->max_materials * sizeof(struct SceneGpuMaterial);
        b += 512 + 16; /* alignment slack for the allocations above */
        s->arena.base     = malloc((size_t)b);
        s->arena.capacity = (uint32_t)b;
        s->arena.used     = 0;
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        s->cull_args_dirty[i] = true;
    /* staged_* grow via realloc during asset load, so they stay heap-backed. */
    s->staged_vertex_cap = 4096;
    s->staged_vertices   = malloc((size_t)s->staged_vertex_cap * sizeof(struct ScenePackedVertex));
    s->staged_index_cap  = 8192;
    s->staged_indices    = malloc((size_t)s->staged_index_cap * sizeof(uint16_t));

    s->cpu_instances = mu_arena_alloc(&s->arena, (uint32_t)(s->max_instances * sizeof(SceneCpuInstance)), 16);
    s->instance_upload_scratch =
        mu_arena_alloc(&s->arena, (uint32_t)(s->max_instances * sizeof(struct SceneInstance)), 16);
    s->candidate_upload_scratch =
        mu_arena_alloc(&s->arena, (uint32_t)(s->max_instances * sizeof(struct SceneCullRow)), 16);
    s->dirty_slots        = mu_arena_alloc(&s->arena, s->max_instances * sizeof(uint32_t), 16);
    s->static_dirty_slots = mu_arena_alloc(&s->arena, s->max_instances * sizeof(uint32_t), 16);
    s->compact_dst        = mu_arena_alloc(&s->arena, s->max_instances * sizeof(uint32_t), 16);
    memset(s->cpu_instances, 0, (size_t)s->max_instances * sizeof(SceneCpuInstance));
    memset(s->instance_upload_scratch, 0, (size_t)s->max_instances * sizeof(struct SceneInstance));
    memset(s->candidate_upload_scratch, 0, (size_t)s->max_instances * sizeof(struct SceneCullRow));
    memset(s->dirty_slots, 0, (size_t)s->max_instances * sizeof(uint32_t));
    memset(s->static_dirty_slots, 0, (size_t)s->max_instances * sizeof(uint32_t));

    s->row_of = mu_arena_alloc(&s->arena, s->max_instances * sizeof(uint32_t), 16);
    for (uint32_t i = 0; i < s->max_instances; ++i)
        s->row_of[i] = UINT32_MAX;

    s->bloom_strength = mu_arena_alloc(&s->arena, s->max_instances * sizeof(float), 16);
    s->bloom_dense    = mu_arena_alloc(&s->arena, s->max_instances * sizeof(uint32_t), 16);
    memset(s->bloom_strength, 0, (size_t)s->max_instances * sizeof(float));
    memset(&s->bloom_bits, 0, sizeof(s->bloom_bits));
    mu_bitset_resize_words(&s->bloom_bits, (s->max_instances + 63u) / 64u);
    s->bloom_table = alloc_slice(s, (VkDeviceSize)s->max_instances * sizeof(float), 16);

    s->wiggle_amp   = mu_arena_alloc(&s->arena, s->max_instances * sizeof(float), 16);
    s->wiggle_dirty = false;
    memset(s->wiggle_amp, 0, (size_t)s->max_instances * sizeof(float));
    s->wiggle_table = alloc_slice(s, (VkDeviceSize)s->max_instances * sizeof(float), 16);

    s->squash_v     = mu_arena_alloc(&s->arena, s->max_instances * sizeof(float), 16);
    s->squash_dirty = false;
    for (uint32_t i = 0; i < s->max_instances; ++i)
        s->squash_v[i] = 1.0f; /* rest shape is the default, not 0 */
    s->squash_table = alloc_slice(s, (VkDeviceSize)s->max_instances * sizeof(float), 16);

    s->stretch_v     = mu_arena_alloc(&s->arena, s->max_instances * sizeof(float), 16);
    s->stretch_dirty = false;
    for (uint32_t i = 0; i < s->max_instances; ++i)
        s->stretch_v[i] = 1.0f; /* rest shape is the default, not 0 */
    s->stretch_table = alloc_slice(s, (VkDeviceSize)s->max_instances * sizeof(float), 16);

    s->face_id     = mu_arena_alloc(&s->arena, s->max_instances * sizeof(uint32_t), 16);
    s->face_dirty  = false;
    memset(s->face_id, 0, (size_t)s->max_instances * sizeof(uint32_t));
    s->face_table = alloc_slice(s, (VkDeviceSize)s->max_instances * sizeof(uint32_t), 16);

    s->cpu_cull = mu_arena_alloc(&s->arena, s->max_meshes * sizeof(struct SceneCullMesh), 16);
    s->cpu_group_shade =
        mu_arena_alloc(&s->arena, (uint32_t)(s->max_meshes * SCENE_MAX_LODS * sizeof(struct SceneGroupShade)), 16);
    s->cpu_rung_vbase = mu_arena_alloc(&s->arena, (uint32_t)(s->max_meshes * SCENE_MAX_LODS * sizeof(uint32_t)), 16);
    s->cpu_group_static =
        mu_arena_alloc(&s->arena, (uint32_t)(s->max_meshes * SCENE_MAX_LODS * sizeof(struct SceneGpuDraw)), 16);
    s->cpu_lod  = mu_arena_alloc(&s->arena, s->max_lod_rows * sizeof(struct SceneLodRow), 16);
    s->cpu_mat  = mu_arena_alloc(&s->arena, s->max_materials * sizeof(struct SceneGpuMaterial), 16);
    s->cpu_rows = mu_arena_alloc(&s->arena, s->max_instances * sizeof(struct SceneCullRow), 16);
    memset(s->cpu_cull, 0, (size_t)s->max_meshes * sizeof(struct SceneCullMesh));
    memset(s->cpu_group_shade, 0, (size_t)s->max_meshes * SCENE_MAX_LODS * sizeof(struct SceneGroupShade));
    memset(s->cpu_rung_vbase, 0, (size_t)s->max_meshes * SCENE_MAX_LODS * sizeof(uint32_t));
    memset(s->cpu_group_static, 0, (size_t)s->max_meshes * SCENE_MAX_LODS * sizeof(struct SceneGpuDraw));
    memset(s->cpu_lod, 0, (size_t)s->max_lod_rows * sizeof(struct SceneLodRow));
    memset(s->cpu_mat, 0, (size_t)s->max_materials * sizeof(struct SceneGpuMaterial));
    memset(s->cpu_rows, 0, (size_t)s->max_instances * sizeof(struct SceneCullRow));

    s->cs_cull        = pipeline_create_compute(vk, "compiledshaders/cull.cs_cull.comp.spv");
    s->cs_count       = pipeline_create_compute(vk, "compiledshaders/compact.cs_count.comp.spv");
    s->cs_scan_block  = pipeline_create_compute(vk, "compiledshaders/compact.cs_scan_block.comp.spv");
    s->cs_scan_blocks = pipeline_create_compute(vk, "compiledshaders/compact.cs_scan_blocks.comp.spv");
    s->cs_emit        = pipeline_create_compute(vk, "compiledshaders/compact.cs_emit.comp.spv");
    s->cs_scatter     = pipeline_create_compute(vk, "compiledshaders/compact.cs_scatter.comp.spv");
    s->cs_sort_clear = pipeline_create_compute(vk, "compiledshaders/sort.cs_sort_clear.comp.spv");
    s->cs_sort_hist   = pipeline_create_compute(vk, "compiledshaders/sort.cs_sort_hist.comp.spv");
    s->cs_sort_scan   = pipeline_create_compute(vk, "compiledshaders/sort.cs_sort_scan.comp.spv");
    s->cs_sort_scatter = pipeline_create_compute(vk, "compiledshaders/sort.cs_sort_scatter.comp.spv");
    s->cs_slime_sim     = pipeline_create_compute(vk, "compiledshaders/slime_sim.comp.spv");
    s->cs_slime_grid    = pipeline_create_compute(vk, "compiledshaders/slime_grid.comp.spv");
    s->cs_slime_resolve = pipeline_create_compute(vk, "compiledshaders/slime_resolve.comp.spv");

    log_info("[scene] created: max_instances=%u (dynamic<%u) max_meshes=%u max_survivors=%u", s->max_instances,
             s->dynamic_capacity, s->max_meshes, s->max_survivors);
    return s;
}

VkBackend *scene_vk(const Scene *s) { return s ? s->vk : NULL; }

void scene_destroy(Scene *s) {
    if (!s)
        return;
    VkBackend *vk = s->vk;
    wait_idle(vk);

    buffer_pool_free(s->instances);
    buffer_pool_free(s->cull_meshes);
    buffer_pool_free(s->lod_rows);
    buffer_pool_free(s->materials);
    buffer_pool_free(s->cull_rows);
    buffer_pool_free(s->bloom_table);
    buffer_pool_free(s->wiggle_table);
    buffer_pool_free(s->squash_table);
    buffer_pool_free(s->stretch_table);
    buffer_pool_free(s->face_table);
    buffer_pool_free(s->group_static);
    buffer_pool_free(s->group_shade);
    buffer_pool_free(s->vertex_arena);
    buffer_pool_free(s->index_arena);
    buffer_pool_free(s->slime_state[0]);
    buffer_pool_free(s->slime_state[1]);
    buffer_pool_free(s->slime_grid_count);
    buffer_pool_free(s->slime_grid_items);
    buffer_pool_free(s->slime_obs_all);
    buffer_pool_free(s->slime_obs_off);
    buffer_pool_free(s->slime_obs_items);
    free(s->slime_obs_init);
    free(s->slime_obs_cpu_off);
    free(s->slime_obs_cpu_items);
    free(s->slime_obs_cpu_cursor);
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        buffer_pool_free(s->counters[i]);
        destroy_buffer(vk, &s->counters_host[i]);
    }
    forEach(v, SCENE_MAX_VIEWS) forEach(i, MAX_FRAMES_IN_FLIGHT) {
        SceneViewFrame *vf = &s->view_frame[v][i];
        buffer_pool_free(vf->survivors);
        buffer_pool_free(vf->survivor_count);
        buffer_pool_free(vf->group_count);
        buffer_pool_free(vf->vis_base);
        buffer_pool_free(vf->cmd_index);
        buffer_pool_free(vf->cursor);
        buffer_pool_free(vf->vsrows);
        buffer_pool_free(vf->draws);
        buffer_pool_free(vf->draw_count);
        buffer_pool_free(vf->scan_aux);
        buffer_pool_free(vf->cull_args);
        buffer_pool_free(vf->surv_sorted);
        buffer_pool_free(vf->sort_aux);
        buffer_pool_free(vf->group_base);
    }
    free(s->staged_vertices);
    free(s->staged_indices);
    free(s->slime_state_init);
    free(s->bloom_bits.array);
    free(s->arena.base);
    free(s);
}

/* ============================================================ assets */

uint32_t scene_material_add(Scene *s, const struct SceneGpuMaterial *material) {
    if (!s || !material || s->material_count >= s->max_materials)
        return UINT32_MAX;
    uint32_t id    = s->material_count++;
    s->cpu_mat[id] = *material;
    return id;
}

uint32_t scene_mesh_add(Scene *s, const SceneMeshDesc *desc) {
    if (!s || s->mesh_count >= s->max_meshes)
        return UINT32_MAX;
    uint32_t mesh = s->mesh_count++;

    /* Every mesh carries the same number of rungs, because a group id is
       mesh * lod_count + lod against one scene-wide lod_count. A mesh whose
       asset supplied fewer rungs repeats its coarsest one: the ladder walk
       lands on a duplicate rung, which is the geometry it would have drawn
       anyway, and the group arithmetic stays a multiply. */
    uint32_t have  = desc->lod_count ? desc->lod_count : 1u;
    uint32_t rungs = have > s->lod_count ? have : s->lod_count;
    if (rungs > SCENE_MAX_LODS || s->lod_row_count + rungs > s->max_lod_rows)
        return UINT32_MAX;
    s->lod_count = rungs;

    /* append vertices */
    if (s->staged_vertex_count + desc->vertex_count > s->staged_vertex_cap) {
        s->staged_vertex_cap = (s->staged_vertex_count + desc->vertex_count) * 2;
        s->staged_vertices =
            realloc(s->staged_vertices, (size_t)s->staged_vertex_cap * sizeof(struct ScenePackedVertex));
    }
    uint32_t vbase = s->staged_vertex_count;
    memcpy(s->staged_vertices + vbase, desc->vertices, (size_t)desc->vertex_count * sizeof(struct ScenePackedVertex));
    s->staged_vertex_count += desc->vertex_count;

    if (s->staged_index_count + desc->index_count > s->staged_index_cap) {
        s->staged_index_cap = (s->staged_index_count + desc->index_count) * 2;
        s->staged_indices   = realloc(s->staged_indices, (size_t)s->staged_index_cap * sizeof(uint16_t));
    }
    uint32_t ibase = s->staged_index_count;
    memcpy(s->staged_indices + ibase, desc->indices, (size_t)desc->index_count * sizeof(uint16_t));
    s->staged_index_count += desc->index_count;

    /* cull mesh (local sphere packed half4) */
    struct SceneCullMesh cm = {0};
    cm.lod_first            = (uint16_t)s->lod_row_count;
    cm.lod_count            = (uint16_t)rungs;
    cm.local_sphere_xy      = pack_half2(desc->local_center[0], desc->local_center[1]);
    cm.local_sphere_zr      = pack_half2(desc->local_center[2], desc->local_radius);
    s->cpu_cull[mesh]       = cm;

    /* One ladder rung, one group template and one shade row per rung. The shade
       row's device address is baked at upload, once the arena exists, so the
       rung's vertex base is only a number until then. */
    for (uint32_t k = 0; k < rungs; ++k) {
        const SceneLodDesc *rung = &desc->lods[k < have ? k : have - 1u];

        s->cpu_lod[s->lod_row_count].error       = rung->error;
        s->cpu_lod[s->lod_row_count].first_index = ibase + rung->first_index;
        s->cpu_lod[s->lod_row_count].index_count = rung->index_count;
        s->lod_row_count++;

        uint32_t g                            = mesh * SCENE_MAX_LODS + k;
        s->cpu_group_static[g].index_count    = rung->index_count;
        s->cpu_group_static[g].first_index    = ibase + rung->first_index;
        s->cpu_group_static[g].instance_count = 0;
        s->cpu_group_static[g].first_instance = 0;
        s->cpu_group_static[g].vertex_offset  = 0; /* the base rides in the shade row */
        s->cpu_group_static[g].mesh_lod       = mesh;

        s->cpu_group_shade[g].material               = desc->material;
        s->cpu_group_shade[g].pad                    = 0;
        s->cpu_rung_vbase[mesh * SCENE_MAX_LODS + k] = vbase + rung->first_vertex;
    }

    log_info("[scene] mesh %u: %u verts %u indices, %u rungs (vbase %u ibase %u)", mesh, desc->vertex_count,
             desc->index_count, rungs, vbase, ibase);
    return mesh;
}

/* One insert path; the region is decided by the caller, not stored per row. */
static uint32_t instance_insert(Scene *s, const SceneInstanceDesc *desc, bool is_static) {
    if (!s || s->instance_count >= s->max_instances)
        return UINT32_MAX;

    /* Each region has its own fixed ceiling; crossing into the other is never
       allowed, because the split is what keeps the dynamic prefix dense. */
    uint32_t slot;
    if (is_static) {
        if (s->static_count >= s->static_capacity)
            return UINT32_MAX;
        slot = s->dynamic_capacity + s->static_count;
    } else {
        if (s->dynamic_count >= s->dynamic_capacity)
            return UINT32_MAX;
        slot = s->dynamic_count;
    }

    pack_instance(&s->cpu_instances[slot].gpu, desc);
    s->cpu_instances[slot].mesh  = desc->mesh;
    s->cpu_instances[slot].alive = 1;
    s->bloom_strength[slot]      = 0.0f;
    mu_bitset_reset(&s->bloom_bits, slot);
    s->cpu_rows[s->candidate_count].slot = slot;
    s->cpu_rows[s->candidate_count].mesh = (uint16_t)desc->mesh;
    s->row_of[slot]                      = s->candidate_count;
    s->candidate_count                   = s->instance_count + 1;
    s->candidates_dirty                  = true;
    if (is_static) {
        s->static_count++;
        s->uploaded_dirty = true; /* static region needs a re-upload */
    } else {
        s->dynamic_count++;
        s->dirty_slots[s->dirty_count++] = slot;
    }
    s->instance_count++;
    /* Roster grew: the seeded dispatch args may now be short and the candidate
       rows need re-uploading. Recorded here and applied at the next frame
       boundary, because writing a GPU slice needs the command buffer and
       inserting an instance does not have one. */

    /* whenever a roster change is recorded (instance_insert, instance_set,
       instance_destroy, scene_compact_slots) */
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        s->cull_args_dirty[i] = true;

    return slot;
}

uint32_t scene_instance_create(Scene *s, const SceneInstanceDesc *desc) {
    if (!s || !desc)
        return UINT32_MAX;
    return instance_insert(s, desc, false);
}

uint32_t scene_instance_create_static(Scene *s, const SceneInstanceDesc *desc) {
    if (!s || !desc)
        return UINT32_MAX;
    return instance_insert(s, desc, true);
}

void scene_instance_set(Scene *s, uint32_t slot, const SceneInstanceDesc *desc) {
    if (!s || !desc || slot >= s->max_instances || !s->cpu_instances[slot].alive)
        return;
    pack_instance(&s->cpu_instances[slot].gpu, desc);
    s->cpu_instances[slot].mesh       = desc->mesh;
    s->cpu_rows[s->row_of[slot]].mesh = (uint16_t)desc->mesh;
    s->candidates_dirty               = true;
    if (slot >= s->dynamic_capacity) {
        /* static: re-upload just this row rather than the whole region */
        s->static_dirty_slots[s->static_dirty_count++] = slot;
        s->uploaded_dirty                              = true;
    } else {
        s->dirty_slots[s->dirty_count++] = slot;
    }
}

bool scene_instance_alive(const Scene *s, uint32_t slot) {
    return s && slot < s->max_instances && s->cpu_instances[slot].alive;
}

void scene_set_instance_bloom(Scene *s, uint32_t slot, float strength) {
    if (!s || slot >= s->max_instances || !s->cpu_instances[slot].alive)
        return;
    if (strength < 0.0f)
        strength = 0.0f;
    s->bloom_strength[slot] = strength;
    mu_bitset_set_bit(&s->bloom_bits, slot, strength > 0.0f);
    s->bloom_dirty = true;
}

float scene_instance_bloom(const Scene *s, uint32_t slot) {
    if (!s || slot >= s->max_instances)
        return 0.0f;
    return s->bloom_strength[slot];
}

/* Per-instance jiggle amplitude. Set once at spawn (before the first upload);
   the VS displaces the vertex by amp * sin(time*speed + slot*phi) * height.
   0 keeps the instance rigid, which is the default for every non-slime prop. */
void scene_set_instance_wiggle(Scene *s, uint32_t slot, float amp) {
    if (!s || slot >= s->max_instances || !s->cpu_instances[slot].alive)
        return;
    if (amp < 0.0f)
        amp = 0.0f;
    if (s->wiggle_amp[slot] != amp) {
        s->wiggle_amp[slot] = amp;
        s->wiggle_dirty     = true;
    }
}

float scene_instance_wiggle(const Scene *s, uint32_t slot) {
    if (!s || slot >= s->max_instances)
        return 0.0f;
    return s->wiggle_amp[slot];
}

/* Per-instance hop squash-and-stretch. Clamped to a sane cartoon range; a
   change marks the table dirty so the next scene_frame re-uploads it whole. */
void scene_set_instance_squash(Scene *s, uint32_t slot, float v) {
    if (!s || slot >= s->max_instances || !s->cpu_instances[slot].alive)
        return;
    if (v < 0.5f)
        v = 0.5f;
    if (v > 1.6f)
        v = 1.6f;
    if (s->squash_v[slot] != v) {
        s->squash_v[slot] = v;
        s->squash_dirty   = true;
    }
}

float scene_instance_squash(const Scene *s, uint32_t slot) {
    if (!s || slot >= s->max_instances)
        return 1.0f;
    return s->squash_v[slot];
}

/* Per-instance slime face tile. No clamping: the atlas has 40 slots and the
   FS wraps with the grid arithmetic, so an out-of-range id still samples
   defined texels rather than faulting. */
void scene_set_instance_face(Scene *s, uint32_t slot, uint32_t face) {
    if (!s || slot >= s->max_instances || !s->cpu_instances[slot].alive)
        return;
    if (s->face_id[slot] != face) {
        s->face_id[slot] = face;
        s->face_dirty    = true;
    }
}

uint32_t scene_instance_face(const Scene *s, uint32_t slot) {
    if (!s || slot >= s->max_instances)
        return 0u;
    return s->face_id[slot];
}

/* Slime face atlas: count source PNGs resampled to SCENE_FACE_TILE and packed
   row-major into a fixed COLSxROWS grid, then uploaded as one linear-UNORM
   texture. The grid is fixed even when count < COLS*ROWS so the shader's tile
   arithmetic never depends on how many faces were staged. */
TextureID scene_face_atlas_load(Scene *s, const char *const *paths, uint32_t count, char *err, uint32_t err_cap) {
    if (!s || !paths || count == 0 || count > SCENE_FACE_COLS * SCENE_FACE_ROWS) {
        snprintf(err, err_cap, "face atlas: need 1..%u paths", SCENE_FACE_COLS * SCENE_FACE_ROWS);
        return UINT32_MAX;
    }
    const uint32_t T = SCENE_FACE_TILE, C = SCENE_FACE_COLS, R = SCENE_FACE_ROWS;
    const uint32_t W = C * T, H = R * T;

    uint8_t *atlas = (uint8_t *)calloc((size_t)W * H, 4u);
    if (!atlas) {
        snprintf(err, err_cap, "face atlas: out of memory");
        return UINT32_MAX;
    }
    uint8_t tile[SCENE_FACE_TILE * SCENE_FACE_TILE * 4u];

    for (uint32_t i = 0; i < count; ++i) {
        int            w, h, c;
        unsigned char *px = stbi_load(paths[i], &w, &h, &c, 4);
        if (!px) {
            snprintf(err, err_cap, "face atlas: cannot decode %s", paths[i]);
            free(atlas);
            return UINT32_MAX;
        }
        /* SDF edges survive bilinear resampling: the 0.5 isolevel stays where
           it was, only the gradient steepness changes with scale. Sources must
           use standard polarity (0.5 edge, 1.0 inside) on every channel. */
        stbir_resize_uint8_linear(px, w, h, 0, tile, (int)T, (int)T, 0, STBIR_RGBA);
        stbi_image_free(px);
        uint32_t tx = (i % C) * T, ty = (i / C) * T;
        for (uint32_t y = 0; y < T; ++y)
            memcpy(atlas + ((size_t)(ty + y) * W + tx) * 4u, tile + (size_t)y * T * 4u, (size_t)T * 4u);
    }

    VkBackend *vk  = s->vk;
    TextureID  tex = create_texture(vk, &(TextureCreateDesc){.width      = W,
                                                            .height     = H,
                                                            .depth      = 1,
                                                            .layers     = 1,
                                                            .mip_count  = 1,
                                                            .format     = VK_FORMAT_R8G8B8A8_UNORM,
                                                            .usage      = IMG_SAMPLED | IMG_XFER_DST,
                                                            .debug_name = "slime_face_atlas"});
    if (tex == UINT32_MAX) {
        snprintf(err, err_cap, "face atlas: texture pool exhausted");
        free(atlas);
        return UINT32_MAX;
    }
    if (!texture_upload(vk, tex, 0, 0, (VkOffset3D){0, 0, 0}, (VkExtent3D){W, H, 1},
                        (ByteSpan){atlas, W * H * 4u})) {
        snprintf(err, err_cap, "face atlas: upload failed");
        destroy_texture(vk, tex);
        free(atlas);
        return UINT32_MAX;
    }
    free(atlas);
    log_info("[scene] face atlas: %u tiles in %ux%u", count, W, H);
    return tex;
}

/* Removal is existence, not state: the candidate row goes away, so no hot loop
   ever asks whether this instance is alive. The slot is retired and becomes a
   hole; holes are reclaimed by scene_compact_slots() at load boundaries.

   Row indices and slot indices are separate spaces the moment static instances
   exist, so the swap-remove goes through row_of[slot] and the moved row's owner
   is remapped. */
void scene_instance_destroy(Scene *s, uint32_t slot) {
    if (!s || slot >= s->max_instances || !s->cpu_instances[slot].alive)
        return;

    uint32_t removed_row = s->row_of[slot];
    uint32_t last_row    = s->candidate_count - 1;
    if (removed_row != last_row) {
        s->cpu_rows[removed_row]                 = s->cpu_rows[last_row];
        s->row_of[s->cpu_rows[removed_row].slot] = removed_row;
    }
    s->candidate_count = last_row;
    s->row_of[slot]    = UINT32_MAX;

    bool     is_static = slot >= s->dynamic_capacity;
    uint32_t last_slot = is_static ? s->dynamic_capacity + s->static_count - 1 : s->dynamic_count - 1;
    if (slot != last_slot) {
        uint32_t moved_row      = s->row_of[last_slot];
        s->cpu_instances[slot]  = s->cpu_instances[last_slot];
        s->bloom_strength[slot] = s->bloom_strength[last_slot];
        s->wiggle_amp[slot]     = s->wiggle_amp[last_slot];
        s->squash_v[slot]       = s->squash_v[last_slot];
        s->stretch_v[slot]      = s->stretch_v[last_slot];
        s->face_id[slot]        = s->face_id[last_slot];
        mu_bitset_set_bit(&s->bloom_bits, slot, mu_bitset_test(&s->bloom_bits, last_slot));
        s->row_of[slot]             = moved_row;
        s->cpu_rows[moved_row].slot = slot;
    }
    memset(&s->cpu_instances[last_slot], 0, sizeof(s->cpu_instances[last_slot]));
    s->bloom_strength[last_slot] = 0.0f;
    s->wiggle_amp[last_slot]     = 0.0f;
    s->squash_v[last_slot]       = 1.0f;
    s->stretch_v[last_slot]      = 1.0f;
    s->face_id[last_slot]        = 0u;
    mu_bitset_reset(&s->bloom_bits, last_slot);
    s->bloom_dirty = true;
    s->instance_count--;
    s->candidates_dirty = true;

    if (is_static) {
        s->static_count--;
        s->uploaded_dirty = true;
    } else {
        s->dynamic_count--;
    }
}

void scene_compact_slots(Scene *s) {
    if (!s)
        return;
    /* O(instances) at load time only. Rebuilds the dense slot space and the
       candidate rows from the surviving live records, so holes never accumulate
       across a session.

       This must NOT compact in place while scanning forward. The survivor is
       always at a lower index than the slot being filled, so a forward copy
       writes into slots the loop has not reached yet and then reads them back as
       if they were originals — which duplicates live instances and leaves stale
       ones behind. (That bug did exactly this: 9 of 18 survivors ended up
       stored twice, which read as cubes overlapping.)

       Two passes instead: one counts and marks, one moves. Still O(n), no
       allocation, and the move pass only ever writes below the read cursor. */
    uint32_t dyn = 0, sta = 0;
    for (uint32_t i = 0; i < s->max_instances; ++i) {
        if (!s->cpu_instances[i].alive)
            continue;
        if (i >= s->dynamic_capacity)
            sta++;
        else
            dyn++;
    }

    /* dst is fixed-capacity scratch carved from the scene arena, reused every
       compaction; load-boundary only, never per event, so no allocation. */
    uint32_t *dst = s->compact_dst;

    uint32_t d = 0, t = 0, row = 0;
    for (uint32_t i = 0; i < s->max_instances; ++i) {
        if (!s->cpu_instances[i].alive)
            continue;
        dst[i] = (i >= s->dynamic_capacity) ? (s->dynamic_capacity + t) : d;
        if (i >= s->dynamic_capacity)
            t++;
        else
            d++;
    }

    /* Second pass: safe to move now, because every destination is known before
       any copy happens, so no copy can clobber a record the move pass still
       needs. dst is monotonically non-decreasing, so dst[i] <= i always. */
    for (uint32_t i = 0; i < s->max_instances; ++i) {
        if (!s->cpu_instances[i].alive)
            continue;
        uint32_t to = dst[i];
        if (to != i) {
            s->cpu_instances[to]      = s->cpu_instances[i];
            s->bloom_strength[to]     = s->bloom_strength[i];
            s->wiggle_amp[to]         = s->wiggle_amp[i];
            s->squash_v[to]           = s->squash_v[i];
            s->stretch_v[to]          = s->stretch_v[i];
            s->face_id[to]            = s->face_id[i];
            s->cpu_instances[i].alive = 0;
            s->row_of[i]              = UINT32_MAX;
        }
        s->cpu_rows[row].slot = to;
        s->cpu_rows[row].mesh = (uint16_t)s->cpu_instances[to].mesh;
        s->row_of[to]         = row;
        row++;
    }

    /* Slots moved: rebuild the bitset from strengths so membership follows the
       new slot layout. Dense is rebuilt on the next bloom upload. */
    mu_bitset_clear(&s->bloom_bits);
    for (uint32_t i = 0; i < s->max_instances; ++i) {
        if (s->cpu_instances[i].alive && s->bloom_strength[i] > 0.0f)
            mu_bitset_set(&s->bloom_bits, i);
        else if (!s->cpu_instances[i].alive) {
            s->bloom_strength[i] = 0.0f;
            s->wiggle_amp[i]     = 0.0f;
            s->squash_v[i]       = 1.0f;
            s->stretch_v[i]      = 1.0f;
            s->face_id[i]        = 0u;
        }
    }
    s->bloom_dirty  = true;
    s->wiggle_dirty = true;
    s->squash_dirty = true;
    s->stretch_dirty = true;
    s->face_dirty   = true;

    s->dynamic_count      = dyn;
    s->static_count       = sta;
    s->instance_count     = dyn + sta;
    s->candidate_count    = row;
    s->dirty_count        = 0;
    s->static_dirty_count = 0;
    s->uploaded_dirty     = true;
    s->candidates_dirty   = true;
}

/* Retires every static instance in one sweep, for a streamed world that rebuilds
   its window wholesale. Walking the tail down destroys the last static slot each
   time, so the O(1) swap-remove never has to move a row: it is a plain O(static)
   teardown with no scratch and no compaction step. The freed slots are reused by
   the next scene_instance_create_static. */
void scene_static_clear(Scene *s) {
    if (!s)
        return;
    for (uint32_t i = s->static_count; i-- > 0u;)
        scene_instance_destroy(s, s->dynamic_capacity + i);
}

void scene_view_set(Scene *s, uint32_t view, const SceneViewDesc *vd) {    if (!s || view >= SCENE_MAX_VIEWS)
        return;
    memcpy(s->views[view].clip_rows, vd->clip_rows, sizeof(vd->clip_rows));
    memcpy(s->views[view].camera_pos, vd->camera_pos, sizeof(vd->camera_pos));
    s->views[view].lod_target = vd->lod_target > 0.0f ? vd->lod_target : 1.0f;
    s->views[view].near_z     = vd->near_z;
    s->views[view].far_z      = vd->far_z;
    if (view + 1 > s->view_count)
        s->view_count = view + 1;
}

void scene_set_clear(Scene *s, const float rgba[4]) { memcpy(s->clear, rgba, sizeof(s->clear)); }

/* ============================================================ upload (T0) */

/* Re-buckets the gathered obstacle discs into a camera-relative uniform grid
   (CSR: offsets + items) and uploads it. Runs on the CPU whenever the streamed
   window changes; the resolve pass only ever reads the result. Discs outside
   the grid are dropped — a slime is never outside the hop grid, which shares
   the same extent, so it can never need an obstacle that falls outside. */
static void slime_obstacles_rebuild(Scene *s, VkCommandBuffer cmd, uint32_t view) {
    VkBackend *vk = s->vk;
    uint32_t   nx = s->slime_grid_nx;
    uint32_t   n  = s->slime_obs_count;
    if (n > s->slime_obs_cap)
        n = s->slime_obs_cap;

    if (n == 0u || nx == 0u) {
        s->slime_obs_nx    = s->slime_obs_nz = 0u;
        s->slime_obs_dirty = false;
        return;
    }

    float    cs     = s->slime_cell_size > 0.0f ? s->slime_cell_size : 3.0f;
    float    extent = (float)nx * cs;
    float    min_x  = s->views[view].camera_pos[0] - extent * 0.5f;
    float    min_z  = s->views[view].camera_pos[2] - extent * 0.5f;
    uint32_t cells  = nx * nx;

    uint32_t *off    = s->slime_obs_cpu_off;
    uint32_t *items  = s->slime_obs_cpu_items;
    uint32_t *cursor = s->slime_obs_cpu_cursor;

    memset(off, 0, (size_t)(cells + 1u) * sizeof(uint32_t));
    const struct SlimeObstacle *o = s->slime_obs_init;
    for (uint32_t i = 0; i < n; ++i) {
        int cx = (int)((o[i].x - min_x) / cs);
        int cz = (int)((o[i].z - min_z) / cs);
        if (cx < 0 || cz < 0 || cx >= (int)nx || cz >= (int)nx)
            continue;
        off[(uint32_t)cz * nx + (uint32_t)cx + 1u]++;
    }
    for (uint32_t c = 0; c < cells; ++c)
        off[c + 1u] += off[c];

    memcpy(cursor, off, (size_t)cells * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; ++i) {
        int cx = (int)((o[i].x - min_x) / cs);
        int cz = (int)((o[i].z - min_z) / cs);
        if (cx < 0 || cz < 0 || cx >= (int)nx || cz >= (int)nx)
            continue;
        items[cursor[(uint32_t)cz * nx + (uint32_t)cx]++] = i;
    }

    renderer_upload_buffer_to_slice(vk, cmd, s->slime_obs_all,
                                    (ByteSpan){s->slime_obs_init, n * sizeof(struct SlimeObstacle)});
    renderer_upload_buffer_to_slice(vk, cmd, s->slime_obs_off,
                                    (ByteSpan){off, (cells + 1u) * sizeof(uint32_t)});
    renderer_upload_buffer_to_slice(vk, cmd, s->slime_obs_items,
                                    (ByteSpan){items, n * sizeof(uint32_t)});

    s->slime_obs_nx    = nx;
    s->slime_obs_nz    = nx;
    s->slime_obs_min_x = min_x;
    s->slime_obs_min_z = min_z;
    s->slime_obs_dirty = false;
}

void scene_slime_obstacles(Scene *s, const struct SlimeObstacle *obstacles, uint32_t count) {
    if (!s)
        return;
    uint32_t n = count;
    if (n > s->slime_obs_cap)
        n = s->slime_obs_cap;
    if (obstacles && n > 0u) {
        free(s->slime_obs_init);
        s->slime_obs_init = (struct SlimeObstacle *)malloc((size_t)n * sizeof(struct SlimeObstacle));
        if (s->slime_obs_init)
            memcpy(s->slime_obs_init, obstacles, (size_t)n * sizeof(struct SlimeObstacle));
    }
    s->slime_obs_count = s->slime_obs_init ? n : 0u;
    /* The next scene_frame re-buckets the grid around the camera and uploads. */
    s->slime_obs_dirty = true;
}

bool scene_slime_enable(Scene *s, uint32_t count, const struct SlimeState *states, float collide_radius,
                        float cell_size) {
    if (!s || count == 0 || !states)
        return false;
    if (count > s->dynamic_capacity / 2u) {
        log_error("[scene] slime count %u exceeds the %u dynamic pairs", count, s->dynamic_capacity / 2u);
        return false;
    }
    if (cell_size <= 0.0f)
        cell_size = 3.0f;

    /* The grid spans the collide annulus plus a one-cell border so a slime on
       the gate's rim still finds its neighbours. Cells are square and the grid
       follows the camera: the crowd is wrapped to stay near the player (see
       slime_sim), so a camera-relative grid is all the collision ever needs and
       the per-frame clear stays small no matter how big the world is. */
    float    extent = 2.0f * (collide_radius + cell_size);
    uint32_t nx     = (uint32_t)ceilf(extent / cell_size);
    if (nx < 1u)
        nx = 1u;
    if (nx > 96u)
        nx = 96u;

    s->slime_count          = count;
    s->slime_collide_radius = collide_radius;
    s->slime_cell_size      = cell_size;
    s->slime_inv_cell       = 1.0f / cell_size;
    s->slime_grid_nx        = nx;
    s->slime_grid_nz        = nx;
    s->slime_grid_cap       = 16u;
    s->slime_parity         = 0u;

    s->slime_state_init = malloc((size_t)count * sizeof(struct SlimeState));
    if (!s->slime_state_init)
        return false;
    memcpy(s->slime_state_init, states, (size_t)count * sizeof(struct SlimeState));

    VkDeviceSize state_bytes = (VkDeviceSize)count * sizeof(struct SlimeState);
    s->slime_state[0]        = alloc_slice(s, state_bytes, 16);
    s->slime_state[1]        = alloc_slice(s, state_bytes, 16);
    s->slime_grid_count      = alloc_slice(s, (VkDeviceSize)nx * nx * sizeof(uint32_t), 16);
    s->slime_grid_items =
        alloc_slice(s, (VkDeviceSize)nx * nx * s->slime_grid_cap * sizeof(uint32_t), 16);
    if (!s->slime_state[0].buffer || !s->slime_state[1].buffer || !s->slime_grid_count.buffer ||
        !s->slime_grid_items.buffer) {
        log_error("[scene] slime buffers failed");
        return false;
    }

    /* Obstacle grid: fixed-capacity slices, rebuilt and re-uploaded whenever the
       streamed world window moves (see slime_obstacles_rebuild). The grid shares
       the hop grid's extent, so a disc is only ever queried inside this range. */
    uint32_t cells = nx * nx;
    s->slime_obs_cap        = SCENE_SLIME_OBS_CAP;
    s->slime_obs_all        = alloc_slice(s, (VkDeviceSize)s->slime_obs_cap * sizeof(struct SlimeObstacle), 16);
    s->slime_obs_off        = alloc_slice(s, ((VkDeviceSize)cells + 1u) * sizeof(uint32_t), 16);
    s->slime_obs_items      = alloc_slice(s, (VkDeviceSize)s->slime_obs_cap * sizeof(uint32_t), 16);
    s->slime_obs_cpu_off    = (uint32_t *)calloc((size_t)cells + 1u, sizeof(uint32_t));
    s->slime_obs_cpu_items  = (uint32_t *)malloc((size_t)s->slime_obs_cap * sizeof(uint32_t));
    s->slime_obs_cpu_cursor = (uint32_t *)malloc((size_t)cells * sizeof(uint32_t));
    if (!s->slime_obs_all.buffer || !s->slime_obs_off.buffer || !s->slime_obs_items.buffer ||
        !s->slime_obs_cpu_off || !s->slime_obs_cpu_items || !s->slime_obs_cpu_cursor) {
        log_error("[scene] slime obstacle buffers failed");
        return false;
    }
    s->slime_obs_dirty = true; /* first scene_frame buckets + uploads */

    s->slime_active = true;
    s->sparkles_per_slime = SCENE_SPARKLES_PER_SLIME;
    {
        const char *sp = getenv("MU_SPARKLES");
        if (sp)
            s->sparkles_per_slime = (uint32_t)strtoul(sp, NULL, 10);
    }
    log_info("[scene] slime sim: %u slimes, grid %ux%u cap %u, radius %.1f, cell %.2f", count, nx, nx,
             s->slime_grid_cap, collide_radius, cell_size);
    return true;
}

bool scene_upload_scene(Scene *s, VkCommandBuffer cmd) {
    if (!s || s->uploaded)
        return s->uploaded;
    VkBackend *vk = s->vk;

    /* vertex / index arenas as contiguous slices with baked addresses */
    if (s->staged_vertex_count == 0 || s->staged_index_count == 0)
        return false;

    BufferSlice varena = alloc_slice(s, (VkDeviceSize)s->staged_vertex_count * sizeof(struct ScenePackedVertex), 16);
    BufferSlice iarena = alloc_slice(s, (VkDeviceSize)s->staged_index_count * sizeof(uint16_t), 2);
    if (!varena.buffer || !iarena.buffer)
        return false;

    renderer_upload_buffer_to_slice(
        vk, cmd, varena, (ByteSpan){s->staged_vertices, s->staged_vertex_count * sizeof(struct ScenePackedVertex)});
    renderer_upload_buffer_to_slice(vk, cmd, iarena,
                                    (ByteSpan){s->staged_indices, s->staged_index_count * sizeof(uint16_t)});

    s->vertex_arena = varena;
    s->index_arena  = iarena;

    /* Groups are packed mesh * lod_count + lod, and each rung's vertex base is
       baked into its shade row now that the arena has an address. The baker
       walks the mesh-major authoring order and writes the group-major order the
       shader indexes, so no consumer ever has to know SCENE_MAX_LODS exists. */
    s->group_count = s->mesh_count * s->lod_count;
    {
        uint64_t vbase_addr = slice_addr(vk, varena);
        for (uint32_t m = 0; m < s->mesh_count; ++m) {
            for (uint32_t k = 0; k < s->lod_count; ++k) {
                uint32_t src           = m * SCENE_MAX_LODS + k;
                uint32_t g             = m * s->lod_count + k;
                s->cpu_group_static[g] = s->cpu_group_static[src];
                s->cpu_group_shade[g]  = s->cpu_group_shade[src];
                s->cpu_group_shade[g].vertex_stream =
                    vbase_addr + (uint64_t)s->cpu_rung_vbase[src] * sizeof(struct ScenePackedVertex);
            }
        }
    }

    /* material table (one default material if none staged) */
    if (s->material_count == 0) {
        s->cpu_mat[0].base_color = 0xFF8070D0u;
        s->cpu_mat[0].texture    = 0xFFFFFFFFu;
        s->cpu_mat[0].flags      = 0;
        s->material_count        = 1;
    }

    renderer_upload_buffer_to_slice(vk, cmd, s->cull_meshes,
                                    (ByteSpan){s->cpu_cull, s->max_meshes * sizeof(struct SceneCullMesh)});
    renderer_upload_buffer_to_slice(vk, cmd, s->lod_rows,
                                    (ByteSpan){s->cpu_lod, s->max_lod_rows * sizeof(struct SceneLodRow)});
    renderer_upload_buffer_to_slice(vk, cmd, s->materials,
                                    (ByteSpan){s->cpu_mat, s->max_materials * sizeof(struct SceneGpuMaterial)});
    renderer_upload_buffer_to_slice(vk, cmd, s->cull_rows,
                                    (ByteSpan){s->cpu_rows, s->max_instances * sizeof(struct SceneCullRow)});
    renderer_upload_buffer_to_slice(vk, cmd, s->bloom_table,
                                    (ByteSpan){s->bloom_strength, (size_t)s->max_instances * sizeof(float)});
    renderer_upload_buffer_to_slice(vk, cmd, s->wiggle_table,
                                    (ByteSpan){s->wiggle_amp, (size_t)s->max_instances * sizeof(float)});
    renderer_upload_buffer_to_slice(vk, cmd, s->squash_table,
                                    (ByteSpan){s->squash_v, (size_t)s->max_instances * sizeof(float)});
    renderer_upload_buffer_to_slice(vk, cmd, s->stretch_table,
                                    (ByteSpan){s->stretch_v, (size_t)s->max_instances * sizeof(float)});
    renderer_upload_buffer_to_slice(vk, cmd, s->face_table,
                                    (ByteSpan){s->face_id, (size_t)s->max_instances * sizeof(uint32_t)});
    s->wiggle_dirty = false;
    s->squash_dirty = false;
    s->stretch_dirty = false;
    s->face_dirty   = false;

    /* Seed both ping-pong state buffers with the same spawn state. From here the
       GPU owns them; the CPU copy is dead weight after this upload. */
    if (s->slime_active && s->slime_state_init) {
        uint32_t bytes = s->slime_count * sizeof(struct SlimeState);
        renderer_upload_buffer_to_slice(vk, cmd, s->slime_state[0], (ByteSpan){s->slime_state_init, bytes});
        renderer_upload_buffer_to_slice(vk, cmd, s->slime_state[1], (ByteSpan){s->slime_state_init, bytes});
        free(s->slime_state_init);
        s->slime_state_init = NULL;
    }

    /* Obstacle discs + their CSR grid are camera-relative and streamed: they are
       bucketed and uploaded from scene_frame whenever the window changes, so
       nothing is done here. */

    /* Whole instance table, once. After this the dynamic prefix is rewritten by
       T1 each frame and the static suffix only when a static row actually
       changes, so a settled scene pays nothing for instance residency. */
    {
        /* Written at each instance's own slot, not packed densely: gpu_inst[] is
           slot-indexed and the static region lives at an offset, so packing would
           put static rows at the wrong indices. Holes are zeroed — a retired slot
           is never referenced, and zero is a defined pose rather than garbage. */
        memset(s->instance_upload_scratch, 0, (size_t)s->max_instances * sizeof(struct SceneInstance));
        for (uint32_t i = 0; i < s->max_instances; ++i) {
            if (s->cpu_instances[i].alive)
                s->instance_upload_scratch[i] = s->cpu_instances[i].gpu;
        }
        renderer_upload_buffer_to_slice(
            vk, cmd, s->instances,
            (ByteSpan){s->instance_upload_scratch, (uint32_t)s->max_instances * sizeof(struct SceneInstance)});
        s->uploaded_dirty = false;
    }

    /* group templates and shade rows, one per (mesh, lod) */
    {
        uint32_t G      = s->group_count ? s->group_count : 1;
        s->group_static = alloc_slice(s, (VkDeviceSize)G * sizeof(struct SceneGpuDraw), 16);
        s->group_shade  = alloc_slice(s, (VkDeviceSize)G * sizeof(struct SceneGroupShade), 16);
        renderer_upload_buffer_to_slice(vk, cmd, s->group_static,
                                        (ByteSpan){s->cpu_group_static, G * sizeof(struct SceneGpuDraw)});
        renderer_upload_buffer_to_slice(vk, cmd, s->group_shade,
                                        (ByteSpan){s->cpu_group_shade, G * sizeof(struct SceneGroupShade)});
    }

    s->dirty_count = s->instance_count; /* upload everything once */

    /* No barrier here on purpose. Every table uploaded above is declared as a
       read by the pass that consumes it (T2 for instances/meshes/rows/lods,
       T3b for group_static, T5 for the vertex and index arenas), and begin_pass
       turns each declaration into a make-visible barrier whose source covers
       TRANSFER. The old code emitted one whole-pool barrier here instead, which
       ordered the same transfers while also implicating every unrelated
       allocation in the pool. */
    s->uploaded = true;
    log_info("[scene] uploaded %u instances, %u meshes, %u groups", s->instance_count, s->mesh_count, s->group_count);
    return true;
}

/* ============================================================ frame (T1..T5) */

static void ensure_pipelines(Scene *s, RenderTarget *color, RenderTarget *depth) {
    if (s->pipelines_ready)
        return;
    s->color_format = color->format;
    s->depth_format = depth->format;

    GraphicsPipelineConfig cfg = pipeline_config_default();
    cfg.vert_path              = "compiledshaders/scene.vert.spv";
    cfg.frag_path              = "compiledshaders/scene.frag.spv";
    /* Current .mua assets include double-sided batches, but this scene has one
       pipeline and does not yet carry batch selection into draw submission. */
    cfg.cull_mode = VK_CULL_MODE_NONE;

    cfg.front_face             = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    cfg.color_attachment_count = 1;
    cfg.color_formats          = &s->color_format;
    cfg.depth_format           = s->depth_format;
//    cfg.polygon_mode=VK_POLYGON_MODE_LINE ;
    /* reverse-Z: near is 1.0 and the far limit is 0.0, so nearer geometry has
       the greater depth. Any Hi-Z pyramid built from this must MAX-reduce. */
    cfg.depth_compare_op = VK_COMPARE_OP_GREATER;
    cfg.blends[0]        = blend_disabled();
    s->draw_pipeline     = pipeline_create_graphics(s->vk, &cfg);

    /* The toon variant shares the vertex layout and push constant, so it is the
       same config with the other fragment shader. Both exist every frame; the
       switch is a pointer choice at draw time, not a rebuild. */
    cfg.vert_path      = "compiledshaders/toon.vert.spv";
    cfg.frag_path      = "compiledshaders/toon.frag.spv";
    s->toon_pipeline   = pipeline_create_graphics(s->vk, &cfg);

    /* Sparkles share the draw's clip rows and color/depth formats but blend
       additively and never write depth: a transparent pass that runs after the
       opaque draw. Same reverse-Z compare, so a sparkle hides behind a trunk. */
    cfg.vert_path          = "compiledshaders/sparkle.vert.spv";
    cfg.frag_path          = "compiledshaders/sparkle.frag.spv";
    cfg.depth_write_enable = false;
    cfg.depth_test_enable  = true;
    cfg.blends[0]          = blend_additive();
    s->sparkle_pipeline    = pipeline_create_graphics(s->vk, &cfg);

    s->toon            = getenv("MU_TOON") != NULL;
    s->pipelines_ready = true;
}

/* The six clip planes in the form (n.xyz, -d) with |n| == 1, extracted from the
   row-major clip rows the shader already has: inside is dot(n, c) + w >= 0.
   Normalizing here is what lets the cull kernel drop its per-plane length();
   a plane whose normal is zero — the far plane under an infinite reverse-Z
   projection — is replaced with one no sphere can fail, so adding a real far
   clip later is a CPU change and not a shader one. */
static void build_frustum(const float rows[4][4], struct SceneVec4 *planes) {
    /* plane = sa*row3 + sb*rowB, in the order the cull used to build them:
       left, right, bottom, top, far, near. */
    static const struct {
        int   b;
        float sa, sb;
    } kPlane[6] = {{0, 1.0f, 1.0f},  {0, 1.0f, -1.0f}, {1, 1.0f, 1.0f},
                   {1, 1.0f, -1.0f}, {2, 0.0f, 1.0f},  {2, 1.0f, -1.0f}};

    for (int p = 0; p < 6; ++p) {
        float nx = kPlane[p].sa * rows[3][0] + kPlane[p].sb * rows[kPlane[p].b][0];
        float ny = kPlane[p].sa * rows[3][1] + kPlane[p].sb * rows[kPlane[p].b][1];
        float nz = kPlane[p].sa * rows[3][2] + kPlane[p].sb * rows[kPlane[p].b][2];
        float nw = kPlane[p].sa * rows[3][3] + kPlane[p].sb * rows[kPlane[p].b][3];

        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len < 1e-20f) {
            planes[p].x = 0.0f;
            planes[p].y = 0.0f;
            planes[p].z = 0.0f;
            planes[p].w = 1e20f;
            continue;
        }
        float inv   = 1.0f / len;
        planes[p].x = nx * inv;
        planes[p].y = ny * inv;
        planes[p].z = nz * inv;
        planes[p].w = nw * inv;
    }
}

/* A view's transients are allocated the first time the view is recorded rather
   than for all SCENE_MAX_VIEWS at upload. Reserving all four multiplied three
   max_survivors-sized arrays by four for views that never render, and it could
   not be done at upload at all: scene_view_set is allowed to arrive after
   scene_upload_scene, and group_count is not final until upload. */
static void ensure_view_frame(Scene *s, uint32_t view, uint32_t lane) {
    SceneViewFrame *vf = &s->view_frame[view][lane];
    if (vf->ready)
        return;
    uint32_t G         = s->group_count ? s->group_count : 1;
    vf->survivors      = alloc_slice(s, (VkDeviceSize)s->max_survivors * sizeof(uint32_t), 16);
    vf->survivor_count = alloc_slice(s, sizeof(uint32_t), 16);
    vf->group_count    = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
    vf->vis_base       = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
    vf->cmd_index      = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
    vf->cursor         = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
    vf->vsrows         = alloc_slice(s, (VkDeviceSize)s->max_survivors * sizeof(struct SceneVsRow), 16);
    vf->draws          = alloc_slice(s, (VkDeviceSize)G * sizeof(struct SceneGpuDraw), 16);
    vf->draw_count     = alloc_slice(s, sizeof(uint32_t), 16);
    /* Three regions of uint2: block totals, visibility offsets, draw offsets.
       The stride must be uint2, not uint: cs_scan_block writes whole uint2
       block totals into region 0, so a uint32 stride made every block total
       overlap the next and corrupted the scan input. */
    vf->scan_aux   = alloc_slice(s, (VkDeviceSize)SCENE_SCAN_BLOCK * 3u * sizeof(SceneU32x2), 16);
    vf->cull_args  = alloc_slice(s, 16, 16);
    vf->group_base = alloc_slice(s, (VkDeviceSize)s->max_survivors * sizeof(uint32_t), 16);
    vf->surv_sorted = alloc_slice(s, (VkDeviceSize)s->max_survivors * sizeof(uint32_t), 16);
    vf->sort_aux   = alloc_slice(s, (VkDeviceSize)128 * sizeof(uint32_t), 16);
    vf->ready      = true;
}

static void build_cull_push(Scene *s, uint32_t view, uint32_t lane, struct SceneCullPush *p) {
    memset(p, 0, sizeof(*p));
    VkBackend      *vk = s->vk;
    SceneViewFrame *vf = &s->view_frame[view][lane];

    build_frustum(s->views[view].clip_rows, p->frustum);
    p->viewparams.x = s->views[view].camera_pos[0];
    p->viewparams.y = s->views[view].camera_pos[1];
    p->viewparams.z = s->views[view].camera_pos[2];
    p->viewparams.w = s->views[view].lod_target;

    p->instances      = slice_addr(vk, s->instances);
    p->cull_meshes    = slice_addr(vk, s->cull_meshes);
    p->cull_rows      = slice_addr(vk, s->cull_rows);
    p->lod_rows       = slice_addr(vk, s->lod_rows);
    p->survivors      = slice_addr(vk, vf->survivors);
    p->survivor_count = slice_addr(vk, vf->survivor_count);
    p->counters       = slice_addr(vk, s->counters[lane]);
    p->cull_args      = slice_addr(vk, vf->cull_args);

    p->counts[0] = s->lod_count;
    p->counts[1] = s->candidate_count;
    p->counts[2] = s->max_survivors;
    p->counts[5] = getenv("MU_SHADER_DEBUG") ? 1u : 0u;
}

static void build_sort_push(Scene *s, uint32_t view, uint32_t lane, struct SceneSortPush *p) {
    memset(p, 0, sizeof(*p));
    VkBackend      *vk = s->vk;
    SceneViewFrame *vf = &s->view_frame[view][lane];

    p->survivors        = slice_addr(vk, vf->survivors);
    p->survivor_count   = slice_addr(vk, vf->survivor_count);
    p->instances        = slice_addr(vk, s->instances);
    p->sort_aux         = slice_addr(vk, vf->sort_aux);
    p->survivors_sorted = slice_addr(vk, vf->surv_sorted);

    p->counts[0] = s->max_survivors;
}

static void build_draw_push(Scene *s, uint32_t view, uint32_t lane, struct SceneDrawPush *p) {
    memset(p, 0, sizeof(*p));
    VkBackend      *vk = s->vk;
    SceneViewFrame *vf = &s->view_frame[view][lane];

    for (int r = 0; r < 4; ++r) {
        p->clip_rows[r].x = s->views[view].clip_rows[r][0];
        p->clip_rows[r].y = s->views[view].clip_rows[r][1];
        p->clip_rows[r].z = s->views[view].clip_rows[r][2];
        p->clip_rows[r].w = s->views[view].clip_rows[r][3];
    }
    /* The sun and the ambient ride in GlobalData, which the renderer owns, so
       the sky pass and both scene pipelines share one description. This push
       only carries what is per-view: the clip rows and the eye. */
    p->camera.x = s->views[view].camera_pos[0];
    p->camera.y = s->views[view].camera_pos[1];
    p->camera.z = s->views[view].camera_pos[2];
    p->camera.w = 0.0f;

    p->group_base  = slice_addr(vk, vf->group_base);
    p->vs_rows     = slice_addr(vk, vf->vsrows);
    p->group_shade = slice_addr(vk, s->group_shade);
    p->materials   = slice_addr(vk, s->materials);

    p->counts[5] = getenv("MU_SHADER_DEBUG") ? 1u : 0u;
}

/* T1c: the GPU slime sim, one chain per frame before any view's cull. The grid
   is built around view 0's camera; with one view that is the only camera there
   is, and with several it is the one whose distance the collision LOD follows.
   state_in is integrated in place then read-only for the grid and resolve;
   resolve commits to state_out (next frame's state_in) plus the draw tables. */
static void slime_record(Scene *s, VkCommandBuffer cmd, uint32_t view) {
    if (!s->slime_active || s->slime_count == 0u || s->view_count == 0u)
        return;

    VkBackend   *vk     = s->vk;
    GpuProfiler *prof   = &vk->gpuprofiler[vk->current_frame];
    uint32_t     count  = s->slime_count;
    uint32_t     groups = (count + 63u) / 64u;
    BufferSlice *st_in  = &s->slime_state[s->slime_parity];
    BufferSlice *st_out = &s->slime_state[s->slime_parity ^ 1u];
    uint32_t     cells  = s->slime_grid_nx * s->slime_grid_nz;
    float        extent = (float)s->slime_grid_nx * s->slime_cell_size;

    cmd_fill_buffer(cmd, s->slime_grid_count, (VkDeviceSize)cells * sizeof(uint32_t), 0u);

    struct SceneSlimePush sp;
    memset(&sp, 0, sizeof(sp));
    sp.state_in   = slice_addr(vk, *st_in);
    sp.state_out  = slice_addr(vk, *st_out);
    sp.instances  = slice_addr(vk, s->instances);
    sp.squash     = slice_addr(vk, s->squash_table);
    sp.stretch    = slice_addr(vk, s->stretch_table);
    sp.face       = slice_addr(vk, s->face_table);
    sp.grid_count = slice_addr(vk, s->slime_grid_count);
    sp.grid_items = slice_addr(vk, s->slime_grid_items);
    if (s->slime_obs_count && s->slime_obs_all.buffer) {
        sp.obstacles = slice_addr(vk, s->slime_obs_all);
        sp.obs_off   = slice_addr(vk, s->slime_obs_off);
        sp.obs_items = slice_addr(vk, s->slime_obs_items);
    }
    sp.counts[0]  = count;
    sp.counts[1]  = s->slime_grid_nx;
    sp.counts[2]  = s->slime_grid_nz;
    sp.counts[3]  = s->slime_grid_cap;
    sp.counts[4]  = s->slime_obs_nx;
    sp.counts[5]  = s->slime_obs_nz;
    sp.counts[6]  = getenv("MU_SLIME_DEBUG") ? 1u : 0u;
    sp.min_x      = s->views[view].camera_pos[0] - extent * 0.5f;
    sp.min_z      = s->views[view].camera_pos[2] - extent * 0.5f;
    sp.obs_min_x  = s->slime_obs_min_x;
    sp.obs_min_z  = s->slime_obs_min_z;
    sp.inv_cell   = s->slime_inv_cell;
    sp.collide_radius = s->slime_collide_radius;

    BufferAccess sim_state[1] = {{.slice = *st_in, .stage = STAGE_CS, .access = ACCESS_SSBO_R | ACCESS_SSBO_W}};
    BufferAccess sim_write[1] = {{.slice = *st_in, .stage = STAGE_CS, .access = ACCESS_SSBO_W}};
    PassDesc     sim_pass     = {.buf_reads       = sim_state,
                                 .buf_read_count  = 1,
                                 .buf_writes      = sim_write,
                                 .buf_write_count = 1,
                                 .pipeline        = s->cs_slime_sim};
    GPU_SCOPE(prof, cmd, "Slime Sim", STAGE_CS) {
        begin_pass(vk, cmd, &sim_pass);
        dispatch_push(vk, cmd, (ByteSpan){&sp, (uint32_t)sizeof(sp)}, groups, 1, 1);
        end_pass(vk, cmd, &sim_pass);
    }

    BufferAccess grid_reads[3] = {
        {.slice = *st_in, .stage = STAGE_CS, .access = ACCESS_SSBO_R},
        {.slice = s->slime_grid_count, .stage = STAGE_CS, .access = ACCESS_SSBO_R | ACCESS_SSBO_W},
        {.slice = s->slime_grid_items, .stage = STAGE_CS, .access = ACCESS_SSBO_R | ACCESS_SSBO_W},
    };
    BufferAccess grid_writes[2] = {
        {.slice = s->slime_grid_count, .stage = STAGE_CS, .access = ACCESS_SSBO_W},
        {.slice = s->slime_grid_items, .stage = STAGE_CS, .access = ACCESS_SSBO_W},
    };
    PassDesc grid_pass = {.buf_reads       = grid_reads,
                          .buf_read_count  = 3,
                          .buf_writes      = grid_writes,
                          .buf_write_count = 2,
                          .pipeline        = s->cs_slime_grid};
    GPU_SCOPE(prof, cmd, "Slime Grid", STAGE_CS) {
        begin_pass(vk, cmd, &grid_pass);
        dispatch_push(vk, cmd, (ByteSpan){&sp, (uint32_t)sizeof(sp)}, groups, 1, 1);
        end_pass(vk, cmd, &grid_pass);
    }

    BufferAccess res_reads[6] = {
        {.slice = *st_in, .stage = STAGE_CS, .access = ACCESS_SSBO_R},
        {.slice = s->slime_grid_count, .stage = STAGE_CS, .access = ACCESS_SSBO_R},
        {.slice = s->slime_grid_items, .stage = STAGE_CS, .access = ACCESS_SSBO_R},
    };
    uint32_t res_read_count = 3u;
    if (s->slime_obs_count && s->slime_obs_all.buffer) {
        res_reads[res_read_count++] = (BufferAccess){.slice = s->slime_obs_all, .stage = STAGE_CS, .access = ACCESS_SSBO_R};
        res_reads[res_read_count++] = (BufferAccess){.slice = s->slime_obs_off, .stage = STAGE_CS, .access = ACCESS_SSBO_R};
        res_reads[res_read_count++] = (BufferAccess){.slice = s->slime_obs_items, .stage = STAGE_CS, .access = ACCESS_SSBO_R};
    }
    BufferAccess res_writes[5] = {
        {.slice = *st_out, .stage = STAGE_CS, .access = ACCESS_SSBO_W},
        {.slice = s->instances, .stage = STAGE_CS, .access = ACCESS_SSBO_W},
        {.slice = s->squash_table, .stage = STAGE_CS, .access = ACCESS_SSBO_W},
        {.slice = s->stretch_table, .stage = STAGE_CS, .access = ACCESS_SSBO_W},
        {.slice = s->face_table, .stage = STAGE_CS, .access = ACCESS_SSBO_W},
    };
    PassDesc res_pass = {.buf_reads       = res_reads,
                         .buf_read_count  = res_read_count,
                         .buf_writes      = res_writes,
                         .buf_write_count = 5,
                         .pipeline        = s->cs_slime_resolve};
    GPU_SCOPE(prof, cmd, "Slime Resolve", STAGE_CS) {
        begin_pass(vk, cmd, &res_pass);
        dispatch_push(vk, cmd, (ByteSpan){&sp, (uint32_t)sizeof(sp)}, groups, 1, 1);
        end_pass(vk, cmd, &res_pass);
    }

    s->slime_parity ^= 1u;
}

void scene_frame(Scene *s, VkCommandBuffer cmd, RenderTarget *color, RenderTarget *depth) {
    if (!s || !s->uploaded || s->view_count == 0 || s->group_count == 0)
        return;
    VkBackend *vk   = s->vk;
    uint32_t   lane = vk->current_frame;
    GpuProfiler *prof = &vk->gpuprofiler[vk->current_frame];

    ensure_pipelines(s, color, depth);

    /* T6: read the previous sample for this lane (the frame-slot wait in
       vk_frame_acquire already covers the copy recorded N frames ago). */
    if (s->frame_serial >= MAX_FRAMES_IN_FLIGHT && s->counters_host[lane].mapping) {
        const uint32_t *c               = (const uint32_t *)s->counters_host[lane].mapping;
        uint32_t        sub             = s->candidate_count;
        s->last_counters.submitted      = sub;
        s->last_counters.culled_frustum = c[SCENE_COUNTER_FRUSTUM];
        s->last_counters.culled_hiz     = c[SCENE_COUNTER_HIZ];
        s->last_counters.drawn          = c[SCENE_COUNTER_DRAWN];
        s->last_counters.lod[0]         = c[SCENE_COUNTER_LOD0];
        s->last_counters.lod[1]         = c[SCENE_COUNTER_LOD1];
        s->last_counters.lod[2]         = c[SCENE_COUNTER_LOD2];
        s->last_counters.lod[3]         = c[SCENE_COUNTER_LOD3];
        s->last_counters.dropped        = c[SCENE_COUNTER_DROPPED];
        s->last_counters.draws          = c[SCENE_COUNTER_DRAWS];
        s->has_counters                 = true;
    }

    /* Every view's transients must exist before anything addresses them, including
       the roster seeding below, which writes the indirect dispatch args into
       cull_args. Allocating lazily inside the view loop left the first frame's
       dispatch reading a null slice and launching zero workgroups. */
    for (uint32_t v = 0; v < s->view_count; ++v)
        forEach(i, MAX_FRAMES_IN_FLIGHT) ensure_view_frame(s, v, i);

    /* Apply a pending roster change to the indirect dispatch args. This is the
       only place the CPU writes them, and only when the roster actually grew. */
    /* scene_frame */
    GPU_SCOPE(prof, cmd, "Scene Upload", STAGE_XFER) {
    if (s->cull_args_dirty[lane]) {
        uint32_t args[4] = {(s->candidate_count + 63u) / 64u, 1u, 1u, 0u};
        if (args[0] == 0)
            args[0] = 1;
        for (uint32_t v = 0; v < s->view_count; ++v)
            renderer_upload_buffer_to_slice(vk, cmd, s->view_frame[v][lane].cull_args, (ByteSpan){args, sizeof(args)});
        s->cull_args_dirty[lane] = false;
    }
    /* T0: a slot-space rebuild (scene_compact_slots) permutes which instance
       lives in which slot, so every row the GPU holds is now wrong. This is the
       rare path and it re-uploads the whole table once; the per-region uploads
       below then keep it current. */
    if (s->uploaded_dirty) {
        /* Slot-indexed, same reason as the initial upload: after a compaction the
           survivors occupy different slots, and gpu_inst[] is addressed by slot. */
        memset(s->instance_upload_scratch, 0, (size_t)s->max_instances * sizeof(struct SceneInstance));
        for (uint32_t i = 0; i < s->max_instances; ++i) {
            if (s->cpu_instances[i].alive)
                s->instance_upload_scratch[i] = s->cpu_instances[i].gpu;
        }
        renderer_upload_buffer_to_slice(
            vk, cmd, s->instances,
            (ByteSpan){s->instance_upload_scratch, (uint32_t)s->max_instances * sizeof(struct SceneInstance)});
        s->uploaded_dirty = false;
        /* the regions were just written wholesale; no per-row work needed */
        s->static_dirty_count = 0;
    }

    /* T1a: the dynamic prefix. Every slot below dynamic_capacity is rewritten
       every frame as one contiguous span — the app moves most of them, so a
       dirty list would degenerate to the same bytes plus the bookkeeping.
       Under the GPU slime sim the whole dynamic prefix is owned by compute, so
       the CPU upload would only clobber the frame's work: skipped entirely. */
    if (s->dynamic_count && !s->slime_active) {
        for (uint32_t i = 0; i < s->dynamic_count; ++i)
            s->instance_upload_scratch[i] = s->cpu_instances[i].gpu;
        BufferSlice dyn = s->instances;
        dyn.offset += 0;
        dyn.size = (VkDeviceSize)s->dynamic_count * sizeof(struct SceneInstance);
        renderer_upload_buffer_to_slice(
            vk, cmd, dyn, (ByteSpan){s->instance_upload_scratch, s->dynamic_count * sizeof(struct SceneInstance)});
    }
    s->dirty_count = 0;

    /* T1b: static rows, only those that actually changed. A settled scene writes
       nothing here, which is the whole point of the split. */
    for (uint32_t d = 0; d < s->static_dirty_count && d < s->max_instances; ++d) {
        uint32_t    slot = s->static_dirty_slots[d];
        BufferSlice row  = s->instances;
        row.offset       = s->instances.offset + (VkDeviceSize)slot * sizeof(struct SceneInstance);
        row.size         = sizeof(struct SceneInstance);
        renderer_upload_buffer_to_slice(vk, cmd, row,
                                        (ByteSpan){&s->cpu_instances[slot].gpu, sizeof(struct SceneInstance)});
    }
    s->static_dirty_count = 0;

    /* Bloom strength rides separately: rebuild the dense blooming-slot list
       from the bitset and upload the table whole when dirty. Rebuild cost is
       proportional to set words, upload is one contiguous copy. */
    if (s->bloom_dirty) {
        s->bloom_count = 0;
        for (uint32_t w = 0; w * 64u < s->max_instances; ++w) {
            uint64_t word = w < s->bloom_bits.word_count ? s->bloom_bits.array[w] : 0u;
            while (word) {
                uint32_t bit                     = (uint32_t)mu_trailing_zeroes_u64(word);
                s->bloom_dense[s->bloom_count++] = w * 64u + bit;
                word &= word - 1u;
            }
        }
        renderer_upload_buffer_to_slice(vk, cmd, s->bloom_table,
                                        (ByteSpan){s->bloom_strength, (size_t)s->max_instances * sizeof(float)});
        s->bloom_dirty = false;
    }

    /* Wiggle amplitudes are set at spawn, but scene_set_instance_wiggle may run
       after the first upload, so mirror bloom's dirty re-upload here. Face and
       squash ride along: all three are small whole-table uploads. Under the
       slime sim, squash and face are GPU-owned and must not be overwritten. */
    if (!s->slime_active && (s->wiggle_dirty || s->squash_dirty || s->stretch_dirty || s->face_dirty)) {
        renderer_upload_buffer_to_slice(vk, cmd, s->wiggle_table,
                                        (ByteSpan){s->wiggle_amp, (size_t)s->max_instances * sizeof(float)});
        renderer_upload_buffer_to_slice(vk, cmd, s->squash_table,
                                        (ByteSpan){s->squash_v, (size_t)s->max_instances * sizeof(float)});
        renderer_upload_buffer_to_slice(vk, cmd, s->stretch_table,
                                        (ByteSpan){s->stretch_v, (size_t)s->max_instances * sizeof(float)});
        renderer_upload_buffer_to_slice(vk, cmd, s->face_table,
                                        (ByteSpan){s->face_id, (size_t)s->max_instances * sizeof(uint32_t)});
        s->wiggle_dirty = false;
        s->squash_dirty = false;
        s->stretch_dirty = false;
        s->face_dirty   = false;
    }

    /* Candidates mirror the live slot set; re-upload the prefix whenever the
       roster changed. This is a per-roster-change cost, not a per-frame one. */
    if (s->candidates_dirty) {
        for (uint32_t r = 0; r < s->candidate_count; ++r)
            s->candidate_upload_scratch[r] = s->cpu_rows[r];
        BufferSlice cand = s->cull_rows;
        cand.size        = (VkDeviceSize)s->candidate_count * sizeof(struct SceneCullRow);
        renderer_upload_buffer_to_slice(
            vk, cmd, cand, (ByteSpan){s->candidate_upload_scratch, s->candidate_count * sizeof(struct SceneCullRow)});
        s->candidates_dirty = false;
    }
    } /* Scene Upload */

    uint32_t G = s->group_count;

    /* The streamed world moved: re-bucket the obstacle discs around the camera
       and upload before the sim reads them this frame. */
    if (s->slime_active && s->slime_obs_dirty) {
        GPU_SCOPE(prof, cmd, "Obs Rebuild", STAGE_XFER)
            slime_obstacles_rebuild(s, cmd, 0u);
    }

    slime_record(s, cmd, 0u);

    for (uint32_t view = 0; view < s->view_count; ++view) {
        SceneViewFrame *vf = &s->view_frame[view][lane];
        ensure_view_frame(s, view, lane);

        /* Only two of the transients actually need clearing, and the reason is worth
       stating, because the other three used to be here:
         - cursor[] is written by cs_emit for every group it emits, and every
           group below G is either emitted or has no survivors to scatter;
         - draw_count and the counter block are written unconditionally by
           cs_scan_blocks and cs_emit;
         - group_count is accumulated into with an atomic, so it must start at
           zero;
         - survivor_count likewise.
       Three fillBuffer calls per view per frame were buying nothing. */
        cmd_fill_buffer(cmd, vf->survivor_count, sizeof(uint32_t), 0u);
        cmd_fill_buffer(cmd, vf->group_count, (VkDeviceSize)G * sizeof(uint32_t), 0u);

        struct SceneCullPush cp_push;
        build_cull_push(s, view, lane, &cp_push);
        struct SceneDrawPush dp;
        build_draw_push(s, view, lane, &dp);

        struct SceneCompactPush cp;
        memset(&cp, 0, sizeof(cp));
        cp.survivors      = slice_addr(vk, vf->survivors);
        cp.survivor_count = slice_addr(vk, vf->survivor_count);
        cp.group_count    = slice_addr(vk, vf->group_count);
        cp.vis_base       = slice_addr(vk, vf->vis_base);
        cp.cmd_index      = slice_addr(vk, vf->cmd_index);
        cp.cursor         = slice_addr(vk, vf->cursor);
        cp.vs_rows        = slice_addr(vk, vf->vsrows);
        cp.instances      = slice_addr(vk, s->instances);
        cp.bloom          = slice_addr(vk, s->bloom_table);
        cp.wiggle         = slice_addr(vk, s->wiggle_table);
        cp.squash         = slice_addr(vk, s->squash_table);
        cp.stretch        = slice_addr(vk, s->stretch_table);
        cp.face_id        = slice_addr(vk, s->face_table);
        cp.draws          = slice_addr(vk, vf->draws);
        cp.group_static   = slice_addr(vk, s->group_static);
        cp.draw_count     = slice_addr(vk, vf->draw_count);
        cp.scan_aux       = slice_addr(vk, vf->scan_aux);
        cp.counters       = slice_addr(vk, s->counters[lane]);
        cp.group_base     = slice_addr(vk, vf->group_base);
        cp.counts[0]      = s->lod_count;
        cp.counts[1]      = G;
        cp.counts[2]      = s->max_survivors;
        cp.counts[3]      = s->candidate_count;
        cp.counts[4]      = (G + SCENE_SCAN_BLOCK - 1u) / SCENE_SCAN_BLOCK;
        /* DEBUG: shader-side printf. Off by default; it serialises and floods
           the log, so it is opt-in per run. */
        cp.counts[5] = cp_push.counts[5];
        if (cp.counts[5] && view == 0 && lane == 0)
            log_info("[dbg] shader printf enable = %u, G = %u", cp.counts[5], G);
        /* Every table is a slice of one buffer, so naming a slice names a byte
           range. That is the whole point of declaring these instead of
           barriering the pool: the previous code emitted four whole-pool
           barriers per view, implicating 512 MB of unrelated allocations to
           order 32 KB of counts. */
        BufferAccess cull_reads[7] = {
            {.slice  = s->instances,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = s->cull_meshes,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = s->cull_rows,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = s->lod_rows,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = vf->survivor_count,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R | ACCESS_SSBO_W},
            {.slice  = s->counters[lane],
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R | ACCESS_SSBO_W},
            {.slice  = vf->cull_args,
             .stage  = STAGE_DRAW,
             .access = ACCESS_INDIRECT},
        };
        BufferAccess cull_writes[2] = {
            {.slice  = vf->survivors,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
            {.slice  = vf->survivor_count,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
        };

        /* T2: cull */
        PassDesc cull_pass = {
            .buf_reads       = cull_reads,
            .buf_read_count  = ARRAY_COUNT(cull_reads),
            .buf_writes      = cull_writes,
            .buf_write_count = ARRAY_COUNT(cull_writes),
            .pipeline        = s->cs_cull,
        };

        /* T2: cull */
        GPU_SCOPE(prof, cmd, "Cull", STAGE_CS) {
            begin_pass(vk, cmd, &cull_pass);
            dispatch_indirect(vk, cmd, (ByteSpan){&cp_push, (uint32_t)sizeof(cp_push)}, vf->cull_args);
            end_pass(vk, cmd, &cull_pass);
        }

        /* T3a: histogram survivors by (mesh,lod) group */
        BufferAccess count_reads[2] = {
            {.slice  = vf->survivors,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = vf->survivor_count,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
        };
        BufferAccess count_writes[1] = {
            {.slice  = vf->group_count,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
        };
        PassDesc count_pass = {
            .buf_reads       = count_reads,
            .buf_read_count  = ARRAY_COUNT(count_reads),
            .buf_writes      = count_writes,
            .buf_write_count = ARRAY_COUNT(count_writes),
            .pipeline        = s->cs_count,
        };
        GPU_SCOPE(prof, cmd, "Compact Count", STAGE_CS) {
            begin_pass(vk, cmd, &count_pass);
            dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, (s->max_survivors + 63u) / 64u, 1, 1);
            end_pass(vk, cmd, &count_pass);
        }

        /* T3b: scan group_count -> vis_base + draw commands + draw_count */
        /* T3b: two-level scan + emit. The old code ran this as one workgroup
           of one thread looping over all G groups; it is now O(G/1024) wide.
           scan_aux carries the per-block totals between the two levels. */
        BufferAccess scan_reads[2] = {
            {.slice  = vf->group_count,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = s->group_static,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
        };
        BufferAccess scan_writes[8] = {
            {.slice  = vf->vis_base,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W | ACCESS_SSBO_W},
            {.slice  = vf->cmd_index,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
            {.slice  = vf->cursor,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
            {.slice  = vf->draws,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
            {.slice  = vf->draw_count,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
            {.slice  = vf->scan_aux,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
            /* cs_scan_blocks mirrors the command count here; without this
               declaration the write is not published before the readback copy. */
            {.slice  = s->counters[lane],
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
            /* cs_emit records vis_base -> group id so the VS can resolve mesh
               from a uniform per-draw row instead of decoding a packed key */
            {.slice  = vf->group_base,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
        };
        PassDesc scan_pass = {
            .buf_reads       = scan_reads,
            .buf_read_count  = ARRAY_COUNT(scan_reads),
            .buf_writes      = scan_writes,
            .buf_write_count = ARRAY_COUNT(scan_writes),
            .pipeline        = s->cs_scan_block,
        };
        GPU_SCOPE(prof, cmd, "Scan Block", STAGE_CS) {
            begin_pass(vk, cmd, &scan_pass);
            dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, cp.counts[4], 1, 1);
            end_pass(vk, cmd, &scan_pass);
        }

        /* cs_scan_blocks READS scan_aux region 0 (the per-block totals cs_scan_block
           just wrote) and writes regions 1 and 2. Declaring writes only left the
           read unpublished, so it consumed stale/zero block offsets and every
           vis_base and cmd_index came out wrong — visible as duplicated and
           missing slots in vis[] that changed as the camera culled instances. */
        BufferAccess block_reads[1] = {
            {.slice  = vf->scan_aux,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
        };
        PassDesc block_pass = {
            .buf_reads       = block_reads,
            .buf_read_count  = ARRAY_COUNT(block_reads),
            .buf_writes      = scan_writes,
            .buf_write_count = ARRAY_COUNT(scan_writes),
            .pipeline        = s->cs_scan_blocks,
        };
        GPU_SCOPE(prof, cmd, "Scan Blocks", STAGE_CS) {
            begin_pass(vk, cmd, &block_pass);
            dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, 1, 1, 1);
            end_pass(vk, cmd, &block_pass);
        }

        /* cs_emit reads group_count, group_static and the block offsets in
           scan_aux regions 1 and 2. */
        BufferAccess emit_reads[3] = {
            {.slice  = vf->group_count,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = s->group_static,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = vf->scan_aux,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
        };
        PassDesc emit_pass = {
            .buf_reads       = emit_reads,
            .buf_read_count  = ARRAY_COUNT(emit_reads),
            .buf_writes      = scan_writes,
            .buf_write_count = ARRAY_COUNT(scan_writes),
            .pipeline        = s->cs_emit,
        };
        GPU_SCOPE(prof, cmd, "Emit Draws", STAGE_CS) {
            begin_pass(vk, cmd, &emit_pass);
            dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, cp.counts[4], 1, 1);
            end_pass(vk, cmd, &emit_pass);
        }

        /* T3c: scatter survivors into their group's contiguous vsrow range.
           The row is built HERE, once per survivor, from the per-slot tables;
           the VS then does one dependent load per VERTEX instead of seven.
           This is the same access pattern the draw would have, paid at 1/250th
           the volume. */
        BufferAccess scatter_reads[10] = {
            {.slice  = vf->survivors,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = vf->survivor_count,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = vf->vis_base,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = vf->cursor,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R | ACCESS_SSBO_W},
            {.slice  = s->instances,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = s->bloom_table,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = s->wiggle_table,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = s->squash_table,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = s->stretch_table,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
            {.slice  = s->face_table,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_R},
        };
        BufferAccess scatter_writes[1] = {
            {.slice  = vf->vsrows,
             .stage  = STAGE_CS,
             .access = ACCESS_SSBO_W},
        };
        PassDesc scatter_pass = {
            .buf_reads       = scatter_reads,
            .buf_read_count  = ARRAY_COUNT(scatter_reads),
            .buf_writes      = scatter_writes,
            .buf_write_count = ARRAY_COUNT(scatter_writes),
            .pipeline        = s->cs_scatter,
        };
        GPU_SCOPE(prof, cmd, "Scatter", STAGE_CS) {
            begin_pass(vk, cmd, &scatter_pass);
            dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, (s->max_survivors + 63u) / 64u, 1, 1);
            end_pass(vk, cmd, &scatter_pass);
        }

        /* T5: draw */
        /* The renderer draws the sky into the HDR target first, so the draw
           loads it rather than clearing. Depth still clears, because the sky
           leaves no depth behind. */
        PassAttachment col = {.target = color, .load = LOAD_KEEP, .store = STORE_KEEP};
        PassAttachment dep = {.target = depth, .load = LOAD_CLEAR, .store = STORE_KEEP};
        dep.clear[0]       = 0.0f; /* reverse-Z: the far plane is 0.0 */

        BufferAccess draw_reads[8] = {
            {.slice  = s->vertex_arena,
             .stage  = STAGE_VERT,
             .access = ACCESS_SSBO_R},
            {.slice  = s->group_shade,
             .stage  = STAGE_VERT,
             .access = ACCESS_SSBO_R},
            {.slice  = s->materials,
             .stage  = STAGE_FRAG,
             .access = ACCESS_SSBO_R},
            {.slice  = vf->vsrows,
             .stage  = STAGE_VERT,
             .access = ACCESS_SSBO_R},
            {.slice  = vf->draws,
             .stage  = STAGE_DRAW,
             .access = ACCESS_INDIRECT},
            {.slice  = vf->draw_count,
             .stage  = STAGE_DRAW,
             .access = ACCESS_INDIRECT},
            {.slice  = s->index_arena,
             .stage  = STAGE_INDEX,
             .access = ACCESS_INDEX},
            {.slice  = vf->group_base,
             .stage  = STAGE_VERT,
             .access = ACCESS_SSBO_R},
        };
        PassDesc draw_pass = {
            .colors         = &col,
            .color_count    = 1,
            .depth          = &dep,
            .buf_reads      = draw_reads,
            .buf_read_count = ARRAY_COUNT(draw_reads),
            .pipeline       = s->toon ? s->toon_pipeline : s->draw_pipeline,
        };
        GPU_SCOPE(prof, cmd, "Scene Draw", STAGE_COLOR) {
            begin_pass(vk, cmd, &draw_pass);

            vkCmdBindIndexBuffer(cmd, vk->gpu_pool.buffer, s->index_arena.offset, VK_INDEX_TYPE_UINT16);
            cmd_draw_indexed_indirect_count(vk, cmd, (ByteSpan){&dp, (uint32_t)sizeof(dp)}, vf->draws, vf->draw_count, G,
                                            (uint32_t)sizeof(struct SceneGpuDraw));
            end_pass(vk, cmd, &draw_pass);
        }

        /* T5b: transparent sparkle pass, after the opaque draw. Reads the slime
           body rows the resolve pass just wrote, blends additively over the
           lit HDR colour, and depth-tests against the opaque depth without
           writing it. Procedural: the vertex shader derives each sparkle from
           its instance id, so there is no particle buffer to upload. */
        if (s->slime_active && s->slime_count && s->sparkles_per_slime) {
            struct SceneSparklePush spk;
            memset(&spk, 0, sizeof(spk));
            for (int r = 0; r < 4; ++r) {
                spk.clip_rows[r].x = s->views[view].clip_rows[r][0];
                spk.clip_rows[r].y = s->views[view].clip_rows[r][1];
                spk.clip_rows[r].z = s->views[view].clip_rows[r][2];
                spk.clip_rows[r].w = s->views[view].clip_rows[r][3];
            }
            spk.camera.x = s->views[view].camera_pos[0];
            spk.camera.y = s->views[view].camera_pos[1];
            spk.camera.z = s->views[view].camera_pos[2];
            spk.instances = slice_addr(vk, s->instances);
            spk.counts[0] = s->slime_count * s->sparkles_per_slime;
            spk.counts[1] = s->slime_count;
            spk.counts[2] = s->sparkles_per_slime;

            PassAttachment scol = {.target = color, .load = LOAD_KEEP, .store = STORE_KEEP};
            PassAttachment sdep = {.target = depth, .load = LOAD_KEEP, .store = STORE_KEEP};
            BufferAccess   sreads[1] = {
                {.slice = s->instances, .stage = STAGE_VERT, .access = ACCESS_SSBO_R},
            };
            PassDesc spass = {.colors         = &scol,
                              .color_count    = 1,
                              .depth          = &sdep,
                              .buf_reads      = sreads,
                              .buf_read_count = ARRAY_COUNT(sreads),
                              .pipeline       = s->sparkle_pipeline};
            GPU_SCOPE(prof, cmd, "Sparkle", STAGE_COLOR) {
                begin_pass(vk, cmd, &spass);
                push_constants(vk, cmd, (ByteSpan){&spk, (uint32_t)sizeof(spk)});
                vkCmdDraw(cmd, 6u, spk.counts[0], 0u, 0u);
                end_pass(vk, cmd, &spass);
            }
        }
    }

    /* copy counters to the host-visible lane buffer for next time */
    cmd_copy_buffer(cmd, s->counters[lane], s->counters_host[lane].buffer, 0, sizeof(uint32_t) * SCENE_COUNTERS);

    s->frame_serial++;
}

uint32_t scene_lod_count(const Scene *s) { return s ? s->lod_count : 0u; }

void scene_set_toon(Scene *s, bool toon) {
    if (s)
        s->toon = toon;
}

bool scene_toon(const Scene *s) { return s ? s->toon : false; }

bool scene_counters(const Scene *s, SceneCounters *out) {
    if (!s || !s->has_counters)
        return false;
    *out = s->last_counters;
    return true;
}
