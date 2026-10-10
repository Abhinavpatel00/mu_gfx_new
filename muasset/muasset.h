#ifndef MUASSET_H
#define MUASSET_H

/* `.mua` 1.0 — cooked GPU-ready asset blob. Spec: docs/muasset-format.md.
 *
 * The blob holds the exact bytes the GPU reads. Vertex rows are the final
 * 16-byte packed form (half3 position, snorm16x2 octahedral normal, half2 uv),
 * indices are u16 and already cache- and fetch-optimized, and every row's field
 * order matches the shader's read order. The runtime load path is therefore
 * validate -> one copy -> one upload: no deinterleave, no quantize, no
 * per-vertex math. Whatever the cooker could have done once, it did once.
 *
 * A .mua is one model in one common space. Node transforms are baked into the
 * vertices, so a model is a list of meshes that the caller instantiates at
 * identity. That is a consequence of not having a scene graph on either side.
 *
 * Little-endian. Offsets only, never pointers. Sections are 16-aligned. */

#include <stdbool.h>
#include <stdint.h>

#define MUASSET_MAGIC 0x2E41554Du /* ".MUA" read as a little-endian u32 */
#define MUASSET_MAJOR 1u
#define MUASSET_MINOR 0u

/* Section tags are a stable ABI: append only, never renumber. */
#define MUASEC_VERT_ARENA 1u /* MuassetVertex rows; all meshes x all rungs */
#define MUASEC_IDX_ARENA  2u /* u16 rows; rung-local indices, all meshes    */
#define MUASEC_MESH       3u /* MuassetMesh rows, one per glTF primitive    */
#define MUASEC_LOD        4u /* MuassetLod rows; ladders of mesh.lod_first   */
#define MUASEC_MATERIAL   5u /* MuassetMaterial rows, byte-identical to the
                              * GPU material row the fragment shader reads   */
#define MUASEC_TEXTURE    6u /* MuassetTexture rows: images cooked to RGBA8 */
#define MUASEC_NAMES      7u /* debug only: [u32 count][u32 off[count+1]][b]  */

/* Header flag bits. */
#define MUASSET_FLAG_MULTI_BATCH 0x1u /* more than one batch id in use      */
#define MUASSET_FLAG_LODS         0x2u /* some mesh has more than one rung  */
#define MUASSET_FLAG_SKIN         0x4u /* reserved; never set in 1.0        */

/* Batch ids. A batch is fixed pipeline state decided at cook time, so the
   runtime never tests a material flag to choose how to draw. */
#define MUASSET_BATCH_OPAQUE     0u
#define MUASSET_BATCH_DOUBLESIDE 1u
#define MUASSET_BATCH_BLEND      2u
#define MUASSET_BATCH_COUNT      3u

#define MUASSET_ALIGN 16u

typedef struct MuassetHeader {
    uint32_t magic;          /* MUASSET_MAGIC                                */
    uint32_t version;        /* major << 16 | minor                          */
    uint32_t section_count;
    uint32_t section_offset; /* byte offset of the section table, 16-aligned */
    uint32_t total_size;     /* whole blob, header included                  */
    uint32_t flags;          /* MUASSET_FLAG_*                               */
} MuassetHeader;

typedef struct MuassetSection {
    uint32_t tag;       /* MUASEC_*                                              */
    uint32_t row_size;  /* bytes per row; 2 for the index arena, 1 for NAMES    */
    uint32_t row_count;
    uint32_t offset;    /* from blob start, 16-aligned                          */
    uint32_t size;      /* row_size * row_count, already padded to 16           */
    uint32_t checksum;  /* crc32 of the section bytes; checked by `validate`,
                         * not on the load path, because hashing a blob forces
                         * reading every byte we just mmap'd for free          */
    uint32_t pad[2];
} MuassetSection;

/* 16 B. Field order is the shader's read order, so the load path memcpy's this
   straight into the vertex arena and the VS reads it as one uint4. Byte-
   identical to ScenePackedVertex; scene_asset.c asserts that. */
typedef struct MuassetVertex {
    uint32_t position_xy; /* half2: x low, y high                             */
    uint32_t position_z;  /* half in low 16 bits                              */
    uint32_t normal_oct;  /* snorm16x2 octahedral                             */
    uint32_t uv;          /* half2                                            */
} MuassetVertex;

/* 32 B. One per glTF primitive. Every field is something the runtime reads;
   anything derivable (vertex count, index count) lives in the LOD rows. */
typedef struct MuassetMesh {
    float    local_sphere[4]; /* xyz centre, w radius; conservative over every rung */
    uint32_t lod_first;       /* base index into the LOD section                   */
    uint32_t lod_count;       /* rungs on this mesh; ladders are padded to the
                               * model-wide max by the cooker                      */
    uint32_t material;        /* index into MUASEC_MATERIAL                       */
    uint32_t batch;           /* MUASSET_BATCH_*                                  */
} MuassetMesh;

/* 16 B. first_vertex is relative to the mesh's own vertex block, which starts
   at the LOD0 rung's first_vertex within MUASEC_VERT_ARENA. Rung 0's error is
   never read: the cull walk starts at rung 0 and only tests rungs 1..n. */
typedef struct MuassetLod {
    uint32_t first_index;  /* into MUASEC_IDX_ARENA; rung-local index values  */
    uint32_t index_count;
    uint32_t first_vertex; /* into MUASEC_VERT_ARENA                            */
    float    error;        /* screen-space error threshold, ascending over rungs */
} MuassetLod;

/* 32 B. Byte-identical to SceneGpuMaterial, so materials are a memcpy like
   vertices. Texture ids are indices into the runtime's bindless array, not
   paths: the cooker resolves and decodes images once, at cook time, and the
   load path only ever uploads bytes. */
typedef struct MuassetMaterial {
    uint32_t base_color;    /* UNORM8x4 */
    uint32_t texture;       /* bindless albedo id, 0xFFFFFFFF = none      */
    uint32_t flags;         /* MUASSET_MAT_* below                         */
    float    metallic;
    float    roughness;
    uint32_t normal_texture; /* bindless normal id, 0xFFFFFFFF = none     */
    uint32_t orm_texture;
    uint32_t pad;
} MuassetMaterial;

#define MUASSET_MAT_HAS_ALBEDO   0x1u
#define MUASSET_MAT_HAS_NORMAL   0x2u
#define MUASSET_MAT_UNLIT        0x4u
#define MUASSET_MAT_HAS_ORM      0x8u

/* 20 B. One image: where it came from, and the decoded mip chain the loader
   uploads without touching a pixel. Kept out of the blob's hot path — images
   stream and re-cook independently of geometry. */
typedef struct MuassetTexture {
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;
    uint32_t uri;    /* into the NAMES blob */
    uint32_t offset; /* bytes into the companion .tex payload */
    uint32_t size;
    uint32_t format; /* MUASSET_TEX_* */
} MuassetTexture;

#define MUASSET_TEX_RGBA8 0u

_Static_assert(sizeof(MuassetHeader) == 24, "MuassetHeader is 24");
_Static_assert(sizeof(MuassetSection) == 32, "MuassetSection is 32");
_Static_assert(sizeof(MuassetVertex) == 16, "MuassetVertex is 16");
_Static_assert(sizeof(MuassetMesh) == 32, "MuassetMesh is 32");
_Static_assert(sizeof(MuassetLod) == 16, "MuassetLod is 16");
_Static_assert(sizeof(MuassetMaterial) == 32, "MuassetMaterial is 32");
_Static_assert(sizeof(MuassetTexture) == 28, "MuassetTexture is 28");

/* A mapped blob plus its section table. The caller owns the mapping. */
typedef struct Muasset {
    const uint8_t          *data;
    uint32_t                size;
    const MuassetHeader    *header;
    const MuassetSection   *sections;

    const MuassetVertex    *vertices;
    uint32_t                vertex_count;
    const uint16_t         *indices;
    uint32_t                index_count;
    const MuassetMesh      *meshes;
    uint32_t                mesh_count;
    const MuassetLod       *lods;
    uint32_t                lod_count;
    const MuassetMaterial  *materials;
    uint32_t                material_count;
    const MuassetTexture   *textures;
    uint32_t                texture_count;

    uint32_t                name_count;
    const uint32_t         *name_offsets; /* name_count + 1 entries */
    const char             *name_bytes;
} Muasset;

uint32_t muasset_align_up(uint32_t v, uint32_t a);
uint32_t muasset_crc32(const uint8_t *data, uint32_t n);

/* Binds the section table and the fixed-layout sections. Checks magic, major
   version, total_size, that every offset lies inside the blob, and that no two
   sections overlap. Does not touch the section payloads, so it is safe to run
   before the blob is resident.
   On failure returns false and writes a one-line reason into err. */
bool muasset_open(const void *data, uint32_t size, Muasset *out, char *err, uint32_t err_cap);

/* Full validation: everything muasset_open checks, plus row_size against the
   known layout for each tag, zero rows where the section is mandatory, and
   crc32 per section. This is the tool's path, not the load path. */
bool muasset_validate(const void *data, uint32_t size, char *err, uint32_t err_cap);

/* Row count a section must have for this format version, or 0 if the tag is
   not known. */
uint32_t muasset_section_row_size(uint32_t tag);

/* Name i from the NAMES blob, or "" when the blob carries no names. */

/* Debug-only mesh name; "" when the blob has no NAMES section. */
static inline const char *muasset_name_at(const Muasset *m, uint32_t i) {
    return i < m->name_count && m->name_bytes ? m->name_bytes + m->name_offsets[i] : "";
}

#endif