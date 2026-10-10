/* scene_asset.c — loads a cooked .mua into the scene.
 *
 * The loader does no geometry work. The cooker already quantized to the exact
 * 16-byte vertex rows the vertex shader reads, welded duplicates, rewrote the
 * index buffer for fetch locality and built the LOD ladder, so every step here
 * is a pointer into the mapped file plus one scene_mesh_add per mesh. The
 * "per-vertex float deinterleave, quantize and bounds recomputation" that a
 * glTF path repeats on every launch happens once, offline, and is the entire
 * reason the format exists.
 *
 * Nothing is uploaded here. scene_mesh_add stages into CPU tables and
 * scene_upload_scene performs the single copy, which is why loading has to
 * finish before the first frame — there is no resize path on the GPU arenas. */

#include "scene.h"

#include "../../muasset/muasset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The format's rows and the renderer's rows are the same bytes. Asserting it
   here means a format change that forgets the shader's vertex layout fails at
   load with a message, not as a scene made of garbage. */
_Static_assert(sizeof(MuassetVertex) == sizeof(struct ScenePackedVertex), "vertex row mismatch");
_Static_assert(sizeof(MuassetMaterial) == sizeof(struct SceneGpuMaterial), "material row mismatch");

static void *read_whole_file(const char *path, uint32_t *size_out, char *err, uint32_t err_cap);

/* Uploads the companion .tex payload and hands back one bindless id per
   texture row, in row order. The blob's ids are indices into the texture
   section; they are rewritten in place to the TextureID the backend allocated,
   which is also the bindless descriptor slot, so the shader indexes directly. */
static uint32_t *load_textures(VkBackend *vk, const Muasset *blob, const char *mua_path,
                               char *err, uint32_t err_cap) {
    if (blob->texture_count == 0)
        return NULL;

    char     tex_path[1024];
    snprintf(tex_path, sizeof(tex_path), "%s.tex", mua_path);
    uint32_t payload_size = 0;
    uint8_t *payload       = (uint8_t *)read_whole_file(tex_path, &payload_size, err, err_cap);
    if (!payload)
        return NULL;

    uint32_t *ids = (uint32_t *)malloc(blob->texture_count * sizeof(uint32_t));
    if (!ids) {
        free(payload);
        snprintf(err, err_cap, "out of memory");
        return NULL;
    }

    for (uint32_t i = 0; i < blob->texture_count; ++i) {
        const MuassetTexture *t = &blob->textures[i];
        if ((uint64_t)t->offset + t->size > payload_size) {
            snprintf(err, err_cap, "%s: texture %u runs past the payload", tex_path, i);
            goto fail;
        }
        VkFormat fmt = t->format == MUASSET_TEX_RGBA8 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED;
        if (fmt == VK_FORMAT_UNDEFINED) {
            snprintf(err, err_cap, "%s: texture %u has format %u this build cannot upload", tex_path, i, t->format);
            goto fail;
        }

        TextureID tex = create_texture(vk, &(TextureCreateDesc){.width     = t->width,
                                                               .height    = t->height,
                                                               .depth     = 1,
                                                               .layers    = 1,
                                                               .mip_count = t->mip_count,
                                                               .format    = fmt,
                                                               .usage     = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                               .debug_name = "mua_tex"});
        if (tex == UINT32_MAX) {
            snprintf(err, err_cap, "%s: texture pool exhausted", tex_path);
            goto fail;
        }

        /* The payload is level after level with no padding, so each mip's
           offset is derivable from the ones before it. */
        uint32_t w = t->width, h = t->height, off = t->offset;
        for (uint32_t m = 0; m < t->mip_count; ++m) {
            if (!texture_upload(vk, tex, m, 0, (VkOffset3D){0, 0, 0}, (VkExtent3D){w, h, 1},
                                (ByteSpan){payload + off, w * h * 4u})) {
                snprintf(err, err_cap, "%s: texture %u mip %u upload failed", tex_path, i, m);
                goto fail;
            }
            off += w * h * 4u;
            w = w > 1u ? w / 2u : 1u;
            h = h > 1u ? h / 2u : 1u;
        }
        ids[i] = tex;
    }
    free(payload);
    return ids;

fail:
    free(payload);
    free(ids);
    return NULL;
}

static void *read_whole_file(const char *path, uint32_t *size_out, char *err, uint32_t err_cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_cap, "cannot open %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(err, err_cap, "cannot seek %s", path);
        return NULL;
    }
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, err_cap, "%s is empty or unreadable", path);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        fclose(f);
        snprintf(err, err_cap, "short read on %s", path);
        return NULL;
    }
    fclose(f);
    *size_out = (uint32_t)n;
    return data;
}

bool scene_asset_probe(const char *path, SceneAssetProbe *out, char *err, uint32_t err_cap) {
    memset(out, 0, sizeof(*out));

    uint32_t size = 0;
    uint8_t *file = (uint8_t *)read_whole_file(path, &size, err, err_cap);
    if (!file)
        return false;

    Muasset blob;
    if (!muasset_open(file, size, &blob, err, err_cap)) {
        free(file);
        return false;
    }

    out->mesh_count    = blob.mesh_count;
    out->material_count = blob.material_count;
    out->texture_count = blob.texture_count;
    out->lod_rows      = blob.lod_count;
    out->vertex_count = blob.vertex_count;
    out->index_count  = blob.index_count;
    for (uint32_t i = 0; i < blob.mesh_count; ++i)
        if (blob.meshes[i].lod_count > out->max_lod_count)
            out->max_lod_count = blob.meshes[i].lod_count;

    free(file);
    return true;
}

bool scene_asset_load(Scene *s, const char *path, SceneAssetLoad *out, char *err, uint32_t err_cap) {
    memset(out, 0, sizeof(*out));

    uint32_t size = 0;
    uint8_t *file = (uint8_t *)read_whole_file(path, &size, err, err_cap);
    if (!file)
        return false;

    Muasset blob;
    if (!muasset_open(file, size, &blob, err, err_cap)) {
        free(file);
        return false;
    }

    /* Bounds for the model as a whole, so a caller can frame or place it
       without walking the mesh table. Loose, because a sphere is loose: this is
       for placement, not for culling — culling uses each mesh's own sphere. */
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < blob.mesh_count; ++i) {
        for (int k = 0; k < 3; ++k) {
            float c = blob.meshes[i].local_sphere[k], r = blob.meshes[i].local_sphere[3];
            if (c - r < lo[k])
                lo[k] = c - r;
            if (c + r > hi[k])
                hi[k] = c + r;
        }
    }
    float radius = 0.0f;
    for (int k = 0; k < 3; ++k) {
        out->center[k] = 0.5f * (lo[k] + hi[k]);
        float half      = 0.5f * (hi[k] - lo[k]);
        radius += half * half;
    }
    out->radius = sqrtf(radius);

    /* Register textures first: material rows reference bindless ids, and the
       blob only knows texture-section indices. */
    uint32_t *tex_ids = load_textures(scene_vk(s), &blob, path, err, err_cap);
    if (blob.texture_count && !tex_ids) {
        free(file);
        return false;
    }

    /* scene_mesh_add takes per-mesh slices out of the two arenas, so the rungs
       are described as offsets into this mesh's own block rather than as
       pointers that would have to be rebased. */
    SceneLodDesc lods[SCENE_MAX_LODS];
    uint32_t     first_mesh = 0;

    /* Blob material index -> scene material index. A model routinely shares one
       material across all of its meshes, so registering the row per mesh would
       burn a material slot per mesh and then run out. Registering per distinct
       blob material also keeps two models that happen to share material 0 but
       not its texture from collapsing onto one row. */
    uint32_t *mat_map = (uint32_t *)malloc((size_t)(blob.material_count ? blob.material_count : 1) * sizeof(uint32_t));
    if (!mat_map) {
        free(tex_ids);
        free(file);
        snprintf(err, err_cap, "out of memory");
        return false;
    }
    for (uint32_t i = 0; i < blob.material_count; ++i)
        mat_map[i] = UINT32_MAX;

    for (uint32_t i = 0; i < blob.mesh_count; ++i) {
        const MuassetMesh *m = &blob.meshes[i];
        if (m->lod_count == 0 || m->lod_count > SCENE_MAX_LODS) {
            snprintf(err, err_cap, "%s: mesh %u has %u rungs, this build takes 1..%u", path, i, m->lod_count,
                     SCENE_MAX_LODS);
            free(file);
            return false;
        }
        if (m->lod_first + m->lod_count > blob.lod_count) {
            snprintf(err, err_cap, "%s: mesh %u ladder runs past the section", path, i);
            free(file);
            return false;
        }

        uint32_t mesh_vbase = blob.lods[m->lod_first].first_vertex;
        uint32_t mesh_ibase = blob.lods[m->lod_first].first_index;
        for (uint32_t k = 0; k < m->lod_count; ++k) {
            const MuassetLod *lod = &blob.lods[m->lod_first + k];
            lods[k] = (SceneLodDesc){.first_vertex = lod->first_vertex - mesh_vbase,
                                     .first_index  = lod->first_index - mesh_ibase,
                                     .index_count  = lod->index_count,
                                     .error        = lod->error};
        }

        /* How much of each arena this mesh occupies.

           Vertices: every rung owns a separate block, so the mesh's footprint is
           the union of all of them, not the largest index value. Sizing this by
           the largest index alone truncates the mesh to rung 0's block and every
           coarser rung then reads the next mesh's vertices — geometry that is
           present, plausible, and wrong.
           Indices: a rung's first_index is already relative to this mesh's
           index base, so the highest first_index + count is the footprint. */
        uint32_t mesh_vertices = 0, mesh_indices = 0;
        for (uint32_t k = 0; k < m->lod_count; ++k) {
            const MuassetLod *lod = &blob.lods[m->lod_first + k];
            uint32_t           span = 0;
            for (uint32_t t = 0; t < lod->index_count; ++t)
                if (blob.indices[lod->first_index + t] + 1u > span)
                    span = blob.indices[lod->first_index + t] + 1u;
            if (lods[k].first_vertex + span > mesh_vertices)
                mesh_vertices = lods[k].first_vertex + span;
            if (lods[k].first_index + lods[k].index_count > mesh_indices)
                mesh_indices = lods[k].first_index + lods[k].index_count;
        }
        if (mesh_vertices > blob.vertex_count || mesh_ibase + mesh_indices > blob.index_count) {
            snprintf(err, err_cap, "%s: mesh %u ranges run past the arenas", path, i);
            free(file);
            return false;
        }

        /* The blob's material is an index; this is the row, with texture ids
           resolved from texture-section indices to bindless slots. */
        struct SceneGpuMaterial row = {
                      .base_color    = blob.materials[m->material].base_color,
                                  .texture       = blob.materials[m->material].texture != 0xFFFFFFFFu &&
                                                         tex_ids
                                                     ? tex_ids[blob.materials[m->material].texture]
                                                     : 0xFFFFFFFFu,
                                  .flags         = blob.materials[m->material].flags,
                                  .metallic      = blob.materials[m->material].metallic,
                                  .roughness     = blob.materials[m->material].roughness,
                                  .normal_texture = blob.materials[m->material].normal_texture != 0xFFFFFFFFu &&
                                                          tex_ids
                                                      ? tex_ids[blob.materials[m->material].normal_texture]
                                                      : 0xFFFFFFFFu,
                                  .orm_texture   = blob.materials[m->material].orm_texture != 0xFFFFFFFFu && tex_ids
                                                       ? tex_ids[blob.materials[m->material].orm_texture]
                                                       : 0xFFFFFFFFu};
        if (mat_map[m->material] == UINT32_MAX) {
            uint32_t id = scene_material_add(s, &row);
            if (id == UINT32_MAX) {
                snprintf(err, err_cap, "%s: material %u does not fit the scene's material capacity", path,
                         m->material);
                free(mat_map);
                free(tex_ids);
                free(file);
                return false;
            }
            mat_map[m->material] = id;
            log_info("[dbg] material %u -> scene %u base=%08X tex=%u ntex=%u flags=%X metal=%g rough=%g",
                     m->material, id, row.base_color, row.texture, row.normal_texture, row.flags, row.metallic,
                     row.roughness);
        }
        uint32_t material = mat_map[m->material];

        uint32_t mesh = scene_mesh_add(
            s, &(SceneMeshDesc){.vertices     = (const struct ScenePackedVertex *)(blob.vertices + mesh_vbase),
                                .vertex_count = mesh_vertices,
                                .indices      = blob.indices + mesh_ibase,
                                .index_count  = mesh_indices,
                                .lods         = lods,
                                .lod_count    = m->lod_count,
                                .local_center = {m->local_sphere[0], m->local_sphere[1], m->local_sphere[2]},
                                .local_radius = m->local_sphere[3],
                                .material     = material});
        if (mesh == UINT32_MAX) {
            snprintf(err, err_cap, "%s: mesh %u did not fit the scene's mesh or LOD capacity", path, i);
            free(file);
            return false;
        }
        if (i == 0)
            first_mesh = mesh;
    }

    free(mat_map);
    free(tex_ids);
    out->first_mesh   = first_mesh;
    out->mesh_count   = blob.mesh_count;
    out->vertex_count = blob.vertex_count;
    out->index_count  = blob.index_count;
    out->lod_count    = scene_lod_count(s);

    free(file);
    return true;
}