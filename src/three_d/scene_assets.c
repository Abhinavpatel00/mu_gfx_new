#include "scene.h"
#include "scene_internal.h"

#include "../../external/cgltf/cgltf.h"
#include "../../external/stb/stb_image.h"
#include "../../external/meshoptimizer/src/meshoptimizer.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENE_INDEX_POOL_MAX (64u * 1024u * 1024u)
#define SCENE_LOD_LEVELS 4

typedef struct LodGroup {
    SceneLodRow rows[SCENE_MAX_LOD_LEVELS];
    uint32_t    count;
} LodGroup;

typedef struct SceneAssetData {
    SceneGpuMesh     *meshes;
    uint32_t          mesh_count;
    SceneGpuMaterial *materials;
    uint32_t          material_count;
    SceneMeshSet     *mesh_sets;
    uint32_t          mesh_set_count;
    LodGroup         *lod_groups;
    BufferSlice      *vertex_slices;
    uint32_t          vertex_slice_count;
    BufferSlice      *skin_spans;
    uint32_t          skin_span_count;
    SceneSkinRecord  *skin_mirror;
    uint32_t         *slot_skin;
    uint16_t         *index_mirror;
    uint32_t          index_used;
    uint64_t          index_stream_addr;
    BufferSlice       index_slice;
} SceneAssetData;

static SceneAssetData g_assets;

uint32_t scene_mesh_set_add(Scene *s, const MeshSetDesc *desc) {
    (void)s;
    if (g_assets.mesh_set_count >= SCENE_MAX_MESH_SETS)
        return UINT32_MAX;
    if (!desc || !desc->mesh_slots || desc->mesh_count == 0)
        return UINT32_MAX;
    if (desc->mesh_count > MAX_MESHES_PER_SET)
        return UINT32_MAX;

    uint32_t id = g_assets.mesh_set_count++;
    g_assets.mesh_sets[id].first = desc->mesh_slots[0];
    g_assets.mesh_sets[id].count = desc->mesh_count;
    return id;
}

uint32_t scene_material_add(Scene *s, const MaterialDesc *desc) {
    (void)s;
    if (g_assets.material_count >= SCENE_MAX_MATERIALS)
        return UINT32_MAX;
    if (!desc)
        return UINT32_MAX;

    uint32_t id = g_assets.material_count++;
    g_assets.materials[id].base_color = desc->base_color;
    g_assets.materials[id].texture = desc->albedo_texture;
    g_assets.materials[id].flags = desc->flags;
    return id;
}

uint64_t scene_index_stream(const Scene *s) {
    (void)s;
    return g_assets.index_stream_addr;
}

BufferSlice scene_index_pool(void) { return g_assets.index_slice; }

void scene_assets_init(VkBackend *vk) {
    g_assets.meshes = (SceneGpuMesh *)calloc(SCENE_MAX_MESH_SLOTS, sizeof(SceneGpuMesh));
    g_assets.materials = (SceneGpuMaterial *)calloc(SCENE_MAX_MATERIALS, sizeof(SceneGpuMaterial));
    g_assets.mesh_sets = (SceneMeshSet *)calloc(SCENE_MAX_MESH_SETS, sizeof(SceneMeshSet));
    g_assets.lod_groups = (LodGroup *)calloc(SCENE_MAX_MESH_SLOTS, sizeof(LodGroup));
    g_assets.vertex_slices = (BufferSlice *)calloc(SCENE_MAX_MESH_SLOTS, sizeof(BufferSlice));
    g_assets.skin_spans = (BufferSlice *)calloc(SCENE_MAX_MESH_SLOTS, sizeof(BufferSlice));
    g_assets.skin_mirror = (SceneSkinRecord *)calloc(SCENE_MAX_MESH_SLOTS, sizeof(SceneSkinRecord));
    g_assets.slot_skin = (uint32_t *)malloc(SCENE_MAX_MESH_SLOTS * sizeof(uint32_t));
    for (uint32_t i = 0; i < SCENE_MAX_MESH_SLOTS; i++)
        g_assets.slot_skin[i] = UINT32_MAX;
    g_assets.index_mirror = (uint16_t *)malloc((size_t)SCENE_INDEX_POOL_MAX * sizeof(uint16_t));

    VkDeviceSize index_bytes = (VkDeviceSize)SCENE_INDEX_POOL_MAX * sizeof(uint16_t);
    g_assets.index_slice = buffer_pool_alloc(&vk->gpu_pool, index_bytes, 16);
    g_assets.index_stream_addr = vk->gpu_base_addr + g_assets.index_slice.offset;
}

void scene_assets_destroy(VkBackend *vk) {
    (void)vk;
    for (uint32_t i = 0; i < g_assets.vertex_slice_count; i++)
        buffer_pool_free(g_assets.vertex_slices[i]);
    for (uint32_t i = 0; i < g_assets.skin_span_count; i++)
        buffer_pool_free(g_assets.skin_spans[i]);
    buffer_pool_free(g_assets.index_slice);
    free(g_assets.meshes);
    free(g_assets.materials);
    free(g_assets.mesh_sets);
    free(g_assets.lod_groups);
    free(g_assets.vertex_slices);
    free(g_assets.skin_spans);
    free(g_assets.skin_mirror);
    free(g_assets.slot_skin);
    free(g_assets.index_mirror);
    memset(&g_assets, 0, sizeof(g_assets));
}

static uint32_t pack_half2(float a, float b) {
    return (uint32_t)mu_quantize_half(a) | ((uint32_t)mu_quantize_half(b) << 16);
}

static uint32_t pack_oct16(float x, float y, float z) {
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0.0f) {
        x /= len;
        y /= len;
        z /= len;
    }
    float inv = 1.0f / (fabsf(x) + fabsf(y) + fabsf(z) + 1e-8f);
    float ox = x * inv;
    float oy = y * inv;
    if (z < 0.0f) {
        float t = (1.0f - fabsf(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        float u = (1.0f - fabsf(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
        ox = t;
        oy = u;
    }
    int qx = (int)lroundf(ox * 32767.0f);
    int qy = (int)lroundf(oy * 32767.0f);
    if (qx < -32768) qx = -32768;
    if (qx > 32767) qx = 32767;
    if (qy < -32768) qy = -32768;
    if (qy > 32767) qy = 32767;
    return ((uint32_t)(qx & 0xFFFF)) | ((uint32_t)(qy & 0xFFFF) << 16);
}

static void mat4_identity(float m[4][4]) {
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

static void mat4_mul(const float a[4][4], const float b[4][4], float out[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, t, sizeof(t));
}

static void node_world(const cgltf_node *node, float out[4][4]) {
    float local[4][4];
    if (node->has_matrix) {
        memcpy(local, node->matrix, sizeof(local));
    } else {
        float qx = node->rotation[0], qy = node->rotation[1], qz = node->rotation[2], qw = node->rotation[3];
        float xx = qx * qx, yy = qy * qy, zz = qz * qz;
        float xy = qx * qy, xz = qx * qz, yz = qy * qz;
        float wx = qw * qx, wy = qw * qy, wz = qw * qz;
        float sx = node->scale[0], sy = node->scale[1], sz = node->scale[2];
        mat4_identity(local);
        local[0][0] = (1.0f - 2.0f * (yy + zz)) * sx;
        local[0][1] = (2.0f * (xy - wz)) * sy;
        local[0][2] = (2.0f * (xz + wy)) * sz;
        local[1][0] = (2.0f * (xy + wz)) * sx;
        local[1][1] = (1.0f - 2.0f * (xx + zz)) * sy;
        local[1][2] = (2.0f * (yz - wx)) * sz;
        local[2][0] = (2.0f * (xz - wy)) * sx;
        local[2][1] = (2.0f * (yz + wx)) * sy;
        local[2][2] = (1.0f - 2.0f * (xx + yy)) * sz;
        local[0][3] = node->translation[0];
        local[1][3] = node->translation[1];
        local[2][3] = node->translation[2];
    }
    if (node->parent) {
        float parent[4][4];
        node_world(node->parent, parent);
        mat4_mul(parent, local, out);
    } else {
        memcpy(out, local, sizeof(local));
    }
}

static void xform_point(const float m[4][4], const float p[3], float out[3]) {
    out[0] = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
    out[1] = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
    out[2] = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3];
}

static void xform_normal(const float m[4][4], const float n[3], float out[3]) {
    float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2];
    float a10 = m[1][0], a11 = m[1][1], a12 = m[1][2];
    float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2];
    float c00 = a11 * a22 - a12 * a21;
    float c01 = a12 * a20 - a10 * a22;
    float c02 = a10 * a21 - a11 * a20;
    float c10 = a02 * a21 - a01 * a22;
    float c11 = a00 * a22 - a02 * a20;
    float c12 = a01 * a20 - a00 * a21;
    float c20 = a01 * a12 - a02 * a11;
    float c21 = a02 * a10 - a00 * a12;
    float c22 = a00 * a11 - a01 * a10;
    out[0] = c00 * n[0] + c10 * n[1] + c20 * n[2];
    out[1] = c01 * n[0] + c11 * n[1] + c21 * n[2];
    out[2] = c02 * n[0] + c12 * n[1] + c22 * n[2];
    float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len > 1e-8f) {
        out[0] /= len;
        out[1] /= len;
        out[2] /= len;
    }
}

static const cgltf_accessor *prim_attr(const cgltf_primitive *prim, const char *name) {
    for (cgltf_size i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == cgltf_attribute_type_position && name[0] == 'P')
            return prim->attributes[i].data;
        if (prim->attributes[i].type == cgltf_attribute_type_normal && name[0] == 'N')
            return prim->attributes[i].data;
        if (prim->attributes[i].type == cgltf_attribute_type_texcoord && name[0] == 'T')
            return prim->attributes[i].data;
        if (prim->attributes[i].type == cgltf_attribute_type_joints && name[0] == 'J')
            return prim->attributes[i].data;
        if (prim->attributes[i].type == cgltf_attribute_type_weights && name[0] == 'W')
            return prim->attributes[i].data;
    }
    return NULL;
}

static bool skin_bake_bind(cgltf_skin *skin, const cgltf_accessor *joints, const cgltf_accessor *weights,
                           float *positions, float *normals, uint32_t vcount) {
    uint32_t jcount = (uint32_t)skin->joints_count;
    if (jcount == 0 || !skin->inverse_bind_matrices || jcount > 256)
        return false;
    float *mats = (float *)malloc((size_t)jcount * 16 * sizeof(float));
    if (!mats)
        return false;
    for (uint32_t j = 0; j < jcount; j++) {
        float jw[4][4], sm[4][4], ibm[16];
        node_world(skin->joints[j], jw);
        cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibm, 16);
        float ibm_t[4][4];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                ibm_t[r][c] = ibm[c * 4 + r];
        mat4_mul(jw, ibm_t, sm);
        memcpy(&mats[j * 16], sm, sizeof(sm));
    }
    for (uint32_t i = 0; i < vcount; i++) {
        uint32_t ji[4] = {0, 0, 0, 0};
        float w[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        cgltf_accessor_read_uint(joints, i, ji, 4);
        cgltf_accessor_read_float(weights, i, w, 4);
        float wsum = w[0] + w[1] + w[2] + w[3];
        if (wsum < 1e-6f) {
            w[0] = 1.0f;
            wsum = 1.0f;
        }
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
        float lx = positions[i * 3 + 0], ly = positions[i * 3 + 1], lz = positions[i * 3 + 2];
        float lnx = normals[i * 3 + 0], lny = normals[i * 3 + 1], lnz = normals[i * 3 + 2];
        for (int k = 0; k < 4; k++) {
            if (w[k] == 0.0f || ji[k] >= jcount)
                continue;
            float wk = w[k] / wsum;
            const float *m = &mats[ji[k] * 16];
            px += wk * (m[0] * lx + m[1] * ly + m[2] * lz + m[3]);
            py += wk * (m[4] * lx + m[5] * ly + m[6] * lz + m[7]);
            pz += wk * (m[8] * lx + m[9] * ly + m[10] * lz + m[11]);
            nx += wk * (m[0] * lnx + m[1] * lny + m[2] * lnz);
            ny += wk * (m[4] * lnx + m[5] * lny + m[6] * lnz);
            nz += wk * (m[8] * lnx + m[9] * lny + m[10] * lnz);
        }
        positions[i * 3 + 0] = px;
        positions[i * 3 + 1] = py;
        positions[i * 3 + 2] = pz;
        normals[i * 3 + 0] = nx;
        normals[i * 3 + 1] = ny;
        normals[i * 3 + 2] = nz;
    }
    free(mats);
    return true;
}

static void path_dir(const char *path, char *out, size_t cap) {
    size_t n = strlen(path);
    size_t cut = 0;
    for (size_t i = 0; i < n; i++) {
        if (path[i] == '/' || path[i] == '\\')
            cut = i + 1;
    }
    if (cut >= cap)
        cut = cap - 1;
    memcpy(out, path, cut);
    out[cut] = '\0';
}

static TextureID load_albedo(VkBackend *vk, cgltf_data *data, const char *path, cgltf_texture *tex) {
    if (!tex || !tex->image)
        return UINT32_MAX;
    cgltf_image *img = tex->image;

    unsigned char *pixels = NULL;
    int w = 0, h = 0;
    if (img->buffer_view) {
        cgltf_buffer_view *bv = img->buffer_view;
        const unsigned char *base = (const unsigned char *)bv->buffer->data + bv->offset;
        pixels = stbi_load_from_memory(base, (int)bv->size, &w, &h, NULL, 4);
    } else if (img->uri) {
        char dir[1024];
        char full[2048];
        path_dir(path, dir, sizeof(dir));
        snprintf(full, sizeof(full), "%s%s", dir, img->uri);
        pixels = stbi_load(full, &w, &h, NULL, 4);
    }
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels)
            stbi_image_free(pixels);
        return UINT32_MAX;
    }

    TextureCreateDesc desc = {.width = (uint32_t)w,
                              .height = (uint32_t)h,
                              .depth = 1,
                              .layers = 1,
                              .mip_count = 1,
                              .format = VK_FORMAT_R8G8B8A8_SRGB,
                              .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .flags = 0,
                              .debug_name = "scene_albedo"};
    TextureID id = create_texture(vk, &desc);
    if (id == UINT32_MAX) {
        stbi_image_free(pixels);
        return UINT32_MAX;
    }
    VkOffset3D offset = {0, 0, 0};
    VkExtent3D extent = {(uint32_t)w, (uint32_t)h, 1};
    ByteSpan span = {.data = pixels, .size = (uint32_t)(w * h * 4)};
    if (!texture_upload(vk, id, 0, 0, offset, extent, span)) {
        stbi_image_free(pixels);
        destroy_texture(vk, id);
        return UINT32_MAX;
    }
    stbi_image_free(pixels);
    (void)data;
    return id;
}

bool scene_mesh_slot_add(Scene *s, const MeshSlotDesc *desc, uint32_t *out_slot) {
    (void)s;
    if (g_assets.mesh_count >= SCENE_MAX_MESH_SLOTS)
        return false;
    if (!desc)
        return false;

    uint32_t slot = g_assets.mesh_count++;
    SceneGpuMesh *m = &g_assets.meshes[slot];
    m->vertex_stream = desc->vertex_stream;
    m->index_stream = g_assets.index_stream_addr;
    m->index_count = desc->index_count;
    m->pack = ((uint32_t)desc->base_vertex & 0xFFFFF) | ((desc->material & 0xFFF) << 20);
    m->local_center = pack_half2(desc->center[0], desc->center[1]);
    m->local_radius = pack_half2(desc->center[2], desc->radius);
    m->lod_group = slot;
    m->flags = 0;
    m->first_cluster = 0;
    m->cluster_count = 0;

    LodGroup *lg = &g_assets.lod_groups[slot];
    lg->count = desc->lod_count < SCENE_MAX_LOD_LEVELS ? desc->lod_count : SCENE_MAX_LOD_LEVELS;
    for (uint32_t i = 0; i < lg->count; i++) {
        lg->rows[i].index_count = desc->lods[i].index_count;
        lg->rows[i].first_index = desc->lods[i].first_index;
        lg->rows[i].vertex_offset = desc->lods[i].vertex_offset;
        lg->rows[i].error = desc->lods[i].error;
    }
    for (uint32_t i = lg->count; i < SCENE_MAX_LOD_LEVELS; i++) {
        lg->rows[i] = lg->rows[0];
        lg->rows[i].error = 1e30f;
    }

    *out_slot = slot;
    return true;
}

SceneGpuMesh *scene_mesh_table(void) { return g_assets.meshes; }
uint32_t scene_mesh_count(void) { return g_assets.mesh_count; }
uint32_t scene_mesh_set_count(void) { return g_assets.mesh_set_count; }
uint32_t scene_slot_skin_set(uint32_t set_id) {
    if (set_id >= g_assets.mesh_set_count)
        return UINT32_MAX;
    uint32_t first = g_assets.mesh_sets[set_id].first;
    if (first >= SCENE_MAX_MESH_SLOTS)
        return UINT32_MAX;
    return g_assets.slot_skin[first];
}

uint32_t scene_skin_stream_slices(BufferSlice *out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_assets.skin_span_count && n < cap; i++) {
        if (g_assets.skin_spans[i].buffer)
            out[n++] = g_assets.skin_spans[i];
    }
    return n;
}

uint32_t scene_vertex_stream_slices(BufferSlice *out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_assets.vertex_slice_count && n < cap; i++) {
        if (g_assets.vertex_slices[i].buffer)
            out[n++] = g_assets.vertex_slices[i];
    }
    return n;
}

SceneGpuMaterial *scene_material_table(void) { return g_assets.materials; }
SceneMeshSet *scene_mesh_set_table(void) { return g_assets.mesh_sets; }

static bool upload_range(VkBackend *vk, VkCommandBuffer cmd, BufferSlice base, uint32_t first, uint32_t count,
                         const void *src, uint32_t stride) {
    if (count == 0)
        return true;
    BufferSlice slice = base;
    slice.offset += (VkDeviceSize)first * stride;
    slice.size = (VkDeviceSize)count * stride;
    ByteSpan span = {.data = (const uint8_t *)src + (size_t)first * stride, .size = (uint32_t)count * stride};
    return renderer_upload_buffer_to_slice(vk, cmd, slice, span);
}

uint32_t scene_load_model(Scene *s, VkCommandBuffer cmd, const char *path) {
    VkBackend *vk = s->vk;
    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success || !data) {
        log_error("[scene] parse failed: %s", path);
        return UINT32_MAX;
    }
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        log_error("[scene] buffers failed: %s", path);
        cgltf_free(data);
        return UINT32_MAX;
    }

    uint32_t mesh_first = g_assets.mesh_count;
    uint32_t mat_first = g_assets.material_count;
    uint32_t set_first = g_assets.mesh_set_count;
    uint32_t index_first = g_assets.index_used;

    uint32_t slots[MAX_MESHES_PER_SET];
    uint32_t slot_count = 0;
    uint32_t model_skin = UINT32_MAX;

    for (cgltf_size mi = 0; mi < data->meshes_count; mi++) {
        cgltf_mesh *mesh = &data->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh->primitives_count; pi++) {
            cgltf_primitive *prim = &mesh->primitives[pi];
            if (prim->type != cgltf_primitive_type_triangles || !prim->indices)
                continue;

            const cgltf_accessor *pos = prim_attr(prim, "POSITION");
            const cgltf_accessor *nrm = prim_attr(prim, "NORMAL");
            const cgltf_accessor *uv = prim_attr(prim, "TEXCOORD");
            const cgltf_accessor *joints_acc = prim_attr(prim, "JOINTS");
            const cgltf_accessor *weights_acc = prim_attr(prim, "WEIGHTS");
            if (!pos || pos->count == 0 || pos->count > 65535)
                continue;

            uint32_t vcount = (uint32_t)pos->count;
            uint32_t icount = (uint32_t)prim->indices->count;
            float *positions = (float *)malloc((size_t)vcount * 3 * sizeof(float));
            float *normals = (float *)malloc((size_t)vcount * 3 * sizeof(float));
            float *uvs = (float *)malloc((size_t)vcount * 2 * sizeof(float));
            uint32_t *indices = (uint32_t *)malloc((size_t)icount * sizeof(uint32_t));
            if (!positions || !normals || !uvs || !indices) {
                free(positions);
                free(normals);
                free(uvs);
                free(indices);
                continue;
            }

            float world[4][4];
            mat4_identity(world);
            cgltf_skin *mesh_skin = NULL;
            if (data->nodes_count > 0) {
                for (cgltf_size ni = 0; ni < data->nodes_count; ni++) {
                    cgltf_node *node = &data->nodes[ni];
                    if (node->mesh == mesh) {
                        node_world(node, world);
                        mesh_skin = node->skin;
                        break;
                    }
                }
            }

            float p[3], n[3], t[2], wp[3], wn[3];
            float center[3] = {0.0f, 0.0f, 0.0f};
            for (uint32_t i = 0; i < vcount; i++) {
                cgltf_accessor_read_float(pos, i, p, 3);
                positions[i * 3 + 0] = p[0];
                positions[i * 3 + 1] = p[1];
                positions[i * 3 + 2] = p[2];
                if (nrm) {
                    cgltf_accessor_read_float(nrm, i, n, 3);
                } else {
                    n[0] = 0.0f;
                    n[1] = 1.0f;
                    n[2] = 0.0f;
                }
                normals[i * 3 + 0] = n[0];
                normals[i * 3 + 1] = n[1];
                normals[i * 3 + 2] = n[2];
                if (uv) {
                    cgltf_accessor_read_float(uv, i, t, 2);
                    uvs[i * 2 + 0] = t[0];
                    uvs[i * 2 + 1] = t[1];
                } else {
                    uvs[i * 2 + 0] = 0.0f;
                    uvs[i * 2 + 1] = 0.0f;
                }
            }
            /* Skinned parts stay in bind-local space: the runtime palette
               (mesh_world * joint_world * ibm) does the whole transform, so the
               old static bake must not run here — applying both makes the VS
               deform twice and geometry flies off-screen. */
            bool part_skinned = mesh_skin && joints_acc && weights_acc;
            (void)skin_bake_bind;
            if (part_skinned && model_skin == UINT32_MAX)
                model_skin = scene_skin_register_from_gltf(vk, cmd, data, mesh_skin, world);
            for (uint32_t i = 0; i < vcount; i++) {
                p[0] = positions[i * 3 + 0];
                p[1] = positions[i * 3 + 1];
                p[2] = positions[i * 3 + 2];
                n[0] = normals[i * 3 + 0];
                n[1] = normals[i * 3 + 1];
                n[2] = normals[i * 3 + 2];
                if (!part_skinned) {
                    xform_point(world, p, wp);
                    xform_normal(world, n, wn);
                } else {
                    /* Unbaked: the VS palette applies mesh_world itself. */
                    wp[0] = p[0];
                    wp[1] = p[1];
                    wp[2] = p[2];
                    wn[0] = n[0];
                    wn[1] = n[1];
                    wn[2] = n[2];
                }
                positions[i * 3 + 0] = wp[0];
                positions[i * 3 + 1] = wp[1];
                positions[i * 3 + 2] = wp[2];
                normals[i * 3 + 0] = wn[0];
                normals[i * 3 + 1] = wn[1];
                normals[i * 3 + 2] = wn[2];
                center[0] += wp[0];
                center[1] += wp[1];
                center[2] += wp[2];
            }
            center[0] /= (float)vcount;
            center[1] /= (float)vcount;
            center[2] /= (float)vcount;
            float radius = 0.0f;
            for (uint32_t i = 0; i < vcount; i++) {
                float dx = positions[i * 3 + 0] - center[0];
                float dy = positions[i * 3 + 1] - center[1];
                float dz = positions[i * 3 + 2] - center[2];
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d > radius)
                    radius = d;
            }
            if (part_skinned && model_skin != UINT32_MAX) {
                float env_root[3], env_reach = 0.0f;
                scene_skin_envelope(model_skin, env_root, &env_reach);
                float eroot[3];
                xform_point(world, env_root, eroot);
                center[0] = eroot[0];
                center[1] = eroot[1];
                center[2] = eroot[2];
                radius = env_reach + radius;
            }

            for (uint32_t i = 0; i < icount; i++) {
                uint32_t idx = 0;
                cgltf_accessor_read_uint(prim->indices, i, &idx, 1);
                indices[i] = idx;
            }

            uint32_t mat_id = 0;
            if (prim->material) {
                cgltf_material *mat = prim->material;
                uint32_t base = 0xFFFFFFFFu;
                if (mat->has_pbr_metallic_roughness) {
                    int r = (int)lroundf(mat->pbr_metallic_roughness.base_color_factor[0] * 255.0f);
                    int g = (int)lroundf(mat->pbr_metallic_roughness.base_color_factor[1] * 255.0f);
                    int b = (int)lroundf(mat->pbr_metallic_roughness.base_color_factor[2] * 255.0f);
                    int a = (int)lroundf(mat->pbr_metallic_roughness.base_color_factor[3] * 255.0f);
                    if (r < 0) r = 0;
                    if (r > 255) r = 255;
                    if (g < 0) g = 0;
                    if (g > 255) g = 255;
                    if (b < 0) b = 0;
                    if (b > 255) b = 255;
                    if (a < 0) a = 0;
                    if (a > 255) a = 255;
                    base = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
                }
                uint32_t albedo = 0xFFFFu;
                if (mat->has_pbr_metallic_roughness && mat->pbr_metallic_roughness.base_color_texture.texture) {
                    TextureID tex =
                        load_albedo(vk, data, path, mat->pbr_metallic_roughness.base_color_texture.texture);
                    if (tex != UINT32_MAX)
                        albedo = tex & 0xFFFFu;
                }
                MaterialDesc mdesc = {.base_color = base, .albedo_texture = albedo, .flags = 0};
                mat_id = scene_material_add(s, &mdesc);
            }

            ScenePackedVertex *packed =
                (ScenePackedVertex *)malloc((size_t)vcount * sizeof(ScenePackedVertex));
            if (!packed) {
                free(positions);
                free(normals);
                free(uvs);
                free(indices);
                continue;
            }
            for (uint32_t i = 0; i < vcount; i++) {
                packed[i].position_xy = pack_half2(positions[i * 3 + 0], positions[i * 3 + 1]);
                packed[i].position_z = pack_half2(positions[i * 3 + 2], 0.0f);
                packed[i].normal_oct =
                    pack_oct16(normals[i * 3 + 0], normals[i * 3 + 1], normals[i * 3 + 2]);
                packed[i].uv = pack_half2(uvs[i * 2 + 0], uvs[i * 2 + 1]);
            }

            VkDeviceSize vbytes = (VkDeviceSize)vcount * sizeof(ScenePackedVertex);
            BufferSlice vslice = buffer_pool_alloc(&vk->gpu_pool, vbytes, 16);
            if (!vslice.buffer) {
                free(positions);
                free(normals);
                free(uvs);
                free(indices);
                free(packed);
                continue;
            }
            ByteSpan vspan = {.data = packed, .size = (uint32_t)vcount * (uint32_t)sizeof(ScenePackedVertex)};
            renderer_upload_buffer_to_slice(vk, cmd, vslice, vspan);
            cmd_buffer_barrier(cmd, vslice.buffer, vslice.offset, vbytes,
                               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            free(packed);

            if (g_assets.vertex_slice_count >= SCENE_MAX_MESH_SLOTS) {
                buffer_pool_free(vslice);
                free(positions);
                free(normals);
                free(uvs);
                free(indices);
                continue;
            }
            g_assets.vertex_slices[g_assets.vertex_slice_count++] = vslice;

            BufferSlice skslice = {0};
            if (part_skinned && model_skin != UINT32_MAX &&
                g_assets.skin_span_count < SCENE_MAX_MESH_SLOTS) {
                VkDeviceSize skbytes = (VkDeviceSize)vcount * 16;
                skslice = buffer_pool_alloc(&vk->gpu_pool, skbytes, 16);
                if (skslice.buffer) {
                    uint8_t *skdata = (uint8_t *)malloc((size_t)skbytes);
                    if (skdata) {
                        for (uint32_t i = 0; i < vcount; i++) {
                            uint32_t ji[4] = {0, 0, 0, 0};
                            float w[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                            cgltf_accessor_read_uint(joints_acc, i, ji, 4);
                            cgltf_accessor_read_float(weights_acc, i, w, 4);
                            uint16_t *d = (uint16_t *)(skdata + (size_t)i * 16);
                            uint32_t jc = scene_skin_joint_count(model_skin);
                            for (int k = 0; k < 4; k++) {
                                d[k] = (uint16_t)(ji[k] < jc ? ji[k] : 0);
                                float q = w[k] < 0.0f ? 0.0f : (w[k] > 1.0f ? 1.0f : w[k]);
                                d[4 + k] = (uint16_t)(q * 65535.0f + 0.5f);
                            }
                        }
                        ByteSpan skspan = {.data = skdata, .size = (uint32_t)skbytes};
                        renderer_upload_buffer_to_slice(vk, cmd, skslice, skspan);
                        cmd_buffer_barrier(cmd, skslice.buffer, skslice.offset, skbytes,
                                           VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
                        free(skdata);
                        g_assets.skin_spans[g_assets.skin_span_count++] = skslice;
                    } else {
                        buffer_pool_free(skslice);
                        memset(&skslice, 0, sizeof(skslice));
                    }
                } else if (skslice.buffer) {
                    buffer_pool_free(skslice);
                    memset(&skslice, 0, sizeof(skslice));
                }
                if (part_skinned && model_skin != UINT32_MAX && !skslice.buffer) {
                    buffer_pool_free(vslice);
                    g_assets.vertex_slice_count--;
                    free(positions);
                    free(normals);
                    free(uvs);
                    free(indices);
                    continue;
                }
            }

            static const float lod_ratios[SCENE_LOD_LEVELS] = {1.0f, 0.5f, 0.25f, 0.125f};
            SceneLodRow lods[SCENE_LOD_LEVELS];
            uint32_t lod_count = 0;
            uint32_t prev_count = 0;
            uint32_t *scratch = (uint32_t *)malloc((size_t)icount * sizeof(uint32_t));
            for (uint32_t li = 0; li < SCENE_LOD_LEVELS; li++) {
                uint32_t want = (uint32_t)((float)icount * lod_ratios[li]);
                if (want < 3)
                    break;
                if (li > 0 && (icount < 768 || want >= prev_count))
                    break;
                uint32_t got = icount;
                float result_error = 0.0f;
                if (li > 0) {
                    got = (uint32_t)meshopt_simplify(scratch, indices, icount, positions, vcount, 12, want,
                                                     1e-3f, 0, &result_error);
                    if (got >= prev_count || got < 3)
                        break;
                } else {
                    memcpy(scratch, indices, (size_t)icount * sizeof(uint32_t));
                }
                if (g_assets.index_used + got > SCENE_INDEX_POOL_MAX)
                    break;
                uint32_t first = g_assets.index_used;
                for (uint32_t i = 0; i < got; i++)
                    g_assets.index_mirror[first + i] = (uint16_t)scratch[i];
                g_assets.index_used += got;
                lods[lod_count].index_count = got;
                lods[lod_count].first_index = first;
                lods[lod_count].vertex_offset = 0;
                lods[lod_count].error = li == 0 ? 0.0f : result_error;
                lod_count++;
                prev_count = got;
            }
            free(scratch);
            if (lod_count == 0) {
                free(positions);
                free(normals);
                free(uvs);
                free(indices);
                continue;
            }

            MeshSlotDesc sdesc = {.vertex_stream = vk->gpu_base_addr + vslice.offset,
                                  .index_stream = g_assets.index_stream_addr,
                                  .index_count = lods[0].index_count,
                                  .base_vertex = 0,
                                  .material = mat_id,
                                  .center = {center[0], center[1], center[2]},
                                  .radius = radius,
                                  .lod_count = lod_count,
                                  .lods = lods};
            uint32_t slot = UINT32_MAX;
            if (scene_mesh_slot_add(s, &sdesc, &slot) && slot_count < MAX_MESHES_PER_SET) {
                slots[slot_count++] = slot;
                if (part_skinned && model_skin != UINT32_MAX && skslice.buffer) {
                    g_assets.slot_skin[slot] = model_skin;
                    g_assets.skin_mirror[slot].skin_stream = vk->gpu_base_addr + skslice.offset;
                    g_assets.skin_mirror[slot].palette = scene_skin_palette_base(model_skin, vk);
                    g_assets.skin_mirror[slot].joint_count = scene_skin_joint_count(model_skin);
                    g_assets.skin_mirror[slot].flags = 0;
                }
            }

            free(positions);
            free(normals);
            free(uvs);
            free(indices);
        }
    }

    uint32_t set_id = UINT32_MAX;
    if (slot_count > 0) {
        MeshSetDesc setdesc = {.mesh_slots = slots, .mesh_count = slot_count};
        set_id = scene_mesh_set_add(s, &setdesc);
    }

    upload_range(vk, cmd, s->mesh_slice, mesh_first, g_assets.mesh_count - mesh_first, g_assets.meshes,
                 (uint32_t)sizeof(SceneGpuMesh));
    upload_range(vk, cmd, s->material_slice, mat_first, g_assets.material_count - mat_first,
                 g_assets.materials, (uint32_t)sizeof(SceneGpuMaterial));
    upload_range(vk, cmd, s->mesh_set_slice, set_first, g_assets.mesh_set_count - set_first,
                 g_assets.mesh_sets, (uint32_t)sizeof(SceneMeshSet));
    for (uint32_t i = mesh_first; i < g_assets.mesh_count; i++) {
        LodGroup *lg = &g_assets.lod_groups[i];
        BufferSlice slice = s->lod_slice;
        slice.offset += (VkDeviceSize)i * SCENE_MAX_LOD_LEVELS * sizeof(SceneLodRow);
        slice.size = (VkDeviceSize)SCENE_MAX_LOD_LEVELS * sizeof(SceneLodRow);
        ByteSpan span = {.data = lg->rows,
                         .size = SCENE_MAX_LOD_LEVELS * (uint32_t)sizeof(SceneLodRow)};
        renderer_upload_buffer_to_slice(vk, cmd, slice, span);
    }
    if (g_assets.mesh_count > mesh_first) {
        BufferSlice slice = s->skin_slice;
        slice.offset += (VkDeviceSize)mesh_first * sizeof(SceneSkinRecord);
        slice.size = (VkDeviceSize)(g_assets.mesh_count - mesh_first) * sizeof(SceneSkinRecord);
        ByteSpan span = {.data = (const uint8_t *)&g_assets.skin_mirror[mesh_first],
                         .size = (g_assets.mesh_count - mesh_first) * (uint32_t)sizeof(SceneSkinRecord)};
        renderer_upload_buffer_to_slice(vk, cmd, slice, span);
    }
    if (g_assets.index_used > index_first) {
        BufferSlice slice = g_assets.index_slice;
        slice.offset += (VkDeviceSize)index_first * sizeof(uint16_t);
        slice.size = (VkDeviceSize)(g_assets.index_used - index_first) * sizeof(uint16_t);
        ByteSpan span = {.data = &g_assets.index_mirror[index_first],
                         .size = (g_assets.index_used - index_first) * (uint32_t)sizeof(uint16_t)};
        renderer_upload_buffer_to_slice(vk, cmd, slice, span);
    }

    cgltf_free(data);
    return set_id;
}
