/* muasset.c — blob binding and validation. Shared by the cooker (to read back
   what it just wrote) and the runtime loader, so the two can never disagree
   about what a well-formed blob is.
 *
 * muasset_open is the load path: it touches the header and the section table
   only. Section payloads are reached by pointer arithmetic and handed to the
 * GPU as-is. */

#include "muasset.h"

#include <stdio.h>
#include <string.h>

uint32_t muasset_align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

uint32_t muasset_crc32(const uint8_t *data, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (uint32_t k = 0; k < 8u; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

uint32_t muasset_section_row_size(uint32_t tag) {
    switch (tag) {
        case MUASEC_VERT_ARENA: return (uint32_t)sizeof(MuassetVertex);
        case MUASEC_IDX_ARENA:  return 2u;
        case MUASEC_MESH:       return (uint32_t)sizeof(MuassetMesh);
        case MUASEC_LOD:        return (uint32_t)sizeof(MuassetLod);
        case MUASEC_MATERIAL:   return (uint32_t)sizeof(MuassetMaterial);
        case MUASEC_TEXTURE:    return (uint32_t)sizeof(MuassetTexture);
        case MUASEC_NAMES:      return 1u;
        default:                return 0u;
    }
}

/* A blob is valid when every section lies inside it and no two sections claim
   the same byte. Overlap is the failure mode that produces plausible-looking
   garbage instead of an obvious one, so it is checked explicitly rather than
   inferred from offsets. */
static bool sections_sane(const MuassetHeader *h, const MuassetSection *secs, uint32_t n, char *err,
                          uint32_t err_cap) {
    for (uint32_t i = 0; i < n; ++i) {
        const MuassetSection *s = &secs[i];
        if ((uint64_t)s->offset + s->size > h->total_size) {
            snprintf(err, err_cap, "section[%u] tag %u runs past the blob (off %u size %u total %u)", i, s->tag,
                     s->offset, s->size, h->total_size);
            return false;
        }
        if (s->size && (s->offset & (MUASSET_ALIGN - 1u))) {
            snprintf(err, err_cap, "section[%u] tag %u offset %u is not 16-aligned", i, s->tag, s->offset);
            return false;
        }
        for (uint32_t j = i + 1u; j < n; ++j) {
            const MuassetSection *o = &secs[j];
            if (!s->size || !o->size)
                continue;
            if (s->offset < o->offset + o->size && o->offset < s->offset + s->size) {
                snprintf(err, err_cap, "section[%u] tag %u overlaps section[%u] tag %u", i, s->tag, j, o->tag);
                return false;
            }
        }
    }
    return true;
}

bool muasset_open(const void *data, uint32_t size, Muasset *out, char *err, uint32_t err_cap) {
    memset(out, 0, sizeof(*out));
    if (size < sizeof(MuassetHeader)) {
        snprintf(err, err_cap, "file is %u bytes, smaller than the %u-byte header", size,
                 (uint32_t)sizeof(MuassetHeader));
        return false;
    }

    const MuassetHeader *h = (const MuassetHeader *)data;
    if (h->magic != MUASSET_MAGIC) {
        snprintf(err, err_cap, "bad magic 0x%08X, expected 0x%08X", h->magic, MUASSET_MAGIC);
        return false;
    }
    if ((h->version >> 16) != MUASSET_MAJOR) {
        snprintf(err, err_cap, "need re-cook: blob is %u.x, this build wants %u.x", h->version >> 16, MUASSET_MAJOR);
        return false;
    }
    if (h->total_size != size) {
        snprintf(err, err_cap, "header says %u bytes, file is %u", h->total_size, size);
        return false;
    }
    if (h->section_offset < sizeof(MuassetHeader) || h->section_count == 0u ||
        h->section_offset + h->section_count * sizeof(MuassetSection) > size) {
        snprintf(err, err_cap, "section table (off %u, %u rows) does not fit in %u bytes", h->section_offset,
                 h->section_count, size);
        return false;
    }

    const MuassetSection *secs = (const MuassetSection *)((const uint8_t *)data + h->section_offset);
    if (!sections_sane(h, secs, h->section_count, err, err_cap))
        return false;

    out->data     = (const uint8_t *)data;
    out->size     = size;
    out->header   = h;
    out->sections = secs;

    for (uint32_t i = 0; i < h->section_count; ++i) {
        const MuassetSection *s  = &secs[i];
        const void           *p  = (const uint8_t *)data + s->offset;
        uint32_t              rv = s->row_size ? s->row_count : 0u;

        switch (s->tag) {
            case MUASEC_VERT_ARENA:
                out->vertices     = (const MuassetVertex *)p;
                out->vertex_count = rv;
                break;
            case MUASEC_IDX_ARENA:
                out->indices     = (const uint16_t *)p;
                out->index_count = rv;
                break;
            case MUASEC_MESH:
                out->meshes     = (const MuassetMesh *)p;
                out->mesh_count = rv;
                break;
            case MUASEC_LOD:
                out->lods      = (const MuassetLod *)p;
                out->lod_count = rv;
                break;
            case MUASEC_MATERIAL:
                out->materials     = (const MuassetMaterial *)p;
                out->material_count = rv;
                break;
            case MUASEC_TEXTURE:
                out->textures     = (const MuassetTexture *)p;
                out->texture_count = rv;
                break;
            case MUASEC_NAMES: {
                uint32_t       names = *(const uint32_t *)p;
                const uint32_t *offs = (const uint32_t *)((const uint8_t *)p + 4);
                out->name_count      = names;
                out->name_offsets    = offs;
                out->name_bytes      = (const char *)(offs + names + 1u);
                break;
            }
            default:
                break; /* an unknown tag is additive: ignore it, do not fail */
        }
    }
    return true;
}

bool muasset_validate(const void *data, uint32_t size, char *err, uint32_t err_cap) {
    Muasset m;
    if (!muasset_open(data, size, &m, err, err_cap))
        return false;

    for (uint32_t i = 0; i < m.header->section_count; ++i) {
        const MuassetSection *s = &m.sections[i];
        uint32_t              want = muasset_section_row_size(s->tag);
        if (want && s->row_size != want) {
            snprintf(err, err_cap, "section[%u] tag %u row_size %u, this build wants %u", i, s->tag, s->row_size,
                     want);
            return false;
        }
        if (s->size != muasset_align_up(want * s->row_count, MUASSET_ALIGN)) {
            snprintf(err, err_cap, "section[%u] tag %u size %u, rows need %u", i, s->tag, s->size,
                     muasset_align_up(want * s->row_count, MUASSET_ALIGN));
            return false;
        }
        if (s->checksum != muasset_crc32(m.data + s->offset, s->size)) {
            snprintf(err, err_cap, "section[%u] tag %u crc32 mismatch", i, s->tag);
            return false;
        }
    }

    if (!m.meshes || !m.lods || !m.vertices || !m.indices) {
        snprintf(err, err_cap, "blob is missing one of the mandatory sections");
        return false;
    }
    for (uint32_t i = 0; i < m.mesh_count; ++i) {
        const MuassetMesh *mesh = &m.meshes[i];
        if (mesh->lod_count == 0u || mesh->lod_first + mesh->lod_count > m.lod_count) {
            snprintf(err, err_cap, "mesh %u ladder [%u,%u) does not fit in %u rungs", i, mesh->lod_first,
                     mesh->lod_first + mesh->lod_count, m.lod_count);
            return false;
        }
        if (mesh->material >= m.material_count) {
            snprintf(err, err_cap, "mesh %u names material %u, only %u exist", i, mesh->material, m.material_count);
            return false;
        }
        for (uint32_t r = 0; r < mesh->lod_count; ++r) {
            const MuassetLod *lod = &m.lods[mesh->lod_first + r];
            if (lod->first_index + lod->index_count > m.index_count ||
                lod->first_vertex + 1u > m.vertex_count) {
                snprintf(err, err_cap, "mesh %u rung %u range exceeds its arenas", i, r);
                return false;
            }
            /* Indices are rung-local: the draw supplies the vertex base, so an
               index that names another rung's vertices is a cooker bug that
               would otherwise read silently wrong geometry. */
            for (uint32_t k = 0; k < lod->index_count; ++k) {
                if (m.indices[lod->first_index + k] + lod->first_vertex >= m.vertex_count) {
                    snprintf(err, err_cap, "mesh %u rung %u index %u is out of range", i, r, k);
                    return false;
                }
            }
        }
    }
    return true;
}