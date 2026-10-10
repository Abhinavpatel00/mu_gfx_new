/* muasset_cook.c — glTF -> .mua, plus dump and validate. Host-only: it links
 * cgltf and meshoptimizer and is never part of the engine.
 *
 * The format exists so that nothing in this file has to happen at load, so this
 * is where every per-vertex decision lives. In order, per glTF primitive:
 *
 *   bake node transform -> weld -> LOD ladder -> cache -> overdraw -> fetch
 *
 * The last step is not optional. meshopt_optimizeVertexFetchRemap rewrites the
 * index buffer so it walks vertices in near-monotonic order, which is what turns
 * the vertex shader's `vertex_stream[vid]` into a sequential read instead of a
 * random one. Skipping it still draws the right picture, at a bandwidth cost the
 * runtime can never recover.
 *
 * Every rung gets its own vertex block and its own rung-local index range. That
 * costs a little file size versus sharing one block, and buys a property the
 * runtime depends on: a rung's draw needs no vertexOffset, because its indices
 * already start at zero and its draw carries the vertex base itself.
 *
 * meshopt_simplify reports error relative to the mesh extents, so each rung's
 * error is scaled by the bounding radius here and lands in world units, which is
 * the unit the cull kernel's screen-space metric is expressed in. */

#define CGLTF_IMPLEMENTATION
#include "../external/cgltf/cgltf.h"
#include "../external/meshoptimizer/src/meshoptimizer.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "../external/stb/stb_image.h"

#include "muasset.h"
#include "muasset_cook.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MESHES  65535u
#define ATTR_FLOATS 5u /* normal xyz + uv xy */

#define POS_STRIDE  (3 * sizeof(float))
#define ATTR_STRIDE (ATTR_FLOATS * sizeof(float))

/* Each rung keeps this fraction of the rung above it. */
#define kTargetError 0.05f /* guard only; see rung_simplify */
static const float kLodRatio[MUACOOK_MAX_LODS] = {1.0f, 0.5f, 0.25f, 0.125f};

#define FAIL(...)                                                                                                    \
    do {                                                                                                             \
        snprintf(err, err_cap, __VA_ARGS__);                                                                          \
        goto fail;                                                                                                   \
    } while (0)

/* ============================================================ growable bytes */

typedef struct Buf {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} Buf;

static bool buf_reserve(Buf *b, size_t extra) {
    if (b->size + extra <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->size + extra)
        cap *= 2;
    uint8_t *d = (uint8_t *)realloc(b->data, cap);
    if (!d)
        return false;
    b->data = d;
    b->cap  = cap;
    return true;
}

static bool buf_append(Buf *b, const void *p, size_t n) {
    if (!n)
        return true;
    if (!buf_reserve(b, n))
        return false;
    memcpy(b->data + b->size, p, n);
    b->size += n;
    return true;
}

static bool buf_pad16(Buf *b) {
    static const uint8_t zero[16] = {0};
    return buf_append(b, zero, b->size % MUASSET_ALIGN ? MUASSET_ALIGN - (b->size % MUASSET_ALIGN) : 0);
}

/* ============================================================ vertex packing

   These must produce the same 16 bytes scene_pack_vertex() does. They are
   duplicated rather than shared because scene.c is the engine and this is a
   host tool; the load path memcpy's whatever we write, so the only thing the
   two have to agree on is that both are valid half / snorm16 encodings. */

static uint16_t pack_half(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t  e    = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t m    = bits & 0x7FFFFFu;
    if (e <= 0)
        return (uint16_t)sign;
    if (e >= 31)
        return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)e << 10) | (m >> 13));
}

static uint32_t pack_half2(float a, float b) { return (uint32_t)pack_half(a) | ((uint32_t)pack_half(b) << 16); }

static uint32_t pack_oct(const float n[3]) {
    float l = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]);
    if (l < 1e-8f)
        return 0;
    float x = n[0] / l, y = n[1] / l;
    if (n[2] < 0.0f) {
        float t = (1.0f - fabsf(x)) * (x >= 0.0f ? 1.0f : -1.0f);
        float u = (1.0f - fabsf(y)) * (y >= 0.0f ? 1.0f : -1.0f);
        x      = t;
        y      = u;
    }
    return (uint32_t)((int32_t)lrintf(x * 32767.0f) & 0xFFFF) | ((uint32_t)((int32_t)lrintf(y * 32767.0f) & 0xFFFF) << 16);
}

/* ============================================================ rung pipeline */

typedef struct Rung {
    uint32_t *idx;       /* rung-local vertex ids                             */
    uint32_t  idx_count;
    float    *pos;       /* vert_count * 3                                    */
    float    *attr;      /* vert_count * ATTR_FLOATS                          */
    uint32_t  vert_count;
    float     error;     /* world units, filled in after the ladder is built  */
} Rung;

static void rung_free(Rung *r) {
    free(r->idx);
    free(r->pos);
    free(r->attr);
    memset(r, 0, sizeof(*r));
}

static bool rung_alloc(Rung *r, uint32_t vert_count, uint32_t idx_count) {
    memset(r, 0, sizeof(*r));
    r->idx        = (uint32_t *)malloc((size_t)idx_count * sizeof(uint32_t));
    r->pos        = (float *)malloc((size_t)vert_count * POS_STRIDE);
    r->attr       = (float *)malloc((size_t)vert_count * ATTR_STRIDE);
    r->idx_count  = idx_count;
    r->vert_count = vert_count;
    return r->idx && r->pos && r->attr;
}

/* Welds binary-identical vertices and compacts every parallel array onto the
   survivors. Runs after welding and again after simplification, because both
   leave rows nobody references. */
static bool rung_weld(Rung *r) {
    uint32_t *remap = (uint32_t *)malloc((size_t)r->vert_count * sizeof(uint32_t));
    if (!remap)
        return false;

    const struct meshopt_Stream streams[2] = {
        {.data = r->pos, .size = POS_STRIDE, .stride = POS_STRIDE},
        {.data = r->attr, .size = ATTR_STRIDE, .stride = ATTR_STRIDE},
    };
    uint32_t unique =
        (uint32_t)meshopt_generateVertexRemapMulti(remap, r->idx, r->idx_count, r->vert_count, streams, 2);

    Rung out;
    if (!rung_alloc(&out, unique, r->idx_count)) {
        free(remap);
        rung_free(&out);
        return false;
    }
    meshopt_remapIndexBuffer(out.idx, r->idx, r->idx_count, remap);
    meshopt_remapVertexBuffer(out.pos, r->pos, r->vert_count, POS_STRIDE, remap);
    meshopt_remapVertexBuffer(out.attr, r->attr, r->vert_count, ATTR_STRIDE, remap);
    free(remap);
    out.error = r->error; /* the weld only moves rows; it does not re-measure */

    rung_free(r);
    *r = out;
    return true;
}

/* Seams are deliberately NOT locked. On hard-surface content exported from a
   DCC tool almost every vertex sits on a UV or normal seam, so locking them
   locks the mesh outright and the ladder degenerates to copies of rung 0 — a
   rung the runtime can pick that costs file size and saves nothing. The
   attribute weights in the error metric already bias collapses away from
   seams, and a rung is only ever shown small enough that the residual UV drift
   is below a pixel. */
static bool rung_simplify(Rung *dst, const Rung *src, uint32_t target_index_count) {
    static const float weights[ATTR_FLOATS] = {0.5f, 0.5f, 0.5f, 1.0f, 1.0f};

    Rung out;
    if (!rung_alloc(&out, src->vert_count, src->idx_count))
        return false;
    memcpy(out.pos, src->pos, (size_t)src->vert_count * POS_STRIDE);
    memcpy(out.attr, src->attr, (size_t)src->vert_count * ATTR_STRIDE);

    /* The target index count is the design decision; the error budget is only a
       guard against collapsing a mesh into nonsense. Setting it tight enough to
       bind would mean the rungs stop wherever the metric happens to stop, which
       on already-coarse assets is rung 0 — a ladder of identical rungs that
       costs file size and buys nothing. Whatever error meshoptimizer reports is
       the value the runtime's screen-space metric gets compared against, so it
       has to be measured, not assumed. */
    out.idx_count = (uint32_t)meshopt_simplifyWithAttributes(out.idx, src->idx, src->idx_count, src->pos,
                                                             src->vert_count, POS_STRIDE, src->attr, ATTR_STRIDE,
                                                             weights, ATTR_FLOATS, NULL, target_index_count,
                                                             kTargetError, meshopt_SimplifyLockBorder, &out.error);
    if (out.idx_count < 3) {
        rung_free(&out);
        return false;
    }
    if (!rung_weld(&out)) {
        rung_free(&out);
        return false;
    }
    *dst = out;
    return true;
}

/* Cache-friendly triangle order, then overdraw-friendly order, then the fetch
   rewrite. The three compose: each one changes the input the next one sees. */
static bool rung_optimize(Rung *r) {
    uint32_t *a = (uint32_t *)malloc((size_t)r->idx_count * sizeof(uint32_t));
    if (!a)
        return false;
    meshopt_optimizeVertexCache(a, r->idx, r->idx_count, r->vert_count);
    meshopt_optimizeOverdraw(r->idx, a, r->idx_count, r->pos, r->vert_count, POS_STRIDE, 1.0f);
    free(a);

    uint32_t *fetch = (uint32_t *)malloc((size_t)r->vert_count * sizeof(uint32_t));
    if (!fetch)
        return false;
    meshopt_optimizeVertexFetchRemap(fetch, r->idx, r->idx_count, r->vert_count);

    Rung out;
    if (!rung_alloc(&out, r->vert_count, r->idx_count)) {
        free(fetch);
        rung_free(&out);
        return false;
    }
    meshopt_remapIndexBuffer(out.idx, r->idx, r->idx_count, fetch);
    meshopt_remapVertexBuffer(out.pos, r->pos, r->vert_count, POS_STRIDE, fetch);
    meshopt_remapVertexBuffer(out.attr, r->attr, r->vert_count, ATTR_STRIDE, fetch);
    free(fetch);
    out.error = r->error; /* reordering rows does not re-measure them */

    /* Truncate to the highest row something still addresses. The remap leaves
       unreferenced rows scattered through the middle, and shipping them would
       hand the vertex shader rows it never reads. */
    uint32_t used = 0;
    for (uint32_t i = 0; i < out.idx_count; ++i)
        used = out.idx[i] + 1u > used ? out.idx[i] + 1u : used;

    rung_free(r);
    *r = out;
    r->vert_count = used;
    return true;
}

static bool rung_emit(const Rung *r, Buf *vert_out, Buf *idx_out, MuassetLod *lod) {
    uint32_t vbase = (uint32_t)(vert_out->size / sizeof(MuassetVertex));
    uint32_t ibase = (uint32_t)(idx_out->size / sizeof(uint16_t));

    if (!buf_reserve(vert_out, (size_t)r->vert_count * sizeof(MuassetVertex)))
        return false;
    MuassetVertex *rows = (MuassetVertex *)(vert_out->data + vert_out->size);
    for (uint32_t i = 0; i < r->vert_count; ++i) {
        const float *p = r->pos + (size_t)i * 3;
        const float *a = r->attr + (size_t)i * ATTR_FLOATS;
        rows[i].position_xy = pack_half2(p[0], p[1]);
        rows[i].position_z  = (uint32_t)pack_half(p[2]);
        rows[i].normal_oct  = pack_oct(a);
        rows[i].uv          = pack_half2(a[3], a[4]);
    }
    vert_out->size += (size_t)r->vert_count * sizeof(MuassetVertex);

    if (!buf_reserve(idx_out, (size_t)r->idx_count * sizeof(uint16_t)))
        return false;
    uint16_t *idx = (uint16_t *)(idx_out->data + idx_out->size);
    for (uint32_t i = 0; i < r->idx_count; ++i)
        idx[i] = (uint16_t)r->idx[i];
    idx_out->size += (size_t)r->idx_count * sizeof(uint16_t);

    lod->first_index  = ibase;
    lod->index_count  = r->idx_count;
    lod->first_vertex = vbase;
    lod->error        = r->error;
    return true;
}

/* ============================================================ glTF import */

static void v3_normalize(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) {
        for (int i = 0; i < 3; ++i)
            v[i] /= l;
    } else {
        v[0] = 0.0f;
        v[1] = 1.0f;
        v[2] = 0.0f;
    }
}

/* Normals need the inverse transpose, and a mirror has to be visible to the
   caller so it can flip winding to match. */
static void mat4_inverse_transpose3(const float *m, float *out, float *det_out) {
    float a = m[0], b = m[1], c = m[2];
    float d = m[4], e = m[5], f = m[6];
    float g = m[8], h = m[9], i = m[10];

    float A = (e * i - f * h), B = -(d * i - f * g), C = (d * h - e * g);
    float det = a * A + b * B + c * C;
    *det_out  = det;
    if (fabsf(det) < 1e-20f) {
        memset(out, 0, 9 * sizeof(float));
        out[0] = out[4] = out[8] = 1.0f;
        *det_out = 0.0f;
        return;
    }
    float inv = 1.0f / det;
    out[0] = A * inv;                        out[1] = B * inv;                        out[2] = C * inv;
    out[3] = -(b * i - c * h) * inv;         out[4] = (a * i - c * g) * inv;           out[5] = -(a * h - b * g) * inv;
    out[6] = (b * f - c * e) * inv;          out[7] = -(a * f - c * d) * inv;          out[8] = (a * e - b * d) * inv;
}

static const cgltf_accessor *attr_of(const cgltf_primitive *prim, cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < prim->attributes_count; ++i)
        if (prim->attributes[i].type == type)
            return prim->attributes[i].data;
    return NULL;
}

static uint32_t batch_of(const cgltf_material *mat) {
    if (mat && mat->alpha_mode == cgltf_alpha_mode_blend)
        return MUASSET_BATCH_BLEND;
    if (mat && mat->double_sided)
        return MUASSET_BATCH_DOUBLESIDE;
    return MUASSET_BATCH_OPAQUE;
}

/* The full PBR row. base_color stays UNORM8x4 because that is what the shader
   unpacks; metallic and roughness ride as f32 because that is their precision
   range and there is one row per material, not per vertex. */
static void material_row(MuassetMaterial *row, const cgltf_material *mat, uint32_t albedo, uint32_t normal,
                         uint32_t orm, uint32_t flags) {
    float c[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    float metallic = 0.0f, roughness = 0.7f;
    if (mat && mat->has_pbr_metallic_roughness) {
        const float *f = mat->pbr_metallic_roughness.base_color_factor;
        /* glTF leaves baseColorFactor zeroed when the document omits it, and an
           absent factor means white. Taking the zeros at face value multiplies
           every albedo by black and the whole model renders as a silhouette. */
        if (f[0] || f[1] || f[2] || f[3])
            memcpy(c, f, sizeof(c));
        metallic  = mat->pbr_metallic_roughness.metallic_factor;
        roughness = mat->pbr_metallic_roughness.roughness_factor;
    }
    row->base_color = (uint32_t)(lrintf(c[0] * 255.0f) & 0xFFu) | ((uint32_t)(lrintf(c[1] * 255.0f) & 0xFFu) << 8) |
                      ((uint32_t)(lrintf(c[2] * 255.0f) & 0xFFu) << 16) | ((uint32_t)(lrintf(c[3] * 255.0f) & 0xFFu) << 24);
    row->texture        = albedo;
    row->flags          = flags;
    row->metallic       = metallic;
    row->roughness      = roughness;
    row->normal_texture = normal;
    row->orm_texture    = orm;
    row->pad            = 0;
}

static uint32_t material_color(const cgltf_material *mat) {
    float c[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    if (mat && mat->has_pbr_metallic_roughness)
        memcpy(c, mat->pbr_metallic_roughness.base_color_factor, sizeof(c));
    return (uint32_t)(lrintf(c[0] * 255.0f) & 0xFFu) | ((uint32_t)(lrintf(c[1] * 255.0f) & 0xFFu) << 8) |
           ((uint32_t)(lrintf(c[2] * 255.0f) & 0xFFu) << 16) | ((uint32_t)(lrintf(c[3] * 255.0f) & 0xFFu) << 24);
}

/* Images are decoded and mipped here, once, and written as a flat payload the
   loader uploads without decoding a pixel. That is the same argument as the
   vertex bytes: anything per-pixel the loader could do, the cooker does. The
   cost is disk and in-memory size, since the payload is uncompressed RGBA8 —
   acceptable until there is a block-compressed path, and a far smaller change to
   make later than a load-time PNG decode would be.

   The scratch pair is heap and reused across levels: a 4k image's level 0 is
   64 MB, which is not a thing to put on a cooker's stack. */
typedef struct TexScratch {
    uint8_t *a, *b;
    size_t   cap;
} TexScratch;

static bool tex_scratch_reserve(TexScratch *s, size_t n) {
    if (s->cap >= n)
        return true;
    uint8_t *na = (uint8_t *)realloc(s->a, n);
    if (!na)
        return false;
    uint8_t *nb = (uint8_t *)realloc(s->b, n);
    if (!nb)
        return false;
    s->a   = na;
    s->b   = nb;
    s->cap = n;
    return true;
}

/* Bytes an RGBA8 mip chain of this shape occupies. The payload is flat, so a
   texture's size has to be computable without walking it. */
static uint32_t mip_bytes(uint32_t w, uint32_t h, uint32_t mips) {
    uint32_t total = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        total += w * h * 4u;
        w = w > 1u ? w / 2u : 1u;
        h = h > 1u ? h / 2u : 1u;
    }
    return total;
}

/* A glTF image arrives either as a URI beside the file or as PNG bytes already
   packed into a bufferView. Every exported .glb does the second, and that is
   what all of the KayKit assets do, so the memory path is the one that runs. */
static bool bake_texture(const unsigned char *bytes, int len, TexScratch *sc, uint32_t *w, uint32_t *h,
                         uint32_t *mips, Buf *payload) {
    int      iw = 0, ih = 0, comp = 0;
    stbi_uc *px = len ? stbi_load_from_memory(bytes, len, &iw, &ih, &comp, 4)
                      : stbi_load((const char *)bytes, &iw, &ih, &comp, 4);
    if (!px)
        return false;

    size_t level_bytes = (size_t)iw * (size_t)ih * 4u;
    if (!tex_scratch_reserve(sc, level_bytes)) {
        stbi_image_free(px);
        return false;
    }
    memcpy(sc->a, px, level_bytes);
    stbi_image_free(px);

    *w    = (uint32_t)iw;
    *h    = (uint32_t)ih;
    *mips = 1;
    if (!buf_append(payload, sc->a, level_bytes))
        return false;

    /* Box-filter the tail. A full chain matters more than it looks: without one
       the sampler reaches outside [0,1] for every distant surface and the whole
       scene shimmers. */
    uint32_t lw = (uint32_t)iw, lh = (uint32_t)ih;
    while (lw > 1u || lh > 1u) {
        uint32_t       nw = lw > 1u ? lw / 2u : 1u, nh = lh > 1u ? lh / 2u : 1u;
        size_t         dst_n = (size_t)nw * (size_t)nh * 4u;
        const uint8_t *src   = sc->a;
        uint8_t       *dst   = sc->b;
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x) {
                uint32_t x0 = x * 2u, y0 = y * 2u;
                uint32_t x1 = x0 + 1u < lw ? x0 + 1u : x0, y1 = y0 + 1u < lh ? y0 + 1u : y0;
                for (uint32_t c = 0; c < 4u; ++c)
                    dst[(y * nw + x) * 4u + c] =
                        (uint8_t)((src[(y0 * lw + x0) * 4u + c] + src[(y0 * lw + x1) * 4u + c] +
                                   src[(y1 * lw + x0) * 4u + c] + src[(y1 * lw + x1) * 4u + c] + 2u) >> 2);
            }
        if (!buf_append(payload, dst, dst_n))
            return false;

        uint8_t *swap = sc->a;
        sc->a        = sc->b;
        sc->b        = swap;
        lw = nw;
        lh = nh;
        (*mips)++;
    }
    return true;
}

static uint32_t material_intern(Buf *materials, const MuassetMaterial *row) {
    for (size_t i = 0; i + sizeof(*row) <= materials->size; i += sizeof(*row)) {
        const MuassetMaterial *e = (const MuassetMaterial *)materials->data + i;
        if (e->base_color == row->base_color && e->texture == row->texture && e->flags == row->flags &&
            e->metallic == row->metallic && e->roughness == row->roughness &&
            e->normal_texture == row->normal_texture && e->orm_texture == row->orm_texture)
            return (uint32_t)(i / sizeof(*row));
    }
    buf_append(materials, row, sizeof(*row));
    return (uint32_t)(materials->size / sizeof(*row)) - 1u;
}

/* ============================================================ cook */

#define SECTION_COUNT 7u

bool muasset_cook(const CookOptions *opt, CookStats *stats_out, char *err, uint32_t err_cap) {
    CookStats st = {0};
    Buf       blob = {0}, verts = {0}, idxs = {0}, meshes = {0}, lods = {0}, materials = {0}, textures = {0},
             tex_payload = {0}, names = {0};
    TexScratch tex_scratch = {0};
    char    **node_names = NULL;
    cgltf_data *gltf = NULL;
    Rung      *rungs = NULL;
    bool       ok = false;

    cgltf_options opts = {0};
    if (cgltf_parse_file(&opts, opt->input_path, &gltf) != cgltf_result_success)
        FAIL("cannot parse %s", opt->input_path);
    if (cgltf_load_buffers(&opts, gltf, opt->input_path) != cgltf_result_success)
        FAIL("cannot read the buffers of %s (a .gltf needs its .bin and its textures beside it)", opt->input_path);

    uint32_t prim_total = 0;
    for (cgltf_size n = 0; n < gltf->nodes_count; ++n)
        if (gltf->nodes[n].mesh) {
            st.node_count++;
            prim_total += gltf->nodes[n].mesh->primitives_count;
        }
    if (prim_total == 0)
        FAIL("%s contains no triangles", opt->input_path);
    if (prim_total > MAX_MESHES)
        FAIL("%s has %u primitives; a model is capped at %u", opt->input_path, prim_total, MAX_MESHES);

    rungs      = (Rung *)calloc((size_t)prim_total * MUACOOK_MAX_LODS, sizeof(Rung));
    node_names = (char **)calloc(prim_total, sizeof(char *));
    if (!rungs || !node_names)
        FAIL("out of memory");

    /* Header and section table are reserved now and filled in once every section
       offset is known. */
    blob.size = muasset_align_up((uint32_t)sizeof(MuassetHeader), MUASSET_ALIGN) + SECTION_COUNT * sizeof(MuassetSection);
    if (!buf_reserve(&blob, blob.size))
        FAIL("out of memory");

    /* Bake every image the model references, once, keyed by the glTF image
       index. Materials are interned on top of the resulting ids, so two
       primitives sharing an image share the bindless slot and the payload
       stores it once. */
    int32_t *image_id = (int32_t *)malloc((size_t)(gltf->images_count ? gltf->images_count : 1) * sizeof(int32_t));
    /* How each image is used decides its upload format: an image that is only
       ever an albedo is sRGB, anything that feeds a data channel (normal, ORM)
       is linear. Tracking it per image rather than per material also covers the
       case where two materials share one image in different roles. */
    uint32_t *image_role = (uint32_t *)calloc(gltf->images_count ? gltf->images_count : 1, sizeof(uint32_t));
    if (!image_id || !image_role)
        FAIL("out of memory");
    for (cgltf_size i = 0; i < gltf->images_count; ++i)
        image_id[i] = -1;

    char base_dir[512];
    {
        const char *slash = strrchr(opt->input_path, '/');
        size_t      n     = slash ? (size_t)(slash - opt->input_path) + 1u : 0u;
        if (n >= sizeof(base_dir))
            n = sizeof(base_dir) - 1u;
        memcpy(base_dir, opt->input_path, n);
        base_dir[n] = '\0';
    }

    char **tex_uri = (char **)calloc(gltf->images_count ? gltf->images_count : 1, sizeof(char *));
    if (!tex_uri)
        FAIL("out of memory");
    /* An image touched by any data role has to stay linear. */
    enum { IMG_ALBEDO = 1u, IMG_DATA = 2u };
    for (cgltf_size mi = 0; mi < gltf->materials_count; ++mi) {
        const cgltf_material *mat = &gltf->materials[mi];
        const cgltf_texture *views[3] = {
            mat && mat->has_pbr_metallic_roughness && mat->pbr_metallic_roughness.base_color_texture.texture
                ? mat->pbr_metallic_roughness.base_color_texture.texture
                : NULL,
            mat && mat->normal_texture.texture ? mat->normal_texture.texture : NULL,
            mat && mat->has_pbr_metallic_roughness && mat->pbr_metallic_roughness.metallic_roughness_texture.texture
                ? mat->pbr_metallic_roughness.metallic_roughness_texture.texture
                : NULL};
        static const uint32_t role[3] = {IMG_ALBEDO, IMG_DATA, IMG_DATA};
        for (int v = 0; v < 3; ++v) {
            if (!views[v] || !views[v]->image)
                continue;
            const cgltf_image *img = views[v]->image;
            image_role[img - gltf->images] |= role[v];
            if (image_id[img - gltf->images] < 0) {
                const unsigned char *bytes = NULL;
                int                  len   = 0;
                char                 full[1024];
                full[0] = '\0';
                if (img->buffer_view) {
                    bytes = (const unsigned char *)img->buffer_view->buffer->data + img->buffer_view->offset;
                    len   = (int)img->buffer_view->size;
                } else if (img->uri) {
                    snprintf(full, sizeof(full), "%s%s", base_dir, img->uri);
                    bytes = (const unsigned char *)full;
                    len   = 0; /* len == 0 selects the path branch */
                } else {
                    continue;
                }
                uint32_t w, h, mips;
                if (!bake_texture(bytes, len, &tex_scratch, &w, &h, &mips, &tex_payload))
                    FAIL("cannot decode %s", full[0] ? full : (img->name ? img->name : "embedded image"));
                MuassetTexture trow = {.width     = w,
                                       .height    = h,
                                       .mip_count = mips,
                                       .uri       = 0xFFFFFFFFu,
                                       .offset    = (uint32_t)(tex_payload.size - mip_bytes(w, h, mips)),
                                       .size      = mip_bytes(w, h, mips),
                                       .format    = MUASSET_TEX_RGBA8};
                tex_uri[img - gltf->images] = strdup(img->name ? img->name : (img->uri ? img->uri : "embedded"));
                if (!tex_uri[img - gltf->images] || !buf_append(&textures, &trow, sizeof(trow)))
                    FAIL("out of memory");
                image_id[img - gltf->images] = (int32_t)(textures.size / sizeof(trow)) - 1;
            }
        }
    }
    /* sRGB is a property of use, not of the bytes, so it is stamped on once
       every role is known. An image referenced as both albedo and data stays
       linear: decoding a normal/ORM map as sRGB would corrupt it, while a
       linear albedo only costs the shader one pow(). */
    for (cgltf_size ii = 0; ii < gltf->images_count; ++ii)
        if (image_id[ii] >= 0)
            ((MuassetTexture *)textures.data + image_id[ii])->format =
                (image_role[ii] == IMG_ALBEDO) ? MUASSET_TEX_RGBA8_SRGB : MUASSET_TEX_RGBA8;
    st.texture_count = (uint32_t)(textures.size / sizeof(MuassetTexture));

    uint32_t mesh_index = 0;
    uint32_t batch_mask = 0;

    for (cgltf_size ni = 0; ni < gltf->nodes_count; ++ni) {
        const cgltf_node *node = &gltf->nodes[ni];
        if (!node->mesh)
            continue;
        const cgltf_mesh *mesh = node->mesh;
        const char      *who  = mesh->name ? mesh->name : "(unnamed mesh)";

        float xform[16];
        cgltf_node_transform_world(node, xform);
        float nrm_xform[9], det;
        mat4_inverse_transpose3(xform, nrm_xform, &det);

        for (cgltf_size pi = 0; pi < mesh->primitives_count; ++pi, ++mesh_index) {
            const cgltf_primitive *prim = &mesh->primitives[pi];
            if (prim->type != cgltf_primitive_type_triangles)
                FAIL("%s primitive %u is not triangles; only triangles are cooked", who, (uint32_t)pi);

            const cgltf_accessor *ap = attr_of(prim, cgltf_attribute_type_position);
            const cgltf_accessor *an = attr_of(prim, cgltf_attribute_type_normal);
            if (!ap || !an)
                FAIL("%s primitive %u is missing %s; the cooker does not generate it", who, (uint32_t)pi,
                     ap ? "NORMAL" : "POSITION");

            uint32_t vert_count = (uint32_t)ap->count;
            if (vert_count > 65536u)
                FAIL("%s primitive %u has %u vertices; rung indices are u16 and cap at 65536", who, (uint32_t)pi,
                     vert_count);

            uint32_t idx_count = prim->indices ? (uint32_t)prim->indices->count : vert_count;
            Rung     base;
            if (!rung_alloc(&base, vert_count, idx_count))
                FAIL("out of memory");

            cgltf_accessor_unpack_floats(ap, base.pos, (cgltf_size)(vert_count * 3));

            /* attr[] is ATTR_FLOATS-strided, but cgltf_accessor_unpack_floats
               writes tightly packed floats and takes no destination stride, so
               unpacking normals there and UVs at attr + 3 overwrites the second
               vertex's normal row. Read each element through the accessor
               instead; it applies sparse data and component conversion too. */
            const cgltf_accessor *auv = attr_of(prim, cgltf_attribute_type_texcoord);
            for (uint32_t v = 0; v < vert_count; ++v) {
                float *a = base.attr + (size_t)v * ATTR_FLOATS;
                cgltf_accessor_read_float(an, v, a, 3);
                a[3] = 0.0f;
                a[4] = 0.0f;
                if (auv)
                    cgltf_accessor_read_float(auv, v, a + 3, 2);
            }

            if (prim->indices) {
                cgltf_accessor_unpack_indices(prim->indices, base.idx, sizeof(uint32_t), prim->indices->count);
            } else {
                for (uint32_t i = 0; i < vert_count; ++i)
                    base.idx[i] = i;
            }

            bool skinned = attr_of(prim, cgltf_attribute_type_joints) && attr_of(prim, cgltf_attribute_type_weights) &&
                          node->skin;
            if (skinned) {
                if (!opt->bake_rest_pose)
                    FAIL("%s primitive %u is skinned; pass --bake-rest-pose to cook it at its bind pose", who,
                         (uint32_t)pi);
                st.baked_rigs++;
            }

            /* Bake the node transform so the model ends up in one space and the
               runtime never has a hierarchy to walk. A negative determinant
               mirrors the geometry, so winding has to flip with it. */
            for (uint32_t v = 0; v < vert_count; ++v) {
                const float *p = base.pos + (size_t)v * 3;
                const float *n = base.attr + (size_t)v * ATTR_FLOATS;
                float px = xform[0] * p[0] + xform[4] * p[1] + xform[8] * p[2] + xform[12];
                float py = xform[1] * p[0] + xform[5] * p[1] + xform[9] * p[2] + xform[13];
                float pz = xform[2] * p[0] + xform[6] * p[1] + xform[10] * p[2] + xform[14];
                base.pos[v * 3 + 0] = px;
                base.pos[v * 3 + 1] = py;
                base.pos[v * 3 + 2] = pz;

                float nrm[3] = {nrm_xform[0] * n[0] + nrm_xform[3] * n[1] + nrm_xform[6] * n[2],
                                nrm_xform[1] * n[0] + nrm_xform[4] * n[1] + nrm_xform[7] * n[2],
                                nrm_xform[2] * n[0] + nrm_xform[5] * n[1] + nrm_xform[8] * n[2]};
                /* Only a mirror reverses the normal. A double-sided flip is a
                   rasterizer convention, not a change to the surface, so it
                   must leave the shading normal alone. */
                if (det < 0.0f)
                    for (int k = 0; k < 3; ++k)
                        nrm[k] = -nrm[k];
                v3_normalize(nrm);
                for (int k = 0; k < 3; ++k)
                    base.attr[v * ATTR_FLOATS + k] = nrm[k];
            }
            /* Winding. Two independent reasons to reverse a triangle, and they
               cancel: a negative-determinant node transform mirrors the
               geometry, and a double-sided primitive has no consistent front.
               The renderer draws with back-face culling, and routing double-
               sided meshes to a second no-cull pipeline is later work, so a
               double-sided primitive is flipped here instead. Two flips cancel,
               which is exactly what this expression says. */
            const cgltf_material *prim_mat = prim->material;
            uint32_t              batch    = batch_of(prim_mat);
            bool                  flip     = (det < 0.0f) != (batch == MUASSET_BATCH_DOUBLESIDE);

            if (flip)
                for (uint32_t i = 0; i + 2 < base.idx_count; i += 3) {
                    uint32_t swap  = base.idx[i + 1];
                    base.idx[i + 1] = base.idx[i + 2];
                    base.idx[i + 2] = swap;
                }

            if (!rung_weld(&base))
                FAIL("out of memory welding %s primitive %u", who, (uint32_t)pi);
            rungs[mesh_index * MUACOOK_MAX_LODS] = base;

            uint32_t rung_count = 1;
            for (uint32_t k = 1; k < opt->lod_count && k < MUACOOK_MAX_LODS; ++k) {
                const Rung *src = &rungs[mesh_index * MUACOOK_MAX_LODS + k - 1];
                uint32_t    target = (uint32_t)((float)src->idx_count * kLodRatio[k]);
                target            = target - (target % 3u);
                if (target < 3u || !rung_simplify(&rungs[mesh_index * MUACOOK_MAX_LODS + k], src, target))
                    break;
                /* Topology and seams can leave a rung unable to collapse
                   anything. A rung that is not meaningfully cheaper is a rung
                   the runtime could pick and pay for nothing, so stop here and
                   let the padding below duplicate the coarsest real rung. */
                if (rungs[mesh_index * MUACOOK_MAX_LODS + k].idx_count * 10u >= src->idx_count * 9u)
                    break;
                rung_count = k + 1;
            }

            /* Bounds come from rung 0 and every rung is padded to the model
               width, so the coarsest rung can never reach outside them by more
               than its own simplification error. */
            struct meshopt_Bounds bounds = meshopt_computeSphereBounds(rungs[mesh_index * MUACOOK_MAX_LODS].pos,
                                                                rungs[mesh_index * MUACOOK_MAX_LODS].vert_count,
                                                                POS_STRIDE, NULL, 0);
            float radius = bounds.radius > 0.0f ? bounds.radius : 1.0f;
            for (uint32_t k = 1; k < rung_count; ++k)
                rungs[mesh_index * MUACOOK_MAX_LODS + k].error *= radius;

            for (uint32_t k = 0; k < rung_count; ++k) {
                Rung *r = &rungs[mesh_index * MUACOOK_MAX_LODS + k];
                if (!rung_optimize(r))
                    FAIL("out of memory optimizing %s primitive %u rung %u", who, (uint32_t)pi, k);
                if (r->vert_count > 65536u)
                    FAIL("%s primitive %u rung %u holds %u vertices; rung indices are u16", who, (uint32_t)pi, k,
                         r->vert_count);
            }

            uint32_t lod_first = (uint32_t)(lods.size / sizeof(MuassetLod));
            if (opt->stats)
                fprintf(stderr, "  %s/%u: rung 0 %u idx / %u verts (r %g)", who, (uint32_t)pi,
                        rungs[mesh_index * MUACOOK_MAX_LODS].idx_count, rungs[mesh_index * MUACOOK_MAX_LODS].vert_count, bounds.radius);
            for (uint32_t k = 0; k < rung_count; ++k) {
                MuassetLod row;
                if (!rung_emit(&rungs[mesh_index * MUACOOK_MAX_LODS + k], &verts, &idxs, &row))
                    FAIL("out of memory emitting %s primitive %u", who, (uint32_t)pi);
                if (opt->stats)
                    fprintf(stderr, " | %u idx / %u verts err %g", row.index_count,
                            rungs[mesh_index * MUACOOK_MAX_LODS + k].vert_count, row.error);
                if (!buf_append(&lods, &row, sizeof(row)))
                    FAIL("out of memory");
            }
            if (opt->stats)
                fprintf(stderr, "\n");
            /* Every mesh in a model carries the same ladder length, because the
               runtime derives a group id as mesh * lod_count + lod against one
               global lod_count. A short ladder repeats its coarsest rung, which
               costs a duplicate row and keeps the group math branch-free. */
            for (uint32_t k = rung_count; k < opt->lod_count; ++k) {
                MuassetLod row;
                memcpy(&row, (const MuassetLod *)lods.data + (lods.size / sizeof(MuassetLod)) - 1u, sizeof(row));
                if (!buf_append(&lods, &row, sizeof(row)))
                    FAIL("out of memory");
            }

            uint32_t albedo = 0xFFFFFFFFu, normal_tex = 0xFFFFFFFFu, orm_tex = 0xFFFFFFFFu, mat_flags = 0;
            if (prim_mat && prim_mat->has_pbr_metallic_roughness &&
                prim_mat->pbr_metallic_roughness.base_color_texture.texture &&
                prim_mat->pbr_metallic_roughness.base_color_texture.texture->image) {
                int32_t id = image_id[prim_mat->pbr_metallic_roughness.base_color_texture.texture->image -
                                      gltf->images];
                if (id >= 0) {
                    albedo    = (uint32_t)id;
                    mat_flags = MUASSET_MAT_HAS_ALBEDO;
                }
            }
            if (prim_mat && prim_mat->normal_texture.texture && prim_mat->normal_texture.texture->image) {
                int32_t id = image_id[prim_mat->normal_texture.texture->image - gltf->images];
                if (id >= 0) {
                    normal_tex = (uint32_t)id;
                    mat_flags |= MUASSET_MAT_HAS_NORMAL;
                }
            }
            if (prim_mat && prim_mat->has_pbr_metallic_roughness &&
                prim_mat->pbr_metallic_roughness.metallic_roughness_texture.texture &&
                prim_mat->pbr_metallic_roughness.metallic_roughness_texture.texture->image) {
                int32_t id = image_id[prim_mat->pbr_metallic_roughness.metallic_roughness_texture.texture->image -
                                      gltf->images];
                if (id >= 0) {
                    orm_tex = (uint32_t)id;
                    mat_flags |= MUASSET_MAT_HAS_ORM;
                }
            }
            if (prim_mat && prim_mat->unlit)
                mat_flags |= MUASSET_MAT_UNLIT;

            MuassetMaterial mrow;
            material_row(&mrow, prim_mat, albedo, normal_tex, orm_tex, mat_flags);
            MuassetMesh mesh_row = {
                .local_sphere = {bounds.center[0], bounds.center[1], bounds.center[2], radius},
                .lod_first    = lod_first,
                .lod_count    = opt->lod_count,
                .material     = material_intern(&materials, &mrow),
                .batch        = batch,
            };
            if (!buf_append(&meshes, &mesh_row, sizeof(mesh_row)))
                FAIL("out of memory");
            batch_mask |= 1u << batch;
            st.mesh_count++;

            node_names[mesh_index] = node->name ? strdup(node->name) : NULL;
        }
    }

    /* names: [u32 count][u32 offset[count+1]][bytes]. Mesh names come first so
       muasset_name_at() indexes them the same way the mesh table does; the
       texture URIs follow, which is why a texture row stores an index rather
       than a pointer. */
    {
        uint32_t  count = st.mesh_count + st.texture_count;
        uint32_t *offs   = (uint32_t *)calloc((size_t)count + 1u, sizeof(uint32_t));
        if (!offs)
            FAIL("out of memory");
        size_t used = 0;
        for (uint32_t i = 0; i < count; ++i) {
            offs[i] = (uint32_t)used;
            const char *n = i < st.mesh_count ? node_names[i] : tex_uri[i - st.mesh_count];
            if (n) {
                size_t len = strlen(n) + 1u;
                if (!buf_append(&names, n, len)) {
                    free(offs);
                    FAIL("out of memory");
                }
                used += len;
                if (i < st.mesh_count)
                    continue;
                /* point the texture row at its own name */
                MuassetTexture *t = (MuassetTexture *)textures.data + (i - st.mesh_count);
                t->uri            = (uint32_t)offs[i];
            }
        }
        offs[count] = (uint32_t)used;

        Buf table = {0};
        if (!buf_append(&table, &count, sizeof(count)) || !buf_append(&table, offs, (size_t)(count + 1u) * sizeof(uint32_t)))
            FAIL("out of memory");
        free(offs);
        if (!buf_append(&table, names.data, names.size))
            FAIL("out of memory");
        free(names.data);
        names = table;
    }

    /* ---- assemble ---- */
    const struct {
        uint32_t   tag;
        Buf       *buf;
        uint32_t   row_size;
    } sections[SECTION_COUNT] = {
        {MUASEC_VERT_ARENA, &verts, (uint32_t)sizeof(MuassetVertex)},
        {MUASEC_IDX_ARENA, &idxs, 2u},
        {MUASEC_MESH, &meshes, (uint32_t)sizeof(MuassetMesh)},
        {MUASEC_LOD, &lods, (uint32_t)sizeof(MuassetLod)},
        {MUASEC_MATERIAL, &materials, (uint32_t)sizeof(MuassetMaterial)},
        {MUASEC_TEXTURE, &textures, (uint32_t)sizeof(MuassetTexture)},
        {MUASEC_NAMES, &names, 1u},
    };

    MuassetSection table[SECTION_COUNT];
    for (uint32_t i = 0; i < SECTION_COUNT; ++i) {
        if (!buf_pad16(&blob))
            FAIL("out of memory");
        /* row_count is the real row count, so it has to be taken before the
           section is padded; size and checksum cover the padded region, because
           that is what lands in the file and what the next section's offset is
           measured from. */
        uint32_t rows = (uint32_t)(sections[i].buf->size / sections[i].row_size);
        if (!buf_pad16(sections[i].buf))
            FAIL("out of memory");
        table[i].tag       = sections[i].tag;
        table[i].row_size  = sections[i].row_size;
        table[i].row_count = rows;
        table[i].offset    = (uint32_t)blob.size;
        table[i].size      = (uint32_t)sections[i].buf->size;
        table[i].checksum  = muasset_crc32(sections[i].buf->data, (uint32_t)sections[i].buf->size);
        table[i].pad[0]    = 0;
        table[i].pad[1]    = 0;
        if (!buf_append(&blob, sections[i].buf->data, sections[i].buf->size))
            FAIL("out of memory");
    }

    MuassetHeader header = {
        .magic          = MUASSET_MAGIC,
        .version        = (MUASSET_MAJOR << 16) | MUASSET_MINOR,
        .section_count  = SECTION_COUNT,
        .section_offset = muasset_align_up((uint32_t)sizeof(MuassetHeader), MUASSET_ALIGN),
        .total_size     = (uint32_t)blob.size,
        .flags          = (opt->lod_count > 1u ? MUASSET_FLAG_LODS : 0u) |
                     ((batch_mask & ~(1u << MUASSET_BATCH_OPAQUE)) ? MUASSET_FLAG_MULTI_BATCH : 0u),
    };
    memcpy(blob.data, &header, sizeof(header));
    memcpy(blob.data + header.section_offset, table, sizeof(table));

    /* Never ship a blob this build cannot read back. The reason goes through a
       separate buffer: writing it into err while err is also the format source
       would have snprintf read the string it is overwriting. */
    {
        char why[256];
        if (!muasset_validate(blob.data, (uint32_t)blob.size, why, sizeof(why)))
            FAIL("cooked blob failed its own validation: %s", why);
    }

    FILE *f = fopen(opt->output_path, "wb");
    if (!f)
        FAIL("cannot open %s for writing", opt->output_path);
    if (fwrite(blob.data, 1, blob.size, f) != blob.size) {
        fclose(f);
        FAIL("short write to %s", opt->output_path);
    }
    fclose(f);

    /* The images themselves live beside the blob, not inside it: they re-cook
       and stream on their own schedule, and packing megabytes of them into the
       geometry file would mean re-cooking the geometry every time a material
       tweaks a scalar. */
    if (tex_payload.size) {
        char tex_path[1024];
        snprintf(tex_path, sizeof(tex_path), "%s.tex", opt->output_path);
        FILE *tf = fopen(tex_path, "wb");
        if (!tf)
            FAIL("cannot open %s for writing", tex_path);
        if (fwrite(tex_payload.data, 1, tex_payload.size, tf) != tex_payload.size) {
            fclose(tf);
            FAIL("short write to %s", tex_path);
        }
        fclose(tf);
    }

    /* Counted from the section table, not from the staging buffers: the
       sections are padded to 16 bytes and that padding must not be reported as
       geometry. */
    st.vertex_count = table[0].row_count;
    st.index_count  = table[1].row_count;
    st.lod_count    = opt->lod_count;
    st.vert_bytes   = table[0].size;
    st.idx_bytes    = table[1].size;
    st.batch_count  = (uint32_t)__builtin_popcount(batch_mask);
    *stats_out      = st;
    ok              = true;

fail:
    if (rungs)
        for (uint32_t k = 0; k < prim_total * MUACOOK_MAX_LODS; ++k)
            rung_free(&rungs[k]);
    free(rungs);
    if (node_names)
        for (uint32_t k = 0; k < prim_total; ++k)
            free(node_names[k]);
    free(node_names);
    if (tex_uri)
        for (cgltf_size k = 0; k < gltf->images_count; ++k)
            free(tex_uri[k]);
    free(tex_uri);
    free(image_id);
    free(image_role);
    free(verts.data);
    free(idxs.data);
    free(meshes.data);
    free(lods.data);
    free(materials.data);
    free(textures.data);
    free(tex_payload.data);
    free(tex_scratch.a);
    free(tex_scratch.b);
    free(names.data);
    free(blob.data);
    cgltf_free(gltf);
    return ok;
}

/* ============================================================ dump / validate */

static uint8_t *read_file(const char *path, uint32_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (uint32_t)n;
    return data;
}

static const char *section_name(uint32_t tag) {
    switch (tag) {
        case MUASEC_VERT_ARENA: return "VERT_ARENA";
        case MUASEC_IDX_ARENA:  return "IDX_ARENA";
        case MUASEC_MESH:       return "MESH";
        case MUASEC_LOD:        return "LOD";
        case MUASEC_MATERIAL:   return "MATERIAL";
        case MUASEC_TEXTURE:    return "TEXTURE";
        case MUASEC_NAMES:      return "NAMES";
        default:                return "?";
    }
}

static const char *name_at(const Muasset *m, uint32_t i) { return muasset_name_at(m, i); }

static int dump(const char *path) {
    uint32_t size = 0;
    uint8_t *data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    char       err[256];
    Muasset    m;
    if (!muasset_open(data, size, &m, err, sizeof(err))) {
        fprintf(stderr, "%s: %s\n", path, err);
        free(data);
        return 1;
    }
    if (!muasset_validate(data, size, err, sizeof(err))) {
        fprintf(stderr, "%s: %s\n", path, err);
        free(data);
        return 1;
    }

    printf("%s  %u bytes  format %u.%u  flags 0x%X\n", path, size, m.header->version >> 16,
           m.header->version & 0xFFFFu, m.header->flags);
    printf("%-12s %10s %10s %10s %10s\n", "section", "rows", "row_size", "offset", "bytes");
    for (uint32_t i = 0; i < m.header->section_count; ++i) {
        const MuassetSection *s = &m.sections[i];
        printf("%-12s %10u %10u %10u %10u  crc %08X\n", section_name(s->tag), s->row_count, s->row_size, s->offset,
               s->size, s->checksum);
    }
    printf("\nmeshes %u  rungs %u  vertices %u  indices %u  materials %u\n", m.mesh_count, m.lod_count, m.vertex_count,
           m.index_count, m.material_count);
    printf("\n%-4s %-22s %-8s %-6s %-24s %-6s %s\n", "id", "name", "batch", "mat", "sphere(center,radius)", "rungs",
           "indices/rung");
    for (uint32_t i = 0; i < m.mesh_count; ++i) {
        const MuassetMesh *mesh = &m.meshes[i];
        printf("%-4u %-22s %-8u %-6u (%8.3f %8.3f %8.3f %7.3f) %-6u ", i, name_at(&m, i), mesh->batch, mesh->material,
               mesh->local_sphere[0], mesh->local_sphere[1], mesh->local_sphere[2], mesh->local_sphere[3],
               mesh->lod_count);
        for (uint32_t r = 0; r < mesh->lod_count; ++r)
            printf("%u%s", m.lods[mesh->lod_first + r].index_count, r + 1u == mesh->lod_count ? "" : "/");
        printf("\n");
    }
    free(data);
    return 0;
}

/* ============================================================ main */

static void usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  muasset_cook <in.gltf> -o <out.mua> [--lods N] [--bake-rest-pose] [--stats]\n"
            "  muasset_cook --dump <file.mua>\n"
            "  muasset_cook --validate <file.mua>\n"
            "\n"
            "  --lods N             rungs per mesh, 1..%u (100/50/25/12.5%% of triangles)\n"
            "  --bake-rest-pose     cook skinned input at its bind pose instead of failing\n"
            "  --stats              print the cook budget\n"
            "  --dump               print the blob's tables and validate it\n"
            "  --validate           check magic, offsets, row sizes and crc32 only\n",
            MUACOOK_MAX_LODS);
}

int main(int argc, char **argv) {
    const char *input = NULL, *output = NULL;
    uint32_t    lod_count = 3;
    bool        bake_rest_pose = false, stats = false, want_dump = false, want_validate = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "--lods") == 0 && i + 1 < argc)
            lod_count = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--bake-rest-pose") == 0)
            bake_rest_pose = true;
        else if (strcmp(argv[i], "--stats") == 0)
            stats = true;
        else if (strcmp(argv[i], "--dump") == 0)
            want_dump = true;
        else if (strcmp(argv[i], "--validate") == 0)
            want_validate = true;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else
            input = argv[i];
    }

    if (lod_count < 1u || lod_count > MUACOOK_MAX_LODS) {
        fprintf(stderr, "--lods must be 1..%u\n", MUACOOK_MAX_LODS);
        return 1;
    }

    if (want_dump || want_validate) {
        if (!input) {
            usage();
            return 1;
        }
        uint32_t size = 0;
        uint8_t *data = read_file(input, &size);
        if (!data) {
            fprintf(stderr, "cannot read %s\n", input);
            return 1;
        }
        char err[256];
        if (!muasset_validate(data, size, err, sizeof(err))) {
            fprintf(stderr, "%s: %s\n", input, err);
            free(data);
            return 1;
        }
        if (!want_dump) {
            printf("%s: ok, %u bytes\n", input, size);
            free(data);
            return 0;
        }
        free(data);
        return dump(input);
    }

    if (!input || !output) {
        usage();
        return 1;
    }

    CookStats  st = {0};
    char       err[512] = {0};
    CookOptions opt = {
        .input_path    = input,
        .output_path   = output,
        .lod_count     = lod_count,
        .bake_rest_pose = bake_rest_pose,
        .stats         = stats,
    };
    if (!muasset_cook(&opt, &st, err, sizeof(err))) {
        fprintf(stderr, "cook failed: %s\n", err);
        return 1;
    }

    printf("%s -> %s\n", input, output);
    printf("  meshes %u  rungs %u  vertices %u  indices %u  vert_bytes %u  idx_bytes %u\n", st.mesh_count, st.lod_count,
           st.vertex_count, st.index_count, st.vert_bytes, st.idx_bytes);
    if (st.baked_rigs)
        printf("  %u skinned primitives cooked at their bind pose\n", st.baked_rigs);
    if (stats)
        printf("  batches %u  nodes %u\n", st.batch_count, st.node_count);
    return 0;
}