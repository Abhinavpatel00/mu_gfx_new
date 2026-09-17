#include "renderer3d.h"
#include <stddef.h>

_Static_assert(sizeof(ScenePackedVertex) == 16, "packed vertex stride");
_Static_assert(sizeof(SceneInstance) == 64, "instance stride");
_Static_assert(sizeof(SceneDraw) == 20, "indirect stride");
_Static_assert(offsetof(ScenePush, clip_rows) == 64 && sizeof(ScenePush) == 144, "push layout");

typedef struct SceneMesh {
    BufferSlice slice;
    SceneVector bounds;
    uint32_t index_count, first_index, index_type, references;
    void *pending;
} SceneMesh;
typedef struct SceneMaterial { SceneVector color; uint32_t references, dirty_slots; } SceneMaterial;
typedef struct SceneEntry { InstanceDesc desc; uint32_t handle; } SceneEntry;
typedef struct SceneRef { uint32_t instance, mesh, material, index_type; } SceneRef;
typedef struct SceneFrame {
    Buffer data, commands;
    ScenePush push;
    uint32_t *dirty;
    uint8_t *marked;
    uint32_t dirty_count;
    bool topology, materials;
} SceneFrame;
struct Renderer3DState {
    mu_bulk_storage meshes, materials, handles;
    SceneEntry *entries;
    SceneInstance *instances;
    SceneVector *bounds;
    SceneRef *refs;
    SceneCandidate *candidates;
    SceneDraw *draws;
    uint32_t count, capacity, material_capacity, batches, narrow_batches;
    VkDeviceAddress geometry_address;
    VkDeviceSize bounds_offset, materials_offset, candidates_offset, visible_offset;
    SceneFrame frames[MAX_FRAMES_IN_FLIGHT];
    VkPipeline cull_pipeline, draw_pipeline;
    bool topology, pending_geometry;
    Render3DStats stats;
};

static void scene_dirty(Renderer3DState *s, uint32_t dense) {
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        SceneFrame *f = &s->frames[i];
        if (!f->marked[dense]) {
            f->marked[dense] = 1;
            f->dirty[f->dirty_count++] = dense;
        }
    }
}
static void scene_topology(Renderer3DState *s) {
    s->topology = true;
    forEach(i, MAX_FRAMES_IN_FLIGHT) s->frames[i].topology = true;
}
static void scene_upload(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, ByteSpan bytes) {
    for (uint32_t at = 0; at < bytes.size; at += 65536) {
        vkCmdUpdateBuffer(cmd, buffer, offset + at, MIN(65536, bytes.size - at),
                          (const uint8_t *)bytes.data + at);
    }
}

bool renderer3d_create(VkBackend *vk, Renderer3D *scene, const Renderer3DDesc *desc) {
    *scene = (Renderer3D){0};
    assert(desc->max_instances && desc->max_materials);
    if (((uint64_t)desc->max_instances + 63) / 64 > vk->info.properties.limits.maxComputeWorkGroupCount[0]) {
        log_error("[3d] instance budget exceeds GPU culling dispatch capacity");
        return false;
    }
    if (!vk->info.feature_chain.core.features.multiDrawIndirect ||
        !vk->info.feature_chain.core.features.drawIndirectFirstInstance ||
        !vk->info.feature_chain.v12.bufferDeviceAddress || !vk->info.feature_chain.v11.shaderDrawParameters ||
        !(vk->gpu_pool.usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
        log_error("[3d] indirect drawing, shader draw parameters, device addresses and indexed GPU pool required");
        return false;
    }
    Renderer3DState *s = scene->state = calloc(1, sizeof(*s));
    s->capacity = desc->max_instances;
    s->geometry_address = vk->gpu_base_addr;
    s->material_capacity = desc->max_materials;
    mu_bulk_storage_init(&s->meshes, sizeof(SceneMesh), 64);
    mu_bulk_storage_init(&s->materials, sizeof(SceneMaterial), desc->max_materials + 1);
    mu_bulk_storage_init(&s->handles, sizeof(uint32_t), desc->max_instances + 1);
    s->entries = calloc(s->capacity, sizeof(*s->entries));
    s->instances = calloc(s->capacity, sizeof(*s->instances));
    s->bounds = calloc(s->capacity, sizeof(*s->bounds));
    s->refs = malloc(s->capacity * sizeof(*s->refs));
    s->candidates = malloc(s->capacity * sizeof(*s->candidates));
    s->draws = malloc(s->capacity * sizeof(*s->draws));
    s->bounds_offset = s->capacity * sizeof(SceneInstance);
    s->materials_offset = s->bounds_offset + s->capacity * sizeof(SceneVector);
    s->candidates_offset = s->materials_offset + (desc->max_materials + 1) * sizeof(SceneVector);
    s->visible_offset = s->capacity * sizeof(SceneDraw);
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        SceneFrame *f = &s->frames[i];
        f->dirty = malloc(s->capacity * sizeof(uint32_t));
        f->marked = calloc(s->capacity, 1);
        if (!create_device_buffer(vk, s->candidates_offset + s->capacity * sizeof(SceneCandidate),
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &f->data)) goto fail;
        if (!create_device_buffer(vk, s->visible_offset + s->capacity * sizeof(uint32_t),
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, &f->commands)) goto fail;
    }
    s->cull_pipeline = create_compute_pipeline(vk, "compiledshaders/scene3d.comp.spv");
    if (!s->cull_pipeline) goto fail;
    GraphicsPipelineConfig config = pipeline_config_default();
    config.vert_path = "compiledshaders/scene3d.vert.spv";
    config.frag_path = "compiledshaders/scene3d.frag.spv";
    config.color_attachment_count = 1;
    config.color_formats = &desc->color_format;
    config.depth_format = desc->depth_format;
    config.depth_compare_op = VK_COMPARE_OP_LESS;
    config.blends[0] = blend_disabled();
    s->draw_pipeline = create_graphics_pipeline(vk, &config);
    if (!s->draw_pipeline) goto fail;
    return true;
fail:
    renderer3d_destroy(vk, scene);
    return false;
}
void renderer3d_destroy(VkBackend *vk, Renderer3D *scene) {
    Renderer3DState *s = scene->state;
    if (!s) return;
    if (s->draw_pipeline) vkDestroyPipeline(vk->devc.device, s->draw_pipeline, NULL);
    if (s->cull_pipeline) vkDestroyPipeline(vk->devc.device, s->cull_pipeline, NULL);
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        destroy_buffer(vk, &s->frames[i].data);
        destroy_buffer(vk, &s->frames[i].commands);
        free(s->frames[i].dirty);
        free(s->frames[i].marked);
    }
    for (uint32_t i = 1; i < s->meshes.next_unused; ++i) {
        SceneMesh *mesh = mu_bulk_storage_ptr(&s->meshes, i);
        if (mesh) { buffer_pool_free(mesh->slice); free(mesh->pending); }
    }
    mu_bulk_storage_deinit(&s->meshes);
    mu_bulk_storage_deinit(&s->materials);
    mu_bulk_storage_deinit(&s->handles);
    free(s->entries); free(s->instances); free(s->bounds);
    free(s->refs); free(s->candidates); free(s->draws);
    free(s);
    scene->state = NULL;
}

MeshId renderer3d_mesh_create(VkBackend *vk, Renderer3D *scene, const MeshDesc *desc) {
    Renderer3DState *s = scene->state;
    if (!desc->vertices.data || !desc->indices.data || !desc->vertices.size || !desc->indices.size ||
        desc->vertices.size % sizeof(SceneVertex) || desc->indices.size % (3 * sizeof(uint32_t))) return (MeshId){0};
    const SceneVertex *vertices = desc->vertices.data;
    const uint32_t *indices = desc->indices.data;
    uint32_t vertex_count = desc->vertices.size / sizeof(SceneVertex);
    uint32_t index_count = desc->indices.size / sizeof(uint32_t), max_index = 0;
    forEach(i, index_count) {
        if (indices[i] >= vertex_count) return (MeshId){0};
        max_index = MAX(max_index, indices[i]);
    }
    forEach(i, vertex_count) {
        forEach(j, 3) if (!isfinite(vertices[i].position[j]) || fabsf(vertices[i].position[j]) > 65504 ||
                          !isfinite(vertices[i].normal[j])) return (MeshId){0};
        forEach(j, 2) if (!isfinite(vertices[i].uv[j]) || fabsf(vertices[i].uv[j]) > 65504) return (MeshId){0};
        if (!(glm_vec3_norm2((float *)vertices[i].normal) > 0)) return (MeshId){0};
    }
    uint32_t index_size = max_index <= UINT16_MAX ? 2 : 4;
    uint32_t vertex_bytes = vertex_count * sizeof(ScenePackedVertex);
    uint32_t bytes = (vertex_bytes + index_count * index_size + 3) & ~3u;
    BufferSlice slice = buffer_pool_alloc(&vk->gpu_pool, bytes, 16);
    if (!slice.buffer) return (MeshId){0};
    ScenePackedVertex *packed = calloc(1, bytes);
    float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
    forEach(i, vertex_count) {
        uint16_t p[3];
        forEach(j, 3) {
            p[j] = mu_quantize_half(vertices[i].position[j]);
            float decoded = mu_dequantize_half(p[j]);
            low[j] = MIN(low[j], decoded); high[j] = MAX(high[j], decoded);
        }
        float normal[3];
        glm_vec3_scale((float *)vertices[i].normal, 1 / (fabsf(vertices[i].normal[0]) +
                       fabsf(vertices[i].normal[1]) + fabsf(vertices[i].normal[2])), normal);
        if (normal[2] < 0) {
            float x = (1 - fabsf(normal[1])) * (normal[0] < 0 ? -1 : 1);
            normal[1] = (1 - fabsf(normal[0])) * (normal[1] < 0 ? -1 : 1); normal[0] = x;
        }
        packed[i] = (ScenePackedVertex){.position_xy = p[0] | ((uint32_t)p[1] << 16), .position_z = p[2],
            .normal_oct = (uint16_t)mu_quantize_snorm(normal[0], 16) |
                         ((uint32_t)(uint16_t)mu_quantize_snorm(normal[1], 16) << 16),
            .uv = mu_quantize_half(vertices[i].uv[0]) | ((uint32_t)mu_quantize_half(vertices[i].uv[1]) << 16)};
    }
    if (index_size == 2) {
        uint16_t *out = (uint16_t *)((uint8_t *)packed + vertex_bytes);
        forEach(i, index_count) out[i] = (uint16_t)indices[i];
    } else memcpy((uint8_t *)packed + vertex_bytes, indices, desc->indices.size);
    uint32_t id = mu_bulk_storage_alloc(&s->meshes);
    SceneMesh *mesh = mu_bulk_storage_ptr(&s->meshes, id);
    *mesh = (SceneMesh){.slice = slice, .pending = packed, .index_count = index_count,
        .first_index = (uint32_t)((slice.offset + vertex_bytes) / index_size), .index_type = index_size == 2 ? 0 : 1,
        .bounds = {(low[0]+high[0])*0.5f, (low[1]+high[1])*0.5f, (low[2]+high[2])*0.5f,
                   glm_vec3_distance(low, high)*0.5f}};
    s->stats.geometry_live_bytes += bytes;
    s->pending_geometry = true;
    return (MeshId){mu_bulk_storage_make_handle(&s->meshes, id)};
}
void renderer3d_mesh_destroy(VkBackend *vk, Renderer3D *scene, MeshId id) {
    Renderer3DState *s = scene->state;
    SceneMesh *mesh = mu_bulk_storage_resolve_handle(&s->meshes, id.handle);
    assert(mesh && !mesh->references);
    s->stats.geometry_live_bytes -= mesh->slice.size;
    buffer_pool_free(mesh->slice); free(mesh->pending);
    mu_bulk_storage_free(&s->meshes, id.handle.id);
}


MaterialId renderer3d_material_create(Renderer3D *scene, SceneVector color) {
    Renderer3DState *s = scene->state;
    if (s->materials.live_count == s->material_capacity) return (MaterialId){0};
    uint32_t id = mu_bulk_storage_alloc(&s->materials);
    *(SceneMaterial *)mu_bulk_storage_ptr(&s->materials, id) = (SceneMaterial){
        .color = color, .dirty_slots = (1u << MAX_FRAMES_IN_FLIGHT) - 1};
    forEach(i, MAX_FRAMES_IN_FLIGHT) s->frames[i].materials = true;
    return (MaterialId){mu_bulk_storage_make_handle(&s->materials, id)};
}
void renderer3d_material_destroy(Renderer3D *scene, MaterialId id) {
    Renderer3DState *s = scene->state;
    SceneMaterial *material = mu_bulk_storage_resolve_handle(&s->materials, id.handle);
    assert(material && !material->references);
    mu_bulk_storage_free(&s->materials, id.handle.id);
}
void renderer3d_material_update(Renderer3D *scene, MaterialId id, SceneVector color) {
    Renderer3DState *s = scene->state;
    SceneMaterial *material = mu_bulk_storage_resolve_handle(&s->materials, id.handle);
    assert(material);
    material->color = color;
    material->dirty_slots = (1u << MAX_FRAMES_IN_FLIGHT) - 1;
    forEach(i, MAX_FRAMES_IN_FLIGHT) s->frames[i].materials = true;
}

void renderer3d_instance_update(Renderer3D *scene, InstanceId id, const InstanceDesc *desc) {
    Renderer3DState *s = scene->state;
    uint32_t *dense = mu_bulk_storage_resolve_handle(&s->handles, id.handle);
    SceneMesh *mesh = mu_bulk_storage_resolve_handle(&s->meshes, desc->mesh.handle);
    SceneMaterial *material = mu_bulk_storage_resolve_handle(&s->materials, desc->material.handle);
    assert(dense && mesh && material);
    SceneEntry *entry = &s->entries[*dense];
    if (entry->desc.mesh.handle.id) {
        SceneMesh *old_mesh = mu_bulk_storage_ptr(&s->meshes, entry->desc.mesh.handle.id);
        SceneMaterial *old_material = mu_bulk_storage_ptr(&s->materials, entry->desc.material.handle.id);
        --old_mesh->references; --old_material->references;
    }
    ++mesh->references; ++material->references;
    if (entry->desc.mesh.handle.id != desc->mesh.handle.id ||
        entry->desc.material.handle.id != desc->material.handle.id) scene_topology(s);
    entry->desc = *desc;
    SceneInstance *gpu = &s->instances[*dense];
    memcpy(gpu->rows, desc->rows, sizeof(gpu->rows));
    gpu->vertices = s->geometry_address + mesh->slice.offset;
    gpu->material = desc->material.handle.id;
    gpu->tint = (uint32_t)mu_quantize_unorm(desc->tint.x, 8) |
                ((uint32_t)mu_quantize_unorm(desc->tint.y, 8) << 8) |
                ((uint32_t)mu_quantize_unorm(desc->tint.z, 8) << 16) |
                ((uint32_t)mu_quantize_unorm(desc->tint.w, 8) << 24);
    const SceneVector *r = desc->rows;
    float cofactor[3];
    glm_vec3_cross((float *)&r[1], (float *)&r[2], cofactor);
    assert(glm_vec3_dot((float *)&r[0], cofactor) > 0);
    SceneVector *bounds = &s->bounds[*dense];
    bounds->x = r[0].x*mesh->bounds.x + r[0].y*mesh->bounds.y + r[0].z*mesh->bounds.z + r[0].w;
    bounds->y = r[1].x*mesh->bounds.x + r[1].y*mesh->bounds.y + r[1].z*mesh->bounds.z + r[1].w;
    bounds->z = r[2].x*mesh->bounds.x + r[2].y*mesh->bounds.y + r[2].z*mesh->bounds.z + r[2].w;
    // sqrt(||A||1 * ||A||inf) bounds the spectral norm even for sheared transforms.
    float columns = MAX(fabsf(r[0].x)+fabsf(r[1].x)+fabsf(r[2].x),
                        MAX(fabsf(r[0].y)+fabsf(r[1].y)+fabsf(r[2].y), fabsf(r[0].z)+fabsf(r[1].z)+fabsf(r[2].z)));
    float rows = MAX(fabsf(r[0].x)+fabsf(r[0].y)+fabsf(r[0].z),
                     MAX(fabsf(r[1].x)+fabsf(r[1].y)+fabsf(r[1].z), fabsf(r[2].x)+fabsf(r[2].y)+fabsf(r[2].z)));
    bounds->w = mesh->bounds.w * sqrtf(columns * rows);
    scene_dirty(s, *dense);
}
InstanceId renderer3d_instance_create(Renderer3D *scene, const InstanceDesc *desc) {
    Renderer3DState *s = scene->state;
    if (s->count == s->capacity) return (InstanceId){0};
    uint32_t id = mu_bulk_storage_alloc(&s->handles);
    *(uint32_t *)mu_bulk_storage_ptr(&s->handles, id) = s->count;
    s->entries[s->count++] = (SceneEntry){.handle = id};
    InstanceId instance = {mu_bulk_storage_make_handle(&s->handles, id)};
    renderer3d_instance_update(scene, instance, desc);
    scene_topology(s);
    return instance;
}
void renderer3d_instance_destroy(Renderer3D *scene, InstanceId id) {
    Renderer3DState *s = scene->state;
    uint32_t *dense = mu_bulk_storage_resolve_handle(&s->handles, id.handle);
    assert(dense);
    SceneEntry *entry = &s->entries[*dense];
    --((SceneMesh *)mu_bulk_storage_ptr(&s->meshes, entry->desc.mesh.handle.id))->references;
    --((SceneMaterial *)mu_bulk_storage_ptr(&s->materials, entry->desc.material.handle.id))->references;
    --s->count;
    if (*dense != s->count) {
        *entry = s->entries[s->count];
        s->instances[*dense] = s->instances[s->count];
        s->bounds[*dense] = s->bounds[s->count];
        *(uint32_t *)mu_bulk_storage_ptr(&s->handles, entry->handle) = *dense;
        scene_dirty(s, *dense);
    }
    mu_bulk_storage_free(&s->handles, id.handle.id);
    scene_topology(s);
}
bool renderer3d_instance_get(const Renderer3D *scene, InstanceId id, InstanceDesc *out) {
    const uint32_t *dense = mu_bulk_storage_resolve_handle_const(&scene->state->handles, id.handle);
    if (!dense) return false;
    *out = scene->state->entries[*dense].desc;
    return true;
}

static int scene_ref_compare(const void *left, const void *right) {
    const SceneRef *a = left, *b = right;
    if (a->index_type != b->index_type) return a->index_type < b->index_type ? -1 : 1;
    if (a->mesh != b->mesh) return a->mesh < b->mesh ? -1 : 1;
    return (a->material > b->material) - (a->material < b->material);
}
static int scene_index_compare(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}
void renderer3d_prepare(VkBackend *vk, Renderer3D *scene, VkCommandBuffer cmd, const SceneView *view) {
    Renderer3DState *s = scene->state;
    SceneFrame *f = &s->frames[vk->current_frame];
    s->stats.scene_upload_bytes = s->stats.geometry_upload_bytes = 0;
    if (s->pending_geometry) {
        for (uint32_t i = 1; i < s->meshes.next_unused; ++i) {
            SceneMesh *mesh = mu_bulk_storage_ptr(&s->meshes, i);
            if (!mesh || !mesh->pending) continue;
            scene_upload(cmd, mesh->slice.buffer, mesh->slice.offset,
                         (ByteSpan){.data = mesh->pending, .size = (uint32_t)mesh->slice.size});
            s->stats.geometry_upload_bytes += mesh->slice.size;
            free(mesh->pending); mesh->pending = NULL;
        }
        s->pending_geometry = false;
    }
    if (s->topology) {
        forEach(i, s->count) {
            SceneEntry *entry = &s->entries[i];
            SceneMesh *mesh = mu_bulk_storage_ptr(&s->meshes, entry->desc.mesh.handle.id);
            s->refs[i] = (SceneRef){.instance=i, .mesh=entry->desc.mesh.handle.id,
                .material=entry->desc.material.handle.id, .index_type=mesh->index_type};
        }
        qsort(s->refs, s->count, sizeof(*s->refs), scene_ref_compare);
        s->batches = s->narrow_batches = 0;
        forEach(i, s->count) {
            SceneRef *ref = &s->refs[i];
            if (!i || scene_ref_compare(ref, &s->refs[i-1])) {
                SceneMesh *mesh = mu_bulk_storage_ptr(&s->meshes, ref->mesh);
                s->draws[s->batches++] = (SceneDraw){.index_count=mesh->index_count,
                    .first_index=mesh->first_index, .first_instance=i};
                s->narrow_batches += ref->index_type == 0;
            }
            s->candidates[i] = (SceneCandidate){.instance=ref->instance, .batch=s->batches-1};
        }
        s->topology = false;
    }
    qsort(f->dirty, f->dirty_count, sizeof(uint32_t), scene_index_compare);
    for (uint32_t i = 0; i < f->dirty_count;) {
        uint32_t first = f->dirty[i], end = first + 1;
        f->marked[first] = 0;
        ++i;
        while (i < f->dirty_count && f->dirty[i] == end) {
            f->marked[end++] = 0; ++i;
        }
        end = MIN(end, s->count);
        if (first >= end) continue;
        scene_upload(cmd, f->data.buffer, first * sizeof(SceneInstance),
            (ByteSpan){.data=s->instances+first, .size=(end-first)*sizeof(SceneInstance)});
        scene_upload(cmd, f->data.buffer, s->bounds_offset + first * sizeof(SceneVector),
            (ByteSpan){.data=s->bounds+first, .size=(end-first)*sizeof(SceneVector)});
        s->stats.scene_upload_bytes += (end-first) * (sizeof(SceneInstance)+sizeof(SceneVector));
    }
    f->dirty_count = 0;
    if (f->materials) {
        for (uint32_t i = 1; i < s->materials.next_unused; ++i) {
            SceneMaterial *material = mu_bulk_storage_ptr(&s->materials, i);
            if (!material || !(material->dirty_slots & (1u << vk->current_frame))) continue;
            material->dirty_slots &= ~(1u << vk->current_frame);
            scene_upload(cmd, f->data.buffer, s->materials_offset + i*sizeof(SceneVector), BYTE_SPAN(material->color));
            s->stats.scene_upload_bytes += sizeof(SceneVector);
        }
        f->materials = false;
    }
    if (f->topology) {
        if (s->count) scene_upload(cmd, f->data.buffer, s->candidates_offset,
            (ByteSpan){.data=s->candidates, .size=s->count*sizeof(SceneCandidate)});
        s->stats.scene_upload_bytes += s->count*sizeof(SceneCandidate);
        f->topology = false;
    }
    if (s->batches) scene_upload(cmd, f->commands.buffer, 0,
        (ByteSpan){.data=s->draws, .size=s->batches*sizeof(SceneDraw)});
    s->stats.scene_upload_bytes += s->batches*sizeof(SceneDraw);
    f->push = (ScenePush){.instances=f->data.address, .bounds=f->data.address+s->bounds_offset,
        .materials=f->data.address+s->materials_offset, .candidates=f->data.address+s->candidates_offset,
        .commands=f->commands.address, .visible=f->commands.address+s->visible_offset,
        .instance_count=s->count, .sun=view->sun};
    memcpy(f->push.clip_rows, view->clip_rows, sizeof(f->push.clip_rows));

    VkMemoryBarrier2 barrier = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, .srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
        .dstAccessMask=VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                       VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT};
    vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount=1, .pMemoryBarriers=&barrier});
    if (s->count) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->cull_pipeline);
        dispatch_push(vk, cmd, BYTE_SPAN(f->push), (s->count+63)/64, 1, 1);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount=1, .pMemoryBarriers=&barrier});
    }
    s->stats.instances = s->count;
    s->stats.batches = s->batches;
}
void renderer3d_record(VkBackend *vk, Renderer3D *scene, VkCommandBuffer cmd, RenderTarget *color, RenderTarget *depth) {
    Renderer3DState *s = scene->state;
    SceneFrame *f = &s->frames[vk->current_frame];
    PassAttachment attachment = {.target=color, .load=LOAD_CLEAR, .clear={0.02f,0.025f,0.03f,1}};
    PassAttachment z = {.target=depth, .load=LOAD_CLEAR, .clear={1}};
    begin_pass(vk, cmd, &(PassDesc){.colors=&attachment, .color_count=1, .depth=&z});
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->draw_pipeline);
    push_constants(vk, cmd, BYTE_SPAN(f->push));
    forEach(type, 2) {
        uint32_t first = type ? s->narrow_batches : 0;
        uint32_t end = type ? s->batches : s->narrow_batches;
        if (first == end) continue;
        vkCmdBindIndexBuffer(cmd, vk->gpu_pool.buffer, 0, type ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        while (first < end) {
            uint32_t count = MIN(end-first, vk->info.properties.limits.maxDrawIndirectCount);
            vkCmdDrawIndexedIndirect(cmd, f->commands.buffer, first*sizeof(SceneDraw), count, sizeof(SceneDraw));
            first += count;
        }
    }
    end_pass(cmd);
}
Render3DStats renderer3d_stats(const Renderer3D *scene) { return scene->state->stats; }
