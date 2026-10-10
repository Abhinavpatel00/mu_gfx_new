#ifndef MUASSET_COOK_H
#define MUASSET_COOK_H

/* muasset_cook: glTF -> .mua offline cooker. Host-only tool, never shipped.
 * One binary, flags instead of subcommands: it cooks, dumps and validates.
 *
 *   muasset_cook in.gltf -o out.mua [--lods N] [--bake-rest-pose] [--stats]
 *   muasset_cook --dump out.mua
 *   muasset_cook --validate out.mua
 *
 * Pipeline per primitive: bake the node transform into the vertices, build the
 * LOD ladder with meshoptimizer, then per rung weld -> cache -> overdraw ->
 * fetch-rewrite -> pack. The last step is what the vertex shader pays for, so
 * it is the one the cooker must not skip.
 *
 * Doctrine: fail loud. A skinned input without --bake-rest-pose, a primitive
 * that does not fit the u16 index arena, or a meshoptimizer step that cannot
 * reach its target is an error, never a quietly downgraded result. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MUACOOK_MAX_LODS 4u

typedef struct CookStats {
    uint32_t mesh_count;
    uint32_t node_count;   /* glTF nodes that carried geometry */
    uint32_t vertex_count; /* packed rows written, all rungs */
    uint32_t index_count;  /* u16 indices written, all rungs */
    uint32_t lod_count;    /* rungs per mesh, after padding to the model max */
    uint32_t source_verts; /* input vertices before welding */
    uint32_t batch_count;  /* distinct MUASSET_BATCH_* ids used */
    uint32_t texture_count; /* images baked to the companion payload */
    uint32_t baked_rigs;   /* primitives cooked from a skinned mesh */
    uint32_t vert_bytes;
    uint32_t idx_bytes;
    double   cook_ms;
} CookStats;

typedef struct CookOptions {
    const char *input_path;   /* .gltf or .glb */
    const char *output_path;  /* .mua; NULL with --dump/--validate */
    uint32_t    lod_count;    /* 1..MUACOOK_MAX_LODS; rungs are 100/50/25/12.5% */
    bool        bake_rest_pose; /* cook skinned input at its bind pose instead of failing */
    bool        stats;
} CookOptions;

bool muasset_cook(const CookOptions *opt, CookStats *stats_out, char *err, uint32_t err_cap);

#endif