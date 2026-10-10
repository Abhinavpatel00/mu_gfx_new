/* scene.c — GPU-driven data-oriented 3D scene.
   T1 upload, T2 cull, T3 compact, T5 draw, T6 counters.
   See docs/gpu-driven-dod-redesign.md. */
#include "scene.h"

#include "../../common.h"
#include <math.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

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
    BufferSlice survivors;
    BufferSlice survivor_count;
    BufferSlice group_count;
    BufferSlice vis_base;
    BufferSlice cmd_index;
    BufferSlice cursor;
    BufferSlice vis;
    BufferSlice draws;
    BufferSlice draw_count;
    BufferSlice scan_aux;   /* 3 * SCENE_SCAN_BLOCK u32x2: block totals + offsets */
    BufferSlice cull_args;  /* VkDispatchIndirectCommand triple, INDIRECT usage */
    BufferSlice group_base; /* max_survivors x u32: vis_base -> group id */
} SceneViewFrame;

#define SCENE_MAX_LODS 4

struct Scene {
    VkBackend *vk;

    uint32_t max_instances;
    uint32_t max_meshes;
    uint32_t max_lod_rows;
    uint32_t max_materials;
    uint32_t max_survivors;

    /* persistent gpu tables */
    BufferSlice instances;
    BufferSlice cull_meshes;
    BufferSlice shade_meshes;
    BufferSlice lod_rows;
    BufferSlice materials;
    BufferSlice cull_rows;
    BufferSlice group_static; /* G x SceneGpuDraw templates */
    BufferSlice group_mesh;   /* G x u32: group id -> mesh id, for the VS */
    BufferSlice vertex_arena; /* mesh-local packed vertices */
    BufferSlice index_arena;  /* u16 indices, bound once per class */

    /* CPU staging for assets (uploaded once by scene_upload_scene) */
    struct ScenePackedVertex *staged_vertices;
    uint32_t                  staged_vertex_count;
    uint32_t                  staged_vertex_cap;
    uint16_t                 *staged_indices;
    uint32_t                  staged_index_count;
    uint32_t                  staged_index_cap;

    struct SceneCullMesh    *cpu_cull;
    struct SceneShadeMesh   *cpu_shade;
    struct SceneLodRow      *cpu_lod;
    struct SceneGpuMaterial *cpu_mat;
    struct SceneCullRow     *cpu_rows;
    struct SceneGpuDraw     *cpu_group_static;

    uint32_t mesh_count;
    uint32_t lod_row_count;
    uint32_t material_count;
    uint32_t group_count; /* = mesh_count * lod_count */

    /* CPU truth. One slot space, split by a scene constant: [0, dynamic_count)
       is the dynamic prefix rewritten every frame, [dynamic_capacity, ...) is the
       static suffix uploaded once. No per-row region flag exists. */
    SceneCpuInstance     *cpu_instances;
    struct SceneInstance *instance_upload_scratch;
    struct SceneCullRow  *candidate_upload_scratch;
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

    /* per view, per frame-in-flight transient tables */
    SceneViewFrame view_frame[SCENE_MAX_VIEWS][MAX_FRAMES_IN_FLIGHT];
    uint32_t       view_count;

    SceneViewDesc views[SCENE_MAX_VIEWS];
    float         sun[3];
    float         ambient;
    float         clear[4];

    /* counters */
    BufferSlice   counters[MAX_FRAMES_IN_FLIGHT];
    Buffer        counters_host[MAX_FRAMES_IN_FLIGHT];
    SceneCounters last_counters;
    uint32_t      frame_serial;
    bool          has_counters;

    /* pipelines */
    PipelineID cs_cull, cs_count, cs_scan_block, cs_scan_blocks, cs_emit, cs_scatter;
    PipelineID draw_pipeline;
    bool       pipelines_ready;
    bool       uploaded;
    VkFormat   color_format, depth_format;
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
    s->max_materials = desc->max_materials ? desc->max_materials : 64;
    s->max_survivors = desc->max_survivors ? desc->max_survivors : s->max_instances;

    /* The split point. A caller that gives only max_instances gets an all-dynamic
       scene, which is the old behaviour and still correct. */
    s->dynamic_capacity = desc->dynamic_capacity ? desc->dynamic_capacity : s->max_instances;
    s->static_capacity  = desc->static_capacity ? desc->static_capacity : (s->max_instances - s->dynamic_capacity);
    if (s->dynamic_capacity + s->static_capacity > s->max_instances)
        s->static_capacity = s->max_instances - s->dynamic_capacity;

    s->sun[0]   = 0.3f;
    s->sun[1]   = 0.9f;
    s->sun[2]   = 0.4f;
    s->ambient  = 0.15f;
    s->clear[0] = 0.05f;
    s->clear[1] = 0.07f;
    s->clear[2] = 0.10f;
    s->clear[3] = 1.0f;

    s->instances    = alloc_slice(s, (VkDeviceSize)s->max_instances * sizeof(struct SceneInstance), 16);
    s->cull_meshes  = alloc_slice(s, (VkDeviceSize)s->max_meshes * sizeof(struct SceneCullMesh), 16);
    s->shade_meshes = alloc_slice(s, (VkDeviceSize)s->max_meshes * sizeof(struct SceneShadeMesh), 16);
    s->lod_rows     = alloc_slice(s, (VkDeviceSize)s->max_lod_rows * sizeof(struct SceneLodRow), 16);
    s->materials    = alloc_slice(s, (VkDeviceSize)s->max_materials * sizeof(struct SceneGpuMaterial), 16);
    s->cull_rows    = alloc_slice(s, (VkDeviceSize)s->max_instances * sizeof(struct SceneCullRow), 16);

    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        s->counters[i] = alloc_slice(s, sizeof(uint32_t) * SCENE_COUNTERS, 16);
        if (!create_buffer(vk, sizeof(uint32_t) * SCENE_COUNTERS, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VMA_MEMORY_USAGE_CPU_ONLY, &s->counters_host[i]))
            log_error("[scene] counters host buffer failed");
    }

    /* cull_args has exactly one writer: scene_frame. Seeding it here as well
       would put two vkCmdCopyBuffer on the same 16 bytes in the first frame's
       command buffer (the seed, then the reseed this flag forces), with no
       barrier between them — a WRITE_AFTER_WRITE the validation layer flags. */

    /* scene_create */
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        s->cull_args_dirty[i] = true;
    s->staged_vertex_cap = 4096;
    s->staged_vertices   = malloc((size_t)s->staged_vertex_cap * sizeof(struct ScenePackedVertex));
    s->staged_index_cap  = 8192;
    s->staged_indices    = malloc((size_t)s->staged_index_cap * sizeof(uint16_t));

    s->cpu_instances            = calloc(s->max_instances, sizeof(SceneCpuInstance));
    s->instance_upload_scratch  = calloc(s->max_instances, sizeof(struct SceneInstance));
    s->candidate_upload_scratch = calloc(s->max_instances, sizeof(struct SceneCullRow));
    s->dirty_slots              = calloc(s->max_instances, sizeof(uint32_t));
    s->static_dirty_slots       = calloc(s->max_instances, sizeof(uint32_t));
    s->row_of                   = malloc((size_t)s->max_instances * sizeof(uint32_t));
    for (uint32_t i = 0; i < s->max_instances; ++i)
        s->row_of[i] = UINT32_MAX;

    s->cpu_cull         = calloc(s->max_meshes, sizeof(struct SceneCullMesh));
    s->cpu_shade        = calloc(s->max_meshes, sizeof(struct SceneShadeMesh));
    s->cpu_lod          = calloc(s->max_lod_rows, sizeof(struct SceneLodRow));
    s->cpu_mat          = calloc(s->max_materials, sizeof(struct SceneGpuMaterial));
    s->cpu_rows         = calloc(s->max_instances, sizeof(struct SceneCullRow));
    s->cpu_group_static = calloc(s->max_meshes, sizeof(struct SceneGpuDraw));

    s->cs_cull        = pipeline_create_compute(vk, "compiledshaders/scene.cs_cull.comp.spv");
    s->cs_count       = pipeline_create_compute(vk, "compiledshaders/compact.cs_count.comp.spv");
    s->cs_scan_block  = pipeline_create_compute(vk, "compiledshaders/compact.cs_scan_block.comp.spv");
    s->cs_scan_blocks = pipeline_create_compute(vk, "compiledshaders/compact.cs_scan_blocks.comp.spv");
    s->cs_emit        = pipeline_create_compute(vk, "compiledshaders/compact.cs_emit.comp.spv");
    s->cs_scatter     = pipeline_create_compute(vk, "compiledshaders/compact.cs_scatter.comp.spv");

    log_info("[scene] created: max_instances=%u (dynamic<%u) max_meshes=%u max_survivors=%u", s->max_instances,
             s->dynamic_capacity, s->max_meshes, s->max_survivors);
    return s;
}

void scene_destroy(Scene *s) {
    if (!s)
        return;
    VkBackend *vk = s->vk;
    wait_idle(vk);

    buffer_pool_free(s->instances);
    buffer_pool_free(s->cull_meshes);
    buffer_pool_free(s->shade_meshes);
    buffer_pool_free(s->lod_rows);
    buffer_pool_free(s->materials);
    buffer_pool_free(s->cull_rows);
    buffer_pool_free(s->group_static);
    buffer_pool_free(s->group_mesh);
    buffer_pool_free(s->vertex_arena);
    buffer_pool_free(s->index_arena);
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
        buffer_pool_free(vf->vis);
        buffer_pool_free(vf->draws);
        buffer_pool_free(vf->draw_count);
        buffer_pool_free(vf->scan_aux);
        buffer_pool_free(vf->cull_args);
        buffer_pool_free(vf->group_base);
    }
    free(s->staged_vertices);
    free(s->staged_indices);
    free(s->cpu_instances);
    free(s->instance_upload_scratch);
    free(s->candidate_upload_scratch);
    free(s->dirty_slots);
    free(s->static_dirty_slots);
    free(s->row_of);
    free(s->cpu_cull);
    free(s->cpu_shade);
    free(s->cpu_lod);
    free(s->cpu_mat);
    free(s->cpu_rows);
    free(s->cpu_group_static);
    free(s);
}

/* ============================================================ assets */

uint32_t scene_mesh_add(Scene *s, const SceneMeshDesc *desc) {
    if (!s || s->mesh_count >= s->max_meshes)
        return UINT32_MAX;
    uint32_t mesh = s->mesh_count++;

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
    cm.lod_count            = 1;
    cm.local_sphere_xy      = pack_half2(desc->local_center[0], desc->local_center[1]);
    cm.local_sphere_zr      = pack_half2(desc->local_center[2], desc->local_radius);
    s->cpu_cull[mesh]       = cm;

    /* one lod rung (LOD0) */
    struct SceneLodRow lr          = {.error = 1e30f, .first_index = ibase, .index_count = desc->index_count};
    s->cpu_lod[s->lod_row_count++] = lr;

    /* shade mesh: vertex stream address is baked at upload; store vbase for now */
    s->cpu_shade[mesh].base_vertex = vbase;
    s->cpu_shade[mesh].material    = desc->material;

    /* group template for (mesh, lod0) */
    struct SceneGpuDraw gs    = {0};
    gs.index_count            = desc->index_count;
    gs.first_index            = ibase;
    gs.vertex_offset          = 0;
    gs.mesh_lod               = mesh;
    s->cpu_group_static[mesh] = gs;
    s->group_count            = s->mesh_count; /* lod_count == 1 */

    log_info("[scene] mesh %u: %u verts %u indices (vbase %u ibase %u)", mesh, desc->vertex_count, desc->index_count,
             vbase, ibase);
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
    s->cpu_instances[slot].mesh          = desc->mesh;
    s->cpu_instances[slot].alive         = 1;
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
        uint32_t moved_row          = s->row_of[last_slot];
        s->cpu_instances[slot]      = s->cpu_instances[last_slot];
        s->row_of[slot]             = moved_row;
        s->cpu_rows[moved_row].slot = slot;
    }
    memset(&s->cpu_instances[last_slot], 0, sizeof(s->cpu_instances[last_slot]));
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

    /* dst[] is scratch. max_instances is a fixed capacity decided at
       scene_create, so this is a bounded stack-free allocation made once per
       compaction, which happens at load boundaries and never per event. */
    uint32_t *dst = malloc((size_t)s->max_instances * sizeof(uint32_t));
    if (!dst)
        return;

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
            s->cpu_instances[i].alive = 0;
            s->row_of[i]              = UINT32_MAX;
        }
        s->cpu_rows[row].slot = to;
        s->cpu_rows[row].mesh = (uint16_t)s->cpu_instances[to].mesh;
        s->row_of[to]         = row;
        row++;
    }
    free(dst);

    s->dynamic_count      = dyn;
    s->static_count       = sta;
    s->instance_count     = dyn + sta;
    s->candidate_count    = row;
    s->dirty_count        = 0;
    s->static_dirty_count = 0;
    s->uploaded_dirty     = true;
    s->candidates_dirty   = true;
}

void scene_view_set(Scene *s, uint32_t view, const SceneViewDesc *vd) {
    if (!s || view >= SCENE_MAX_VIEWS)
        return;
    memcpy(s->views[view].clip_rows, vd->clip_rows, sizeof(vd->clip_rows));
    memcpy(s->views[view].camera_pos, vd->camera_pos, sizeof(vd->camera_pos));
    s->views[view].lod_target = vd->lod_target > 0.0f ? vd->lod_target : 1.0f;
    s->views[view].near_z     = vd->near_z;
    s->views[view].far_z      = vd->far_z;
    if (view + 1 > s->view_count)
        s->view_count = view + 1;
}

void scene_set_sun(Scene *s, const float dir[3], float ambient) {
    s->sun[0]  = dir[0];
    s->sun[1]  = dir[1];
    s->sun[2]  = dir[2];
    s->ambient = ambient;
}

void scene_set_clear(Scene *s, const float rgba[4]) { memcpy(s->clear, rgba, sizeof(s->clear)); }

/* ============================================================ upload (T0) */

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

    uint64_t vbase_addr = slice_addr(vk, varena);
    for (uint32_t m = 0; m < s->mesh_count; ++m) {
        s->cpu_shade[m].vertex_stream =
            vbase_addr + (uint64_t)s->cpu_shade[m].base_vertex * sizeof(struct ScenePackedVertex);
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
    renderer_upload_buffer_to_slice(vk, cmd, s->shade_meshes,
                                    (ByteSpan){s->cpu_shade, s->max_meshes * sizeof(struct SceneShadeMesh)});
    renderer_upload_buffer_to_slice(vk, cmd, s->lod_rows,
                                    (ByteSpan){s->cpu_lod, s->max_lod_rows * sizeof(struct SceneLodRow)});
    renderer_upload_buffer_to_slice(vk, cmd, s->materials,
                                    (ByteSpan){s->cpu_mat, s->max_materials * sizeof(struct SceneGpuMaterial)});
    renderer_upload_buffer_to_slice(vk, cmd, s->cull_rows,
                                    (ByteSpan){s->cpu_rows, s->max_instances * sizeof(struct SceneCullRow)});

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

    /* per-view, per-lane transient tables */
    uint32_t G = s->group_count ? s->group_count : 1;
    for (uint32_t v = 0; v < SCENE_MAX_VIEWS; ++v) { /* <-- was s->view_count */
        forEach(i, MAX_FRAMES_IN_FLIGHT) {
            SceneViewFrame *vf = &s->view_frame[v][i];
            vf->survivors      = alloc_slice(s, (VkDeviceSize)s->max_survivors * sizeof(uint32_t), 16);
            vf->survivor_count = alloc_slice(s, sizeof(uint32_t), 16);
            vf->group_count    = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
            vf->vis_base       = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
            vf->cmd_index      = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
            vf->cursor         = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
            vf->vis            = alloc_slice(s, (VkDeviceSize)s->max_survivors * sizeof(uint32_t), 16);
            vf->draws          = alloc_slice(s, (VkDeviceSize)G * sizeof(struct SceneGpuDraw), 16);
            vf->draw_count     = alloc_slice(s, sizeof(uint32_t), 16);
            /* Three regions of uint2: block totals, visibility offsets, draw offsets.
               The stride must be uint2, not uint: cs_scan_block writes whole
               uint2 block totals into region 0, so a uint32 stride made every
               block total overlap the next and corrupted the scan input. */
            vf->scan_aux   = alloc_slice(s, (VkDeviceSize)SCENE_SCAN_BLOCK * 3u * sizeof(SceneU32x2), 16);
            vf->cull_args  = alloc_slice(s, 16, 16);
            vf->group_base = alloc_slice(s, (VkDeviceSize)s->max_survivors * sizeof(uint32_t), 16);
        }
    }
    /* group templates (one GpuDraw per (mesh,lod) group) */
    s->group_static = alloc_slice(s, (VkDeviceSize)s->group_count * sizeof(struct SceneGpuDraw), 16);
    s->group_mesh   = alloc_slice(s, (VkDeviceSize)s->group_count * sizeof(uint32_t), 16);
    {
        uint32_t *gm = calloc(s->group_count ? s->group_count : 1, sizeof(uint32_t));
        for (uint32_t g = 0; g < s->group_count; ++g)
            gm[g] = s->cpu_group_static[g].mesh_lod; /* mesh lives in bits 0..15 */
        renderer_upload_buffer_to_slice(vk, cmd, s->group_mesh, (ByteSpan){gm, s->group_count * sizeof(uint32_t)});
        free(gm);
    }
    renderer_upload_buffer_to_slice(vk, cmd, s->group_static,
                                    (ByteSpan){s->cpu_group_static, s->group_count * sizeof(struct SceneGpuDraw)});

    s->dirty_count = s->instance_count; /* upload everything once */

    /* No barrier here on purpose. Every table uploaded above is declared as a
       read by the pass that consumes it (T2 for instances/meshes/rows/lods,
       T3b for group_static, T5 for the vertex and index arenas), and begin_pass
       turns each declaration into a make-visible barrier whose source covers
       TRANSFER. The old code emitted one whole-pool barrier here instead, which
       ordered the same transfers while also implicating every unrelated
       allocation in the pool. */
    s->uploaded = true;
    log_info("[scene] uploaded %u instances, %u meshes, %u groups", s->instance_count, s->mesh_count, G);
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
    cfg.cull_mode              = VK_CULL_MODE_BACK_BIT;
    cfg.front_face             = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    cfg.color_attachment_count = 1;
    cfg.color_formats          = &s->color_format;
    cfg.depth_format           = s->depth_format;
    /* reverse-Z: near is 1.0 and the far limit is 0.0, so nearer geometry has
       the greater depth. Any Hi-Z pyramid built from this must MAX-reduce. */
    cfg.depth_compare_op = VK_COMPARE_OP_GREATER;
    cfg.blends[0]        = blend_disabled();
    s->draw_pipeline     = pipeline_create_graphics(s->vk, &cfg);
    s->pipelines_ready   = true;
}

static void build_scene_push(Scene *s, uint32_t view, uint32_t lane, struct ScenePush *p) {
    memset(p, 0, sizeof(*p));
    VkBackend      *vk = s->vk;
    SceneViewFrame *vf = &s->view_frame[view][lane];

    for (int r = 0; r < 4; ++r) {
        p->clip_rows[r].x = s->views[view].clip_rows[r][0];
        p->clip_rows[r].y = s->views[view].clip_rows[r][1];
        p->clip_rows[r].z = s->views[view].clip_rows[r][2];
        p->clip_rows[r].w = s->views[view].clip_rows[r][3];
    }
    p->sun.x        = s->sun[0];
    p->sun.y        = s->sun[1];
    p->sun.z        = s->sun[2];
    p->sun.w        = s->ambient;
    p->viewparams.x = s->views[view].camera_pos[0];
    p->viewparams.y = s->views[view].camera_pos[1];
    p->viewparams.z = s->views[view].camera_pos[2];
    p->viewparams.w = s->views[view].lod_target;

    p->instances      = slice_addr(vk, s->instances);
    p->cull_meshes    = slice_addr(vk, s->cull_meshes);
    p->cull_rows      = slice_addr(vk, s->cull_rows);
    p->shade_meshes   = slice_addr(vk, s->shade_meshes);
    p->lod_rows       = slice_addr(vk, s->lod_rows);
    p->materials      = slice_addr(vk, s->materials);
    p->survivors      = slice_addr(vk, vf->survivors);
    p->survivor_count = slice_addr(vk, vf->survivor_count);
    p->vis            = slice_addr(vk, vf->vis);
    p->draws          = slice_addr(vk, vf->draws);
    p->counters       = slice_addr(vk, s->counters[lane]);
    p->cull_args      = slice_addr(vk, vf->cull_args);
    p->group_base     = slice_addr(vk, vf->group_base);
    p->group_mesh     = slice_addr(vk, s->group_mesh);

    p->counts[0] = 1; /* lod_count */
    p->counts[1] = s->candidate_count;
    p->counts[2] = s->max_survivors;
}

void scene_frame(Scene *s, VkCommandBuffer cmd, RenderTarget *color, RenderTarget *depth) {
    if (!s || !s->uploaded || s->view_count == 0 || s->group_count == 0)
        return;
    VkBackend *vk   = s->vk;
    uint32_t   lane = vk->current_frame;

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

    /* Apply a pending roster change to the indirect dispatch args. This is the
       only place the CPU writes them, and only when the roster actually grew. */
    /* scene_frame */
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
       dirty list would degenerate to the same bytes plus the bookkeeping. */
    if (s->dynamic_count) {
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

    uint32_t G = s->group_count;

    for (uint32_t view = 0; view < s->view_count; ++view) {
        SceneViewFrame *vf = &s->view_frame[view][lane];

        /* Zero every GPU-written count before anything reads it. Declared as
           reads on the cull pass below, so begin_pass publishes them. */
        cmd_fill_buffer(cmd, vf->survivor_count, sizeof(uint32_t), 0u);
        cmd_fill_buffer(cmd, vf->group_count, (VkDeviceSize)G * sizeof(uint32_t), 0u);
        cmd_fill_buffer(cmd, vf->cursor, (VkDeviceSize)G * sizeof(uint32_t), 0u);
        cmd_fill_buffer(cmd, vf->draw_count, sizeof(uint32_t), 0u);
        cmd_fill_buffer(cmd, s->counters[lane], sizeof(uint32_t) * SCENE_COUNTERS, 0u);

        struct ScenePush sp;
        build_scene_push(s, view, lane, &sp);

        struct SceneCompactPush cp;
        memset(&cp, 0, sizeof(cp));
        cp.survivors      = slice_addr(vk, vf->survivors);
        cp.survivor_count = slice_addr(vk, vf->survivor_count);
        cp.group_count    = slice_addr(vk, vf->group_count);
        cp.vis_base       = slice_addr(vk, vf->vis_base);
        cp.cmd_index      = slice_addr(vk, vf->cmd_index);
        cp.cursor         = slice_addr(vk, vf->cursor);
        cp.vis            = slice_addr(vk, vf->vis);
        cp.draws          = slice_addr(vk, vf->draws);
        cp.group_static   = slice_addr(vk, s->group_static);
        cp.draw_count     = slice_addr(vk, vf->draw_count);
        cp.scan_aux       = slice_addr(vk, vf->scan_aux);
        cp.counters       = slice_addr(vk, s->counters[lane]);
        cp.group_base     = slice_addr(vk, vf->group_base);
        cp.counts[0]      = 1;
        cp.counts[1]      = G;
        cp.counts[2]      = s->max_survivors;
        cp.counts[4]      = (G + SCENE_SCAN_BLOCK - 1u) / SCENE_SCAN_BLOCK;
        /* DEBUG: shader-side printf. Off by default; it serialises and floods
           the log, so it is opt-in per run. */
        cp.counts[5] = getenv("MU_SHADER_DEBUG") ? 1u : 0u;
        if (getenv("MU_SHADER_DEBUG") && view == 0 && lane == 0)
            log_info("[dbg] shader printf enable = %u, G = %u", cp.counts[5], G);
        /* Every table is a slice of one buffer, so naming a slice names a byte
           range. That is the whole point of declaring these instead of
           barriering the pool: the previous code emitted four whole-pool
           barriers per view, implicating 512 MB of unrelated allocations to
           order 32 KB of counts. */
        BufferAccess cull_reads[7] = {
            {.slice  = s->instances,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = s->cull_meshes,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = s->cull_rows,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = s->lod_rows,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = vf->survivor_count,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
            {.slice  = s->counters[lane],
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
            {.slice  = vf->cull_args,
             .stage  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
             .access = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT},
        };
        BufferAccess cull_writes[2] = {
            {.slice  = vf->survivors,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
            {.slice  = vf->survivor_count,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
        };

        /* T2: cull */
        PassDesc cull_pass = {
            .buf_reads       = cull_reads,
            .buf_read_count  = ARRAY_COUNT(cull_reads),
            .buf_writes      = cull_writes,
            .buf_write_count = ARRAY_COUNT(cull_writes),
            .pipeline        = s->cs_cull,
        };
        begin_pass(vk, cmd, &cull_pass);
        dispatch_indirect(vk, cmd, (ByteSpan){&sp, (uint32_t)sizeof(sp)}, vf->cull_args);
        end_pass(vk, cmd, &cull_pass);

        /* T3a: histogram survivors by (mesh,lod) group */
        BufferAccess count_reads[2] = {
            {.slice  = vf->survivors,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = vf->survivor_count,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        };
        BufferAccess count_writes[1] = {
            {.slice  = vf->group_count,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
        };
        PassDesc count_pass = {
            .buf_reads       = count_reads,
            .buf_read_count  = ARRAY_COUNT(count_reads),
            .buf_writes      = count_writes,
            .buf_write_count = ARRAY_COUNT(count_writes),
            .pipeline        = s->cs_count,
        };
        begin_pass(vk, cmd, &count_pass);
        dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, (s->max_survivors + 63u) / 64u, 1, 1);
        end_pass(vk, cmd, &count_pass);

        /* T3b: scan group_count -> vis_base + draw commands + draw_count */
        /* T3b: two-level scan + emit. The old code ran this as one workgroup
           of one thread looping over all G groups; it is now O(G/1024) wide.
           scan_aux carries the per-block totals between the two levels. */
        BufferAccess scan_reads[2] = {
            {.slice  = vf->group_count,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = s->group_static,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        };
        BufferAccess scan_writes[8] = {
            {.slice  = vf->vis_base,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
            {.slice  = vf->cmd_index,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
            {.slice  = vf->cursor,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
            {.slice  = vf->draws,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
            {.slice  = vf->draw_count,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
            {.slice  = vf->scan_aux,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
            /* cs_scan_blocks mirrors the command count here; without this
               declaration the write is not published before the readback copy. */
            {.slice  = s->counters[lane],
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
            /* cs_emit records vis_base -> group id so the VS can resolve mesh
               from a uniform per-draw row instead of decoding a packed key */
            {.slice  = vf->group_base,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
        };
        PassDesc scan_pass = {
            .buf_reads       = scan_reads,
            .buf_read_count  = ARRAY_COUNT(scan_reads),
            .buf_writes      = scan_writes,
            .buf_write_count = ARRAY_COUNT(scan_writes),
            .pipeline        = s->cs_scan_block,
        };
        begin_pass(vk, cmd, &scan_pass);
        dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, cp.counts[4], 1, 1);
        end_pass(vk, cmd, &scan_pass);

        /* cs_scan_blocks READS scan_aux region 0 (the per-block totals cs_scan_block
           just wrote) and writes regions 1 and 2. Declaring writes only left the
           read unpublished, so it consumed stale/zero block offsets and every
           vis_base and cmd_index came out wrong — visible as duplicated and
           missing slots in vis[] that changed as the camera culled instances. */
        BufferAccess block_reads[1] = {
            {.slice  = vf->scan_aux,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        };
        PassDesc block_pass = {
            .buf_reads       = block_reads,
            .buf_read_count  = ARRAY_COUNT(block_reads),
            .buf_writes      = scan_writes,
            .buf_write_count = ARRAY_COUNT(scan_writes),
            .pipeline        = s->cs_scan_blocks,
        };
        begin_pass(vk, cmd, &block_pass);
        dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, 1, 1, 1);
        end_pass(vk, cmd, &block_pass);

        /* cs_emit reads group_count, group_static and the block offsets in
           scan_aux regions 1 and 2. */
        BufferAccess emit_reads[3] = {
            {.slice  = vf->group_count,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = s->group_static,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = vf->scan_aux,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        };
        PassDesc emit_pass = {
            .buf_reads       = emit_reads,
            .buf_read_count  = ARRAY_COUNT(emit_reads),
            .buf_writes      = scan_writes,
            .buf_write_count = ARRAY_COUNT(scan_writes),
            .pipeline        = s->cs_emit,
        };
        begin_pass(vk, cmd, &emit_pass);
        dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, cp.counts[4], 1, 1);
        end_pass(vk, cmd, &emit_pass);

        /* T3c: scatter survivors into their group's contiguous vis range */
        BufferAccess scatter_reads[4] = {
            {.slice  = vf->survivors,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = vf->survivor_count,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = vf->vis_base,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = vf->cursor,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
        };
        BufferAccess scatter_writes[1] = {
            {.slice  = vf->vis,
             .stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
        };
        PassDesc scatter_pass = {
            .buf_reads       = scatter_reads,
            .buf_read_count  = ARRAY_COUNT(scatter_reads),
            .buf_writes      = scatter_writes,
            .buf_write_count = ARRAY_COUNT(scatter_writes),
            .pipeline        = s->cs_scatter,
        };
        begin_pass(vk, cmd, &scatter_pass);
        dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, (s->max_survivors + 63u) / 64u, 1, 1);
        end_pass(vk, cmd, &scatter_pass);

        /* T5: draw */
        PassAttachment col = {.target = color, .load = LOAD_CLEAR, .store = STORE_KEEP};
        PassAttachment dep = {.target = depth, .load = LOAD_CLEAR, .store = STORE_KEEP};
        memcpy(col.clear, s->clear, sizeof(s->clear));
        dep.clear[0] = 0.0f; /* reverse-Z: the far plane is 0.0 */

        BufferAccess draw_reads[10] = {
            {.slice  = s->instances,
             .stage  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = s->vertex_arena,
             .stage  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = s->shade_meshes,
             .stage  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = s->materials,
             .stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = vf->vis,
             .stage  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = vf->draws,
             .stage  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
             .access = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT},
            {.slice  = vf->draw_count,
             .stage  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
             .access = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT},
            {.slice  = s->index_arena,
             .stage  = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
             .access = VK_ACCESS_2_INDEX_READ_BIT},
            {.slice  = vf->group_base,
             .stage  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
            {.slice  = s->group_mesh,
             .stage  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
             .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        };
        PassDesc draw_pass = {
            .colors         = &col,
            .color_count    = 1,
            .depth          = &dep,
            .buf_reads      = draw_reads,
            .buf_read_count = ARRAY_COUNT(draw_reads),
            .pipeline       = s->draw_pipeline,
        };
        begin_pass(vk, cmd, &draw_pass);

        vkCmdBindIndexBuffer(cmd, vk->gpu_pool.buffer, s->index_arena.offset, VK_INDEX_TYPE_UINT16);
        cmd_draw_indexed_indirect_count(vk, cmd, (ByteSpan){&sp, (uint32_t)sizeof(sp)}, vf->draws, vf->draw_count, G,
                                        (uint32_t)sizeof(struct SceneGpuDraw));
        end_pass(vk, cmd, &draw_pass);
    }

    /* copy counters to the host-visible lane buffer for next time */
    cmd_copy_buffer(cmd, s->counters[lane], s->counters_host[lane].buffer, 0, sizeof(uint32_t) * SCENE_COUNTERS);

    s->frame_serial++;
}

bool scene_counters(const Scene *s, SceneCounters *out) {
    if (!s || !s->has_counters)
        return false;
    *out = s->last_counters;
    return true;
}
