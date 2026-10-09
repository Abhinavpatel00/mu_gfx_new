/* scene_shared.h — the single authoritative C/Slang definition of every GPU
   row the GPU-driven 3D renderer uses. See docs/gpu-driven-dod-redesign.md.

   DOD: no row carries a byte any consumer branches on. Flags are table
   membership (render class), bounds are derived, dynamic is a dirty event.
   Every size is _Static_assert'ed on the C side; the Slang side must not
   disagree. */
#ifndef MU_GFX_SCENE_SHARED_H
#define MU_GFX_SCENE_SHARED_H

/* ---- counter slots (written by the cull kernel, read back lag-2) ---- */
#define SCENE_COUNTER_FRUSTUM 0
#define SCENE_COUNTER_HIZ     1
#define SCENE_COUNTER_DRAWN   2
#define SCENE_COUNTER_LOD0    3
#define SCENE_COUNTER_LOD1    4
#define SCENE_COUNTER_LOD2    5
#define SCENE_COUNTER_LOD3    6
#define SCENE_COUNTER_DROPPED 7
#define SCENE_COUNTER_DRAWS   8 /* non-empty groups == draw_count */
#define SCENE_COUNTERS        9

/* ---- render class ids. A class is fixed pipeline state; membership is a
   table, never a per-row flag. (Only OPAQUE is wired so far; adding a class is
   adding a candidate table + a cull entry + a pipeline.) ---- */
#define SCENE_CLASS_OPAQUE 0
#define SCENE_CLASS_COUNT  1

#ifdef __STDC__
#include <stdint.h>

typedef struct SceneVec4 {
    float x, y, z, w;
} SceneVec4;
#define SCENE_PTR(type) uint64_t
#define SCENE_U16       uint16_t
#define SCENE_I16       int16_t
#define SCENE_U32       uint32_t
#define SCENE_I32       int32_t
typedef struct SceneU32x2 {
    uint32_t lo, hi;
} SceneU32x2;
#define SCENE_U32X2 SceneU32x2
#else
/* Slang side: vec4 is a real float4 so dot()/swizzles work; pointers are real
   device-space pointers in the push constant. */
#define SceneVec4       float4
#define SCENE_PTR(type) type *
#define SCENE_U16       uint16_t
#define SCENE_I16       int16_t
#define SCENE_U32       uint
#define SCENE_I32       int
#define SCENE_U32X2     uint2
#define SceneU16        uint16_t
#define SceneI16        int16_t
#define SceneU32        uint
#define SceneI32        int
#endif

/* ---- 16-byte packed vertex: half3 pos, snorm16x2 oct normal, half2 uv ---- */
// do i really need thiss in soa ?
struct ScenePackedVertex {
    SCENE_U32 position_xy; /* half2: x low, y high */
    SCENE_U32 position_z;  /* half in low 16, spare high 16 */
    SCENE_U32 normal_oct;  /* snorm16x2 */
    SCENE_U32 uv;          /* half2 */
};

/* ---- 20-byte instance: world f32 position, quat, uniform half scale, bias.
   Field order is cull-read order. No stored bounds, no flags. ---- */
struct SceneInstance {
    float     pos[3];   /* 12 world position */
    SCENE_U32 quat;     /*  4 10-10-10-2 normalized quaternion */
    SCENE_U16 scale;    /*  2 half uniform scale */
    SCENE_I16 lod_bias; /*  2 signed LOD bias */
};

/* ---- 32-byte cull-only mesh row. The local bounding sphere is a packed
   half4 (centre xyz + radius) so the cull derives the world sphere exactly. ---- */
struct SceneCullMesh {
    SCENE_U16 lod_first;       /* 2 base into SceneLodRow[] */
    SCENE_U16 lod_count;       /* 2 rungs */
    SCENE_U32 local_sphere_xy; /* 4 half2: centre x, y */
    SCENE_U32 local_sphere_zr; /* 4 half2: centre z, radius */
    SCENE_U32 cluster_first;   /* 4 reserved (clusters) */
    SCENE_U32 cluster_count;   /* 4 reserved */
    SCENE_U32 pad[3];          /* 12 */
};

/* ---- 16-byte shade-only mesh row (VS reads it, broadcast per draw) ---- */
struct SceneShadeMesh {
    SCENE_PTR(ScenePackedVertex) vertex_stream; /* 8 device address */
    SCENE_U32 base_vertex;                      /* 4 reserved (address-baked streams) */
    SCENE_U32 material;                         /* 4 SceneGpuMaterial index */
};

/* ---- 12-byte LOD rung ---- */
struct SceneLodRow {
    float     error;       /* screen-space error threshold */
    SCENE_U32 first_index; /* into the class index arena (elements) */
    SCENE_U32 index_count;
};

/* ---- 8-byte candidate row: one (instance, mesh) pair in one class ---- */
struct SceneCullRow {
    SCENE_U32 slot; /* instance slot */
    SCENE_U16 mesh; /* SceneCullMesh index (load index, never branched on) */
    SCENE_U16 pad;
};

/* ---- 32-byte compacted indexed-indirect command ----- */
struct SceneGpuDraw {
    SCENE_U32 index_count;    /* static per (mesh,lod) */
    SCENE_U32 instance_count; /* T3: group survivor count */
    SCENE_U32 first_index;    /* static: SceneLodRow.first_index */
    SCENE_I32 vertex_offset;  /* static: 0 (vertex_stream is baked per mesh) */
    SCENE_U32 first_instance; /* T3: vis base for the group */
    SCENE_U32 mesh_lod;       /* mesh:16 | lod:8 | class:8 (lod/class zero for now) */
    SCENE_U32 pad[2];
};

/* ---- 12-byte material ---- */
struct SceneGpuMaterial {
    SCENE_U32 base_color; /* UNORM8x4 */
    SCENE_U32 texture;    /* bindless albedo id, 0xFFFFFFFF = none */
    SCENE_U32 flags;      /* sampler:2 | ... */
};

/* ---- push constant, once per cull dispatch and once per draw (224 bytes).
   Shared by the cull kernel and the VS/FS so the pipeline layout matches. ---- */
struct ScenePush {
    SceneVec4 clip_rows[4]; /* 64 */
    SceneVec4 sun;          /* 16 xyz direction, w ambient */
    SceneVec4 viewparams;   /* 16 xyz camera position, w lod target */
    /* spare, reserved */

    SCENE_PTR(SceneInstance) instances;
    SCENE_PTR(SceneCullMesh) cull_meshes;
    SCENE_PTR(SceneCullRow) cull_rows; /* the batch's candidates */
    SCENE_PTR(SceneShadeMesh) shade_meshes;
    SCENE_PTR(SceneLodRow) lod_rows;
    SCENE_PTR(SceneGpuMaterial) materials;
    SCENE_PTR(SCENE_U32) survivors;
    SCENE_PTR(SCENE_U32) survivor_count;
    SCENE_PTR(SCENE_U32) vis;
    SCENE_PTR(SceneGpuDraw) draws;
    /* group_base[] is indexed by the draw's instance base (firstInstance) and
       yields the (mesh,lod) group id; group_mesh[group] then yields the mesh id.
       Both are uniform across a draw, so the VS broadcasts both instead of
       gathering a per-lane value. group_base is sized max_survivors because
       firstInstance indexes vis_all[]. */
    SCENE_PTR(SCENE_U32) group_base;
    SCENE_PTR(SCENE_U32) group_mesh;
    SCENE_PTR(SCENE_U32) counters;
    /* VkDispatchIndirectCommand triple for the cull. CPU-written on roster
       change; the kernel raises x to its true high-water mark so a stale CPU
       value can never under-dispatch the next frame. */
    SCENE_PTR(SCENE_U32) cull_args;

    SCENE_U32 counts[8]; /* 0 lod_count, 1 candidate_count, 2 max_survivors */
};

/* ---- compaction context (histogram / scan / emit / scatter) ----
   scan_aux is one slice carved into three regions of SCENE_SCAN_BLOCK entries:
     [0*BLOCK)  per-block totals (count, emit), written by cs_scan_block
     [1*BLOCK)  block offset for vis_base
     [2*BLOCK)  block offset for the command index                        */
#define SCENE_SCAN_THREADS 256u
#define SCENE_SCAN_BLOCK   (SCENE_SCAN_THREADS * 4u)
struct SceneCompactPush {
    SCENE_PTR(SCENE_U32) survivors;
    SCENE_PTR(SCENE_U32) survivor_count;
    SCENE_PTR(SCENE_U32) group_count;
    SCENE_PTR(SCENE_U32) vis_base;
    SCENE_PTR(SCENE_U32) cmd_index;
    SCENE_PTR(SCENE_U32) cursor;
    SCENE_PTR(SCENE_U32) vis;
    SCENE_PTR(SceneGpuDraw) draws;
    SCENE_PTR(SceneGpuDraw) group_static;
    SCENE_PTR(SCENE_U32) draw_count;
    SCENE_PTR(SCENE_U32X2) scan_aux;
    SCENE_PTR(SCENE_U32) counters;
    SCENE_PTR(SCENE_U32) group_base; /* vis_base -> group id, for the VS */
    SCENE_U32 counts[8];             /* 0 lod_count, 1 group_count G, 2 max_survivors, 3 reserved, 4 blocks */
};

#undef SCENE_PTR

#ifdef __STDC__

/* ---- layout contract: a lie here is a shader read of the wrong bytes ---- */
_Static_assert(sizeof(struct ScenePackedVertex) == 16, "ScenePackedVertex is 16 bytes");
_Static_assert(sizeof(struct SceneInstance) == 20, "SceneInstance is 20 bytes");
_Static_assert(sizeof(struct SceneCullMesh) == 32, "SceneCullMesh is 32 bytes");
_Static_assert(sizeof(struct SceneShadeMesh) == 16, "SceneShadeMesh is 16 bytes");
_Static_assert(sizeof(struct SceneLodRow) == 12, "SceneLodRow is 12 bytes");
_Static_assert(sizeof(struct SceneCullRow) == 8, "SceneCullRow is 8 bytes");
_Static_assert(sizeof(struct SceneGpuDraw) == 32, "SceneGpuDraw is 32 bytes");
_Static_assert(sizeof(struct SceneGpuMaterial) == 12, "SceneGpuMaterial is 12 bytes");
_Static_assert(sizeof(struct ScenePush) <= 256, "ScenePush exceeds the 256-byte push range");
_Static_assert(sizeof(struct SceneCompactPush) <= 256, "SceneCompactPush exceeds the push range");

#endif /* __STDC__ */
#endif /* MU_GFX_SCENE_SHARED_H */
