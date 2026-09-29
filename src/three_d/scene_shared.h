#ifndef MU_GFX_SCENE_SHARED_H
#define MU_GFX_SCENE_SHARED_H
#ifdef __STDC__
#include <stdint.h>
typedef struct SceneVector { float x, y, z, w; } SceneVector;
#define SCENE_POINTER(type) uint64_t
#define SCENE_UINT uint32_t
#define SCENE_INT int32_t
#define SCENE_SKIN_PHASES 8
typedef struct ScenePackedVertex ScenePackedVertex;
typedef struct SceneSkinPalette { float m[16]; } SceneSkinPalette;
#else
#define SceneVector float4
#define SCENE_POINTER(type) type *
#define SCENE_UINT uint
#define SCENE_INT int
#endif

struct ScenePackedVertex {
    SCENE_UINT position_xy;
    SCENE_UINT position_z;
    SCENE_UINT normal_oct;
    SCENE_UINT uv;
};

#ifdef __STDC__
typedef struct SceneGpuInstance {
    uint32_t pos_xy;
    uint32_t pos_z__scale;
    uint32_t spare;
    uint32_t quat;
    uint32_t flags;
} SceneGpuInstance;

typedef struct SceneGpuMesh {
    SCENE_POINTER(ScenePackedVertex) vertex_stream;
    SCENE_POINTER(uint16_t) index_stream;
    uint32_t index_count;
    uint32_t pack;
    uint16_t first_cluster;
    uint16_t cluster_count;
    uint16_t flags;
    uint16_t lod_group;
    uint32_t local_center;
    uint32_t local_radius;
} SceneGpuMesh;

typedef struct SceneGpuMaterial {
    uint32_t base_color;
    uint32_t texture;
    uint32_t flags;
} SceneGpuMaterial;

typedef struct SceneGpuDraw {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    uint32_t vertex_offset;
    uint32_t first_instance;
    uint32_t draw_id;
    uint32_t pad[2];
} SceneGpuDraw;

typedef struct SceneMeshSet {
    uint32_t first;
    uint32_t count;
} SceneMeshSet;

typedef struct SceneLodRow {
    uint32_t index_count;
    uint32_t first_index;
    uint32_t vertex_offset;
    float    error;
} SceneLodRow;

typedef struct SceneSkinRecord {
    SCENE_POINTER(uint16_t) skin_stream; /* per-vertex: joints[4], weights[4], u16 */
    SCENE_POINTER(SceneSkinPalette) palette; /* skin palette base, mat4 array */
    uint32_t joint_count;
    uint32_t flags;
} SceneSkinRecord;

typedef struct SceneGpuView {
    float    rows[4][4];
    float    sun[4];
    SCENE_POINTER(SceneGpuDraw) draws;
    SCENE_POINTER(SCENE_UINT) visible;
    SCENE_POINTER(SceneGpuInstance) instances;
    SCENE_POINTER(SceneMeshSet) mesh_sets;
    SCENE_POINTER(SceneLodRow) lod_table;
    SCENE_POINTER(SCENE_UINT) command_count;
    SCENE_POINTER(SCENE_UINT) counters;
    SCENE_POINTER(SceneGpuMaterial) materials;
    uint32_t draw_count;
    uint32_t caps;
    uint32_t hiz_size[2];
    uint32_t sampler;
    float    lod_target;
    uint32_t pad;
} SceneGpuView;

typedef struct SceneViewPush {
    SCENE_POINTER(SceneGpuView) view;
    SCENE_POINTER(SceneGpuMesh) meshes;
    SCENE_POINTER(SceneSkinRecord) skin_table;
    uint32_t misc;
    uint32_t pad;
} SceneViewPush;
#else
struct SceneSkinPalette { float4x4 m; };
struct SceneGpuInstance {
    uint32_t pos_xy;
    uint32_t pos_z__scale;
    uint32_t spare;
    uint32_t quat;
    uint32_t flags;
};

struct SceneGpuMesh {
    SCENE_POINTER(ScenePackedVertex) vertex_stream;
    SCENE_POINTER(uint16_t) index_stream;
    uint32_t index_count;
    uint32_t pack;
    uint16_t first_cluster;
    uint16_t cluster_count;
    uint16_t flags;
    uint16_t lod_group;
    uint32_t local_center;
    uint32_t local_radius;
};

struct SceneGpuMaterial {
    uint32_t base_color;
    uint32_t texture;
    uint32_t flags;
};

struct SceneGpuDraw {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    uint32_t vertex_offset;
    uint32_t first_instance;
    uint32_t draw_id;
    uint32_t pad[2];
};

struct SceneMeshSet {
    uint32_t first;
    uint32_t count;
};

struct SceneLodRow {
    uint32_t index_count;
    uint32_t first_index;
    uint32_t vertex_offset;
    float    error;
};

struct SceneSkinRecord {
    SCENE_POINTER(uint16_t) skin_stream;
    SCENE_POINTER(SceneSkinPalette) palette;
    uint32_t joint_count;
    uint32_t flags;
};

struct SceneGpuView {
    float    rows[4][4];
    float    sun[4];
    SCENE_POINTER(SceneGpuDraw) draws;
    SCENE_POINTER(SCENE_UINT) visible;
    SCENE_POINTER(SceneGpuInstance) instances;
    SCENE_POINTER(SceneMeshSet) mesh_sets;
    SCENE_POINTER(SceneLodRow) lod_table;
    SCENE_POINTER(SCENE_UINT) command_count;
    SCENE_POINTER(SCENE_UINT) counters;
    SCENE_POINTER(SceneGpuMaterial) materials;
    uint32_t draw_count;
    uint32_t caps;
    uint32_t hiz_size[2];
    uint32_t sampler;
    float    lod_target;
    uint32_t pad;
};

struct SceneViewPush {
    SCENE_POINTER(SceneGpuView) view;
    SCENE_POINTER(SceneGpuMesh) meshes;
    SCENE_POINTER(SceneSkinRecord) skin_table;
    uint32_t misc;
    uint32_t pad;
};
#endif

#undef SCENE_POINTER
#undef SCENE_UINT
#undef SCENE_INT
#ifdef __STDC__
_Static_assert(sizeof(SceneGpuInstance) == 20, "GpuInstance must be 20 bytes");
_Static_assert(sizeof(SceneGpuMesh) == 40, "GpuMesh must be 40 bytes");
_Static_assert(sizeof(SceneGpuMaterial) == 12, "GpuMaterial must be 12 bytes");
_Static_assert(sizeof(SceneGpuDraw) == 32, "GpuDraw must be 32 bytes");
_Static_assert(sizeof(SceneGpuView) == 176, "GpuView must be 176 bytes");
_Static_assert(sizeof(SceneViewPush) == 32, "ViewPush must be 32 bytes");
_Static_assert(sizeof(SceneMeshSet) == 8, "MeshSet must be 8 bytes");
_Static_assert(sizeof(SceneLodRow) == 16, "LodRow must be 16 bytes");
_Static_assert(sizeof(SceneSkinRecord) == 24, "SkinRecord must be 24 bytes");
#endif
#endif
