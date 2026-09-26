/* glTF/GLB import. One file becomes one SceneAsset: packed vertex streams,
   a combined index buffer, the material table, node/skeleton/animation truth,
   and persistent device buffers for everything the GPU fetches by address.

   Static and skinned models take the same path; skinning only changes which
   vertex stream a mesh draws from and whether a palette exists. */

#include "scene3d.h"

#include "../../external/cgltf/cgltf.h"
#include "../../external/stb/stb_image.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CPU-side node truth the shaders never see: local + global matrices plus the
   parent link used to walk the hierarchy. */
struct SceneNodeData {
    float   local[4][4];
    float   global[4][4];
    int32_t parent;
    int32_t pad[3];
};

static uint64_t scene_hash(const char *s) {
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        h = (h ^ (uint64_t)*p) * 1099511628211ull;
    return h;
}

static float scene_half_to_float(uint16_t h) { return mu_dequantize_half(h); }

static uint16_t scene_pack_snorm8(float v) {
    int q = (int)lroundf(v * 127.0f);
    if (q < -127)
        q = -127;
    if (q > 127)
        q = 127;
    return (uint16_t)(q & 0xFF);
}

static uint16_t scene_pack_snorm10(float v) {
    int q = (int)lroundf(v * 511.0f);
    if (q < -511)
        q = -511;
    if (q > 511)
        q = 511;
    return (uint16_t)(q & 0x3FF);
}

/* Octahedral normal, snorm16 x 2. Matches the shader's reconstruction. */
static uint32_t scene_pack_oct(float x, float y, float z) {
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0.0f) {
        x /= len;
        y /= len;
        z /= len;
    }
    float inv = 1.0f / (fabsf(x) + fabsf(y) + fabsf(z) + 1e-8f);
    float ox  = x * inv;
    float oy  = y * inv;
    if (z < 0.0f) {
        float t = (1.0f - fabsf(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        float u = (1.0f - fabsf(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
        ox      = t;
        oy      = u;
    }
    return (uint32_t)scene_pack_snorm10(ox) | ((uint32_t)scene_pack_snorm10(oy) << 16);
}

static void scene_mat4_identity(float m[4][4]) {
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

static void scene_mat4_mul(const float a[4][4], const float b[4][4], float out[4][4]) {
    float r[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, r, sizeof(r));
}

/* TRS with a quaternion, matching the CPU animation path. */
static void scene_trs(float out[4][4], const float t[3], const float r[4], const float s[3]) {
    float x = r[0], y = r[1], z = r[2], w = r[3];
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;

    out[0][0] = (1.0f - 2.0f * (yy + zz)) * s[0];
    out[0][1] = (2.0f * (xy - wz)) * s[1];
    out[0][2] = (2.0f * (xz + wy)) * s[2];
    out[0][3] = t[0];

    out[1][0] = (2.0f * (xy + wz)) * s[0];
    out[1][1] = (1.0f - 2.0f * (xx + zz)) * s[1];
    out[1][2] = (2.0f * (yz - wx)) * s[2];
    out[1][3] = t[1];

    out[2][0] = (2.0f * (xz - wy)) * s[0];
    out[2][1] = (2.0f * (yz + wx)) * s[1];
    out[2][2] = (1.0f - 2.0f * (xx + yy)) * s[2];
    out[2][3] = t[2];

    out[3][0] = 0.0f;
    out[3][1] = 0.0f;
    out[3][2] = 0.0f;
    out[3][3] = 1.0f;
}

/* ---- glTF/GLB import -------------------------------------------------- */

/* Float -> IEEE half. Enough range for model coordinates; denormals flush. */
static uint16_t scene_float_to_half(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = bits & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10)
            return (uint16_t)sign;
        mant |= 0x800000u;
        return (uint16_t)(sign | (mant >> (uint32_t)(14 - exp)));
    }
    if (exp >= 31)
        return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

/* transpose(inverse(upper3x3)) written as cofactor/det: node transforms are
   baked into the vertices once at load, so normals need the inverse-transpose
   to survive non-uniform scale. */
static void scene_normal_matrix(const float m[16], float out[3][3]) {
    float a[3][3] = {{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}};
    float c00 =  a[1][1] * a[2][2] - a[1][2] * a[2][1];
    float c01 = -(a[1][0] * a[2][2] - a[1][2] * a[2][0]);
    float c02 =  a[1][0] * a[2][1] - a[1][1] * a[2][0];
    float det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    if (fabsf(det) < 1e-12f)
        det = 1.0f;

    out[0][0] = c00 / det;
    out[0][1] = c01 / det;
    out[0][2] = c02 / det;
    out[1][0] = -(a[0][1] * a[2][2] - a[0][2] * a[2][1]) / det;
    out[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    out[1][2] = -(a[0][0] * a[2][1] - a[0][1] * a[2][0]) / det;
    out[2][0] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    out[2][1] = -(a[0][0] * a[1][2] - a[0][2] * a[1][0]) / det;
    out[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
}

/* One file becomes one SceneAsset. Every triangle primitive is merged into a
   single mesh — one draw slot, one candidate per instance — because the whole
   model shares one material. Node rest transforms are baked into the vertices;
   skinned nodes render at rest pose (the spec ignores a skinned node's own
   transform) and their palette lands with the skinning dispatch. */
uint32_t scene3d_load_model(Scene3d *s, const char *path) {
    assert(s->model_count < SCENE3D_MAX_MODELS);
    assert(s->mesh_slot_count + 1 <= SCENE3D_MAX_BATCHES);

    cgltf_options options = {0};
    cgltf_data   *gltf    = NULL;
    if (cgltf_parse_file(&options, path, &gltf) != cgltf_result_success || !gltf ||
        cgltf_load_buffers(&options, gltf, path) != cgltf_result_success) {
        fprintf(stderr, "[scene3d] cannot load %s\n", path);
        if (gltf)
            cgltf_free(gltf);
        return UINT32_MAX;
    }

    /* Pass 1: enforce the merge contract (triangles, one material) and size
       the staging arrays. Node instancing is honored: the same primitive
       reached through two nodes counts twice. */
    uint32_t              vert_total = 0, index_total = 0;
    const cgltf_material *material   = NULL;
    bool                  skinned    = false;

    for (cgltf_size n = 0; n < gltf->nodes_count; n++) {
        const cgltf_node *node = &gltf->nodes[n];
        if (!node->mesh)
            continue;
        skinned |= node->skin != NULL;

        for (cgltf_size pi = 0; pi < node->mesh->primitives_count; pi++) {
            const cgltf_primitive *prim = &node->mesh->primitives[pi];
            const cgltf_accessor  *pos  = NULL;
            for (cgltf_size at = 0; at < prim->attributes_count; at++)
                if (prim->attributes[at].type == cgltf_attribute_type_position && prim->attributes[at].index == 0)
                    pos = prim->attributes[at].data;

            if (prim->type != cgltf_primitive_type_triangles || !pos) {
                fprintf(stderr, "[scene3d] %s: non-triangle primitive\n", path);
                cgltf_free(gltf);
                return UINT32_MAX;
            }
            if (prim->material && material && prim->material != material) {
                fprintf(stderr, "[scene3d] %s: multiple materials are not supported yet\n", path);
                cgltf_free(gltf);
                return UINT32_MAX;
            }
            if (prim->material)
                material = prim->material;

            vert_total += (uint32_t)pos->count;
            index_total += prim->indices ? (uint32_t)prim->indices->count : (uint32_t)pos->count;
        }
    }
    if (!vert_total || !index_total) {
        fprintf(stderr, "[scene3d] %s: no geometry\n", path);
        cgltf_free(gltf);
        return UINT32_MAX;
    }

    ScenePackedVertex *verts   = (ScenePackedVertex *)malloc((size_t)vert_total * sizeof(ScenePackedVertex));
    uint16_t          *indices = (uint16_t *)malloc((size_t)index_total * sizeof(uint16_t));
    float              lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float              hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    uint32_t           vb = 0, ib = 0;

    /* Pass 2: bake node rest transforms and pack. */
    for (cgltf_size n = 0; n < gltf->nodes_count; n++) {
        const cgltf_node *node = &gltf->nodes[n];
        if (!node->mesh)
            continue;

        float world[16], normal_m[3][3];
        if (node->skin) {
            memset(world, 0, sizeof(world));
            world[0] = world[5] = world[10] = world[15] = 1.0f;
            normal_m[0][0] = normal_m[1][1] = normal_m[2][2] = 1.0f;
            normal_m[0][1] = normal_m[0][2] = normal_m[1][0] = 0.0f;
            normal_m[1][2] = normal_m[2][0] = normal_m[2][1] = 0.0f;
        } else {
            cgltf_node_transform_world(node, world);
            scene_normal_matrix(world, normal_m);
        }

        for (cgltf_size pi = 0; pi < node->mesh->primitives_count; pi++) {
            const cgltf_primitive *prim = &node->mesh->primitives[pi];
            const cgltf_accessor  *pos = NULL, *nrm = NULL, *uvc = NULL;
            for (cgltf_size at = 0; at < prim->attributes_count; at++) {
                const cgltf_attribute *attr = &prim->attributes[at];
                if (attr->index != 0)
                    continue;
                if (attr->type == cgltf_attribute_type_position)
                    pos = attr->data;
                else if (attr->type == cgltf_attribute_type_normal)
                    nrm = attr->data;
                else if (attr->type == cgltf_attribute_type_texcoord)
                    uvc = attr->data;
            }

            for (cgltf_size i = 0; i < pos->count; i++) {
                float p[3], n[3] = {0.0f, 0.0f, 1.0f}, t[2] = {0.0f, 0.0f};
                cgltf_accessor_read_float(pos, i, p, 3);
                if (nrm)
                    cgltf_accessor_read_float(nrm, i, n, 3);
                if (uvc)
                    cgltf_accessor_read_float(uvc, i, t, 2);

                float wp[3] = {world[0] * p[0] + world[4] * p[1] + world[8] * p[2] + world[12],
                               world[1] * p[0] + world[5] * p[1] + world[9] * p[2] + world[13],
                               world[2] * p[0] + world[6] * p[1] + world[10] * p[2] + world[14]};
                float wn[3] = {normal_m[0][0] * n[0] + normal_m[0][1] * n[1] + normal_m[0][2] * n[2],
                               normal_m[1][0] * n[0] + normal_m[1][1] * n[1] + normal_m[1][2] * n[2],
                               normal_m[2][0] * n[0] + normal_m[2][1] * n[1] + normal_m[2][2] * n[2]};

                for (int k = 0; k < 3; k++) {
                    if (wp[k] < lo[k])
                        lo[k] = wp[k];
                    if (wp[k] > hi[k])
                        hi[k] = wp[k];
                }

                ScenePackedVertex *v = &verts[vb + i];
                v->position_xy = (uint32_t)scene_float_to_half(wp[0]) | ((uint32_t)scene_float_to_half(wp[1]) << 16);
                v->position_z  = (uint32_t)scene_float_to_half(wp[2]);
                v->normal_oct  = scene_pack_oct(wn[0], wn[1], wn[2]);
                v->uv          = (uint32_t)scene_float_to_half(t[0]) | ((uint32_t)scene_float_to_half(t[1]) << 16);
            }

            uint32_t prim_index_count =
                prim->indices ? (uint32_t)prim->indices->count : (uint32_t)pos->count;
            if (prim->indices) {
                for (cgltf_size i = 0; i < prim->indices->count; i++) {
                    cgltf_uint idx = 0;
                    cgltf_accessor_read_uint(prim->indices, i, &idx, 1);
                    assert(vb + idx <= 0xFFFFu);
                    indices[ib + i] = (uint16_t)(vb + idx);
                }
            } else {
                for (cgltf_size i = 0; i < pos->count; i++)
                    indices[ib + i] = (uint16_t)(vb + i);
            }

            vb += (uint32_t)pos->count;
            ib += prim_index_count;
        }
    }

    /* Albedo: GLB images are embedded in a buffer view; .gltf may point at a
       file next to the model. Decoded once, uploaded sRGB like the sprites. */
    uint32_t albedo = UINT32_MAX;
    if (material && material->has_pbr_metallic_roughness &&
        material->pbr_metallic_roughness.base_color_texture.texture) {
        const cgltf_image *image = material->pbr_metallic_roughness.base_color_texture.texture->image;
        int                tw = 0, th = 0, tc = 0;
        unsigned char     *pixels = NULL;
        if (image && image->buffer_view) {
            const cgltf_buffer_view *bv = image->buffer_view;
            pixels = stbi_load_from_memory((const unsigned char *)bv->buffer->data + bv->offset, (int)bv->size,
                                           &tw, &th, &tc, 4);
        } else if (image && image->uri) {
            char full[1024];
            snprintf(full, sizeof(full), "%s", path);
            char *slash = strrchr(full, '/');
            if (slash)
                slash[1] = '\0';
            else
                full[0] = '\0';
            if (strlen(full) + strlen(image->uri) < sizeof(full)) {
                strcat(full, image->uri);
                pixels = stbi_load(full, &tw, &th, &tc, 4);
            }
        }

        if (pixels) {
            TextureCreateDesc desc = {
                .width      = (uint32_t)tw,
                .height     = (uint32_t)th,
                .mip_count  = 1,
                .format     = VK_FORMAT_R8G8B8A8_SRGB,
                .usage      = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .debug_name = path,
            };
            albedo = create_texture(s->vk, &desc);
            if (albedo == UINT32_MAX ||
                !texture_upload(s->vk, albedo, 0, 0, (VkOffset3D){0, 0, 0},
                                (VkExtent3D){(uint32_t)tw, (uint32_t)th, 1},
                                (ByteSpan){pixels, (uint32_t)tw * (uint32_t)th * 4u})) {
                fprintf(stderr, "[scene3d] %s: albedo upload failed\n", path);
                if (albedo != UINT32_MAX) {
                    destroy_texture(s->vk, albedo);
                    albedo = UINT32_MAX;
                }
            }
            stbi_image_free(pixels);
        } else {
            fprintf(stderr, "[scene3d] %s: albedo decode failed\n", path);
        }
    }

    SceneAsset *a = &s->models[s->model_count];
    memset(a, 0, sizeof(*a));

    a->mesh_count = 1;
    a->meshes     = (SceneMesh *)calloc(1, sizeof(SceneMesh));
    a->materials  = (SceneMaterialGpu *)calloc(1, sizeof(SceneMaterialGpu));
    a->material_count = 1;

    VkDeviceSize vert_bytes = (VkDeviceSize)vert_total * sizeof(ScenePackedVertex);
    VkDeviceSize idx_bytes  = (VkDeviceSize)index_total * sizeof(uint16_t);
    VkDeviceSize mat_bytes  = sizeof(SceneMaterialGpu);

    /* Host-visible for now; the importer switches to device-local uploads
       once the asset streaming path lands. */
    create_buffer(s->vk, vert_bytes,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                  VMA_MEMORY_USAGE_CPU_ONLY, &a->gpu.static_vertices);
    create_buffer(s->vk, idx_bytes,
                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                  VMA_MEMORY_USAGE_CPU_ONLY, &a->gpu.indices);
    create_buffer(s->vk, mat_bytes,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                  VMA_MEMORY_USAGE_CPU_ONLY, &a->gpu.materials);
    assert(a->gpu.static_vertices.mapping && a->gpu.indices.mapping && a->gpu.materials.mapping);

    memcpy(a->gpu.static_vertices.mapping, verts, vert_bytes);
    memcpy(a->gpu.indices.mapping, indices, idx_bytes);

    SceneMaterialGpu *mg = &a->materials[0];
    *mg                  = (SceneMaterialGpu){.base_color = {1.0f, 1.0f, 1.0f, 1.0f},
                                              .albedo_texture = albedo,
                                              .sampler        = 0,
                                              .flags          = 0,
                                              .pad            = 0};
    if (material && material->has_pbr_metallic_roughness) {
        const float *f = material->pbr_metallic_roughness.base_color_factor;
        mg->base_color = (SceneVector){f[0], f[1], f[2], f[3]};
    }
    memcpy(a->gpu.materials.mapping, mg, mat_bytes);

    SceneMesh *mesh = &a->meshes[0];
    float      center[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    float      half[3]   = {(hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f, (hi[2] - lo[2]) * 0.5f};
    mesh->first_index    = 0;
    mesh->index_count    = index_total;
    mesh->base_vertex    = 0;
    mesh->material       = 0;
    mesh->center[0]      = center[0];
    mesh->center[1]      = center[1];
    mesh->center[2]      = center[2];
    mesh->radius         = sqrtf(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
    mesh->vertex_stream  = a->gpu.static_vertices.address;

    a->static_vertex_count = vert_total;
    a->index_count         = index_total;
    a->draw_offset         = s->mesh_slot_count;
    a->flags               = skinned ? SCENE_ASSET_SKINNED : 0;
    a->ref_count           = 1;
    s->mesh_slot_count += a->mesh_count;

    free(verts);
    free(indices);
    cgltf_free(gltf);
    fprintf(stderr, "[scene3d] %s: %u verts, %u indices, %u draw slot %u, albedo %u\n", path, vert_total,
            index_total, s->model_count, a->draw_offset, albedo);
    return s->model_count++;
}

void scene3d_assets_destroy(Scene3d *s) {
    forEach(mo, s->model_count) {
        SceneAsset *a = &s->models[mo];
        forEach(mi, a->material_count)
            if (a->materials[mi].albedo_texture != UINT32_MAX)
                destroy_texture(s->vk, a->materials[mi].albedo_texture);
        destroy_buffer(s->vk, &a->gpu.static_vertices);
        destroy_buffer(s->vk, &a->gpu.skin_input);
        destroy_buffer(s->vk, &a->gpu.skinned_vertices);
        destroy_buffer(s->vk, &a->gpu.indices);
        destroy_buffer(s->vk, &a->gpu.materials);
        destroy_buffer(s->vk, &a->gpu.palette);
        free(a->meshes);
        free(a->materials);
        free(a->nodes);
        free(a->joints);
        free(a->skins);
        free(a->clips);
        free(a->samplers);
        free(a->channels);
        free(a->palette_mats);
        memset(a, 0, sizeof(*a));
    }
    s->model_count     = 0;
    s->mesh_slot_count = 0;
}

uint32_t scene3d_instance_capacity(const Scene3d *s) {
    if (!s->mesh_slot_count)
        return SCENE3D_MAX_INSTANCES;
    uint32_t cap = SCENE3D_MAX_INSTANCES / s->mesh_slot_count;
    return cap ? cap : 1;
}
