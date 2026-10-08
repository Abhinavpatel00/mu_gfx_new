#ifndef MUASSET_COOK_H
#define MUASSET_COOK_H

/* muasset_cook: glTF -> .mua offline cooker. Host-only tool, never ships.
   Classic indexed-indirect path: no mesh shaders, no meshlets-as-draws.
   Clusters are contiguous index ranges over a reordered IDX arena; the GPU
   cull compacts per-cluster draws into the batch indirect buffer.
   Doctrine: fail loud on bad assets, never a silent downgrade. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MUACOOK_MAX_JOINTS    128u
#define MUACOOK_MAX_LODS        4u
#define MUACOOK_MAX_CLUSTERS  256u

typedef struct CookStats {
    uint32_t mesh_count;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t cluster_count;
    uint32_t lod_rows;
    uint32_t skin_rows;
    uint32_t anim_keys;
    uint32_t batch_route[8];
    bool     normals_computed;
    double   cook_ms;
} CookStats;

typedef struct CookOptions {
    const char *input_path;   /* .gltf or .glb */
    const char *output_path;  /* .mua */
    const char *sidecar_path; /* .mua.json, NULL = <output>.json */
    int         lod_count;    /* 1..4, default 3 */
    bool        clusters;     /* build CLUSTER section, default true */
    bool        verbose;
} CookOptions;

bool muasset_cook(const CookOptions *opt, CookStats *stats_out,
                  char *err, size_t err_cap);

#endif
