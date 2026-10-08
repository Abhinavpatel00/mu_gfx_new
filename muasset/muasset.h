#ifndef MUASSET_H
#define MUASSET_H

/* `.mua`: cooked GPU-ready asset blob. Spec: docs/muasset-format.md.
   Field order inside every row matches the shader's read order
   (gpu-driven-dod-redesign.md); the loader never swizzles vertex mass.
   Naming per docs/mycstyle.md: PascalCase types, snake_case fns/fields. */

#include <stdint.h>

#define MUASSET_MAGIC  0x2E41554DU /* ".MUA" little-endian */
#define MUASSET_MAJOR  1u
#define MUASSET_MINOR  0u

#define MUASEC_VERT_ARENA   1u
#define MUASEC_IDX_ARENA    2u
#define MUASEC_CULLMESH     3u
#define MUASEC_SHADEMESH    4u
#define MUASEC_LOD          5u
#define MUASEC_CLUSTER      6u
#define MUASEC_SKIN         7u
#define MUASEC_MATERIAL     8u
#define MUASEC_BATCH_ROUTE  9u
#define MUASEC_ANIM        10u
#define MUASEC_NAMES       11u

#define MUASSET_BATCH_OPAQUE      0u
#define MUASSET_BATCH_DOUBLESIDE  1u
#define MUASSET_BATCH_BLEND       2u
#define MUASSET_BATCH_SKINNED     3u
#define MUASSET_BATCH_COUNT       4u

#define MUASSET_ALIGN  16u

typedef struct MuassetHeader {
    uint32_t  magic;
    uint32_t  version;
    uint32_t  section_count;
    uint32_t  total_size;
    uint8_t   content_hash[32];
    uint32_t  flags;
} MuassetHeader;

typedef struct MuassetSection {
    uint32_t  tag;
    uint32_t  row_size;
    uint32_t  row_count;
    uint32_t  offset;
    uint32_t  size;
    uint32_t  checksum;
} MuassetSection;

typedef struct MuassetVertStatic {
    float     pos[3];
    float     normal[3];
    float     uv[2];
} MuassetVertStatic;

typedef struct MuassetVertSkin {
    float     pos[3];
    float     normal[3];
    float     uv[2];
    uint8_t   joints[4];
    float     weights[4];
} MuassetVertSkin;

typedef struct MuassetCullMesh {
    uint16_t  first_cluster;
    uint16_t  cluster_count;
    uint16_t  flags;
    uint16_t  lod_group;
    uint16_t  local_center[4];
    uint16_t  local_radius[2];
    uint32_t  index_count;
    uint32_t  pad;
} MuassetCullMesh;

typedef struct MuassetShadeMesh {
    uint64_t  vert_off;
    uint64_t  idx_off;
} MuassetShadeMesh;

typedef struct MuassetLod {
    uint32_t  first_index;
    uint32_t  index_count;
    uint32_t  first_vertex;
    float     error;
} MuassetLod;

typedef struct MuassetCluster {
    float     sphere[4];
    float     cone_axis[3];
    float     cone_cutoff;
    uint32_t  first_index;
    uint32_t  index_count;
    uint32_t  pad[2];
} MuassetCluster;

typedef struct MuassetSkin {
    float     inv_bind[12];
    uint16_t  joint_node;
    uint16_t  pad;
} MuassetSkin;

typedef struct MuassetMaterial {
    float     base_color[4];
    float     metallic;
    float     roughness;
    uint32_t  albedo_uri;
    uint32_t  sampler;
} MuassetMaterial;

typedef struct MuassetAnimKey {
    float     time;
    float     t[3];
    float     r[4];
    float     s[3];
} MuassetAnimKey;

_Static_assert(sizeof(MuassetHeader) == 48, "MuassetHeader != 48");
_Static_assert(sizeof(MuassetSection) == 24, "MuassetSection != 24");
_Static_assert(sizeof(MuassetVertStatic) == 32, "MuassetVertStatic != 32");
_Static_assert(sizeof(MuassetVertSkin) == 48, "MuassetVertSkin != 48");
_Static_assert(sizeof(MuassetCullMesh) == 24, "MuassetCullMesh != 24");
_Static_assert(sizeof(MuassetShadeMesh) == 16, "MuassetShadeMesh != 16");
_Static_assert(sizeof(MuassetLod) == 16, "MuassetLod != 16");
_Static_assert(sizeof(MuassetCluster) == 48, "MuassetCluster != 48");
_Static_assert(sizeof(MuassetSkin) == 56, "MuassetSkin != 56");
_Static_assert(sizeof(MuassetMaterial) == 32, "MuassetMaterial != 32");
_Static_assert(sizeof(MuassetAnimKey) == 44, "MuassetAnimKey != 44");

uint32_t  muasset_align_up(uint32_t v, uint32_t a);
uint32_t  muasset_crc32(const uint8_t *data, uint32_t n);
const MuassetSection *muasset_find(const MuassetHeader *h, uint32_t tag);

#endif
