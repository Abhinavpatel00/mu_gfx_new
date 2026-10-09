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
} SceneCpuInstance;

typedef struct SceneViewFrame {
    BufferSlice survivors;
    BufferSlice survivor_count;
    BufferSlice group_count;
    BufferSlice vis_base;
    BufferSlice cursor;
    BufferSlice vis;
    BufferSlice draws;
    BufferSlice draw_count;
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

    /* CPU truth */
    SceneCpuInstance *cpu_instances;
    uint32_t          instance_count;
    uint32_t          candidate_count;

    uint32_t *dirty_slots;
    uint32_t  dirty_count;

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
    PipelineID cs_cull, cs_count, cs_prefix, cs_compact;
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

    s->staged_vertex_cap = 4096;
    s->staged_vertices   = malloc((size_t)s->staged_vertex_cap * sizeof(struct ScenePackedVertex));
    s->staged_index_cap  = 8192;
    s->staged_indices    = malloc((size_t)s->staged_index_cap * sizeof(uint16_t));

    s->cpu_instances = calloc(s->max_instances, sizeof(SceneCpuInstance));
    s->dirty_slots   = calloc(s->max_instances, sizeof(uint32_t));

    s->cpu_cull         = calloc(s->max_meshes, sizeof(struct SceneCullMesh));
    s->cpu_shade        = calloc(s->max_meshes, sizeof(struct SceneShadeMesh));
    s->cpu_lod          = calloc(s->max_lod_rows, sizeof(struct SceneLodRow));
    s->cpu_mat          = calloc(s->max_materials, sizeof(struct SceneGpuMaterial));
    s->cpu_rows         = calloc(s->max_instances, sizeof(struct SceneCullRow));
    s->cpu_group_static = calloc(s->max_meshes, sizeof(struct SceneGpuDraw));

    s->cs_cull    = pipeline_create_compute(vk, "compiledshaders/scene.cs_cull.comp.spv");
    s->cs_count   = pipeline_create_compute(vk, "compiledshaders/compact.cs_count.comp.spv");
    s->cs_prefix  = pipeline_create_compute(vk, "compiledshaders/compact.cs_prefix.comp.spv");
    s->cs_compact = pipeline_create_compute(vk, "compiledshaders/compact.cs_compact.comp.spv");

    log_info("[scene] created: max_instances=%u max_meshes=%u max_survivors=%u", s->max_instances, s->max_meshes,
             s->max_survivors);
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
    buffer_pool_free(s->vertex_arena);
    buffer_pool_free(s->index_arena);
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        buffer_pool_free(s->counters[i]);
        destroy_buffer(vk, &s->counters_host[i]);
    }
    forEach(v, s->view_count) forEach(i, MAX_FRAMES_IN_FLIGHT) {
        SceneViewFrame *vf = &s->view_frame[v][i];
        buffer_pool_free(vf->survivors);
        buffer_pool_free(vf->survivor_count);
        buffer_pool_free(vf->group_count);
        buffer_pool_free(vf->vis_base);
        buffer_pool_free(vf->cursor);
        buffer_pool_free(vf->vis);
        buffer_pool_free(vf->draws);
        buffer_pool_free(vf->draw_count);
    }
    free(s->staged_vertices);
    free(s->staged_indices);
    free(s->cpu_instances);
    free(s->dirty_slots);
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

uint32_t scene_instance_create(Scene *s, const SceneInstanceDesc *desc) {
    if (!s || s->instance_count >= s->max_instances)
        return UINT32_MAX;
    uint32_t slot = s->instance_count++;
    pack_instance(&s->cpu_instances[slot].gpu, desc);
    s->cpu_instances[slot].mesh      = desc->mesh;
    s->cpu_rows[slot].slot           = slot;
    s->cpu_rows[slot].mesh           = (uint16_t)desc->mesh;
    s->dirty_slots[s->dirty_count++] = slot;
    s->candidate_count               = s->instance_count;
    return slot;
}

void scene_instance_set(Scene *s, uint32_t slot, const SceneInstanceDesc *desc) {
    if (!s || slot >= s->max_instances)
        return;
    pack_instance(&s->cpu_instances[slot].gpu, desc);
    s->cpu_instances[slot].mesh      = desc->mesh;
    s->dirty_slots[s->dirty_count++] = slot;
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

    /* all instances */
    struct SceneInstance *tmp = malloc((size_t)s->instance_count * sizeof(struct SceneInstance));
    for (uint32_t i = 0; i < s->instance_count; ++i)
        tmp[i] = s->cpu_instances[i].gpu;
    if (s->instance_count)
        renderer_upload_buffer_to_slice(vk, cmd, s->instances,
                                        (ByteSpan){tmp, s->instance_count * sizeof(struct SceneInstance)});
    free(tmp);

    /* per-view, per-lane transient tables */

    /* per-view, per-lane transient tables */
    uint32_t G = s->group_count ? s->group_count : 1;
    for (uint32_t v = 0; v < SCENE_MAX_VIEWS; ++v) { /* <-- was s->view_count */
        forEach(i, MAX_FRAMES_IN_FLIGHT) {
            SceneViewFrame *vf = &s->view_frame[v][i];
            vf->survivors      = alloc_slice(s, (VkDeviceSize)s->max_survivors * sizeof(uint32_t), 16);
            vf->survivor_count = alloc_slice(s, sizeof(uint32_t), 16);
            vf->group_count    = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
            vf->vis_base       = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
            vf->cursor         = alloc_slice(s, (VkDeviceSize)G * sizeof(uint32_t), 16);
            vf->vis            = alloc_slice(s, (VkDeviceSize)s->max_survivors * sizeof(uint32_t), 16);
            vf->draws          = alloc_slice(s, (VkDeviceSize)G * sizeof(struct SceneGpuDraw), 16);
            vf->draw_count     = alloc_slice(s, sizeof(uint32_t), 16);
        }
    }
    /* group templates (one GpuDraw per (mesh,lod) group) */
    s->group_static = alloc_slice(s, (VkDeviceSize)s->group_count * sizeof(struct SceneGpuDraw), 16);
    renderer_upload_buffer_to_slice(vk, cmd, s->group_static,
                                    (ByteSpan){s->cpu_group_static, s->group_count * sizeof(struct SceneGpuDraw)});

    s->dirty_count = s->instance_count; /* upload everything once */
    cmd_buffer_barrier(cmd, vk->gpu_pool.buffer, 0, vk->gpu_pool.size_bytes, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                           VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

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
    cfg.depth_compare_op       = VK_COMPARE_OP_LESS; /* rh_no: near 0, far 1 */
    cfg.blends[0]              = blend_disabled();
    s->draw_pipeline           = pipeline_create_graphics(s->vk, &cfg);
    s->pipelines_ready         = true;
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
        s->has_counters                 = true;
    }

    /* T1: upload only the dirty instance rows (static instances cost 0). */
    for (uint32_t d = 0; d < s->dirty_count && d < s->max_instances; ++d) {
        uint32_t    slot = s->dirty_slots[d];
        BufferSlice row  = s->instances;
        row.offset       = s->instances.offset + (VkDeviceSize)slot * sizeof(struct SceneInstance);
        row.size         = sizeof(struct SceneInstance);
        row.mapped       = s->instances.mapped
                               ? (uint8_t *)s->instances.mapped + (VkDeviceSize)slot * sizeof(struct SceneInstance)
                               : NULL;
        renderer_upload_buffer_to_slice(vk, cmd, row,
                                        (ByteSpan){&s->cpu_instances[slot].gpu, sizeof(struct SceneInstance)});
    }
    s->dirty_count = 0;

    uint32_t G = s->group_count;

    for (uint32_t view = 0; view < s->view_count; ++view) {
        SceneViewFrame *vf = &s->view_frame[view][lane];

        /* zero the GPU-written counts (survivors, groups, cursors, counters, draws) */
        vkCmdFillBuffer(cmd, vf->survivor_count.buffer, vf->survivor_count.offset, sizeof(uint32_t), 0u);
        vkCmdFillBuffer(cmd, vf->group_count.buffer, vf->group_count.offset, (VkDeviceSize)G * sizeof(uint32_t), 0u);
        vkCmdFillBuffer(cmd, vf->cursor.buffer, vf->cursor.offset, (VkDeviceSize)G * sizeof(uint32_t), 0u);
        vkCmdFillBuffer(cmd, vf->draw_count.buffer, vf->draw_count.offset, sizeof(uint32_t), 0u);
        vkCmdFillBuffer(cmd, s->counters[lane].buffer, s->counters[lane].offset, sizeof(uint32_t) * SCENE_COUNTERS, 0u);

        cmd_buffer_barrier(cmd, vk->gpu_pool.buffer, 0, vk->gpu_pool.size_bytes, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        struct ScenePush sp;
        build_scene_push(s, view, lane, &sp);

        /* T2: cull */
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->render_pipelines.pipelines[s->cs_cull - 1]);
        dispatch_push(vk, cmd, (ByteSpan){&sp, (uint32_t)sizeof(sp)}, (s->candidate_count + 63u) / 64u, 1, 1);

        cmd_buffer_barrier(cmd, vk->gpu_pool.buffer, 0, vk->gpu_pool.size_bytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        /* T3: compact */
        struct SceneCompactPush cp;
        memset(&cp, 0, sizeof(cp));
        cp.survivors      = slice_addr(vk, vf->survivors);
        cp.survivor_count = slice_addr(vk, vf->survivor_count);
        cp.group_count    = slice_addr(vk, vf->group_count);
        cp.vis_base       = slice_addr(vk, vf->vis_base);
        cp.cursor         = slice_addr(vk, vf->cursor);
        cp.vis            = slice_addr(vk, vf->vis);
        cp.draws          = slice_addr(vk, vf->draws);
        cp.group_static   = slice_addr(vk, s->group_static);
        cp.draw_count     = slice_addr(vk, vf->draw_count);
        cp.counts[0]      = 1;
        cp.counts[1]      = G;
        cp.counts[2]      = s->max_survivors;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->render_pipelines.pipelines[s->cs_count - 1]);
        dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, (s->max_survivors + 63u) / 64u, 1, 1);

        cmd_buffer_barrier(cmd, vk->gpu_pool.buffer, 0, vk->gpu_pool.size_bytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->render_pipelines.pipelines[s->cs_prefix - 1]);
        dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, 1, 1, 1);

        cmd_buffer_barrier(cmd, vk->gpu_pool.buffer, 0, vk->gpu_pool.size_bytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->render_pipelines.pipelines[s->cs_compact - 1]);
        dispatch_push(vk, cmd, (ByteSpan){&cp, (uint32_t)sizeof(cp)}, (s->max_survivors + 63u) / 64u, 1, 1);

        cmd_buffer_barrier(cmd, vk->gpu_pool.buffer, 0, vk->gpu_pool.size_bytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

        /* T5: draw */
        PassAttachment col = {.target = color, .load = LOAD_CLEAR, .store = STORE_KEEP};
        PassAttachment dep = {.target = depth, .load = LOAD_CLEAR, .store = STORE_KEEP};
        memcpy(col.clear, s->clear, sizeof(s->clear));
        dep.clear[0] = 1.0f;
        begin_pass(vk, cmd, &(PassDesc){.colors = &col, .color_count = 1, .depth = &dep, .pipeline = s->draw_pipeline});

        vkCmdBindIndexBuffer(cmd, vk->gpu_pool.buffer, s->index_arena.offset, VK_INDEX_TYPE_UINT16);
        cmd_draw_indexed_indirect_count(vk, cmd, (ByteSpan){&sp, (uint32_t)sizeof(sp)}, vf->draws, vf->draw_count, G,
                                        (uint32_t)sizeof(struct SceneGpuDraw));
        end_pass(cmd);
    }

    /* copy counters to the host-visible lane buffer for next time */
    VkBufferCopy copy = {
        .srcOffset = s->counters[lane].offset, .dstOffset = 0, .size = sizeof(uint32_t) * SCENE_COUNTERS};
    vkCmdCopyBuffer(cmd, vk->gpu_pool.buffer, s->counters_host[lane].buffer, 1, &copy);

    s->frame_serial++;
}

bool scene_counters(const Scene *s, SceneCounters *out) {
    if (!s || !s->has_counters)
        return false;
    *out = s->last_counters;
    return true;
}
