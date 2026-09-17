#ifndef MU_GFX_SCENE3D_SHARED_H
#define MU_GFX_SCENE3D_SHARED_H
#ifdef __STDC__
#include <stdint.h>
typedef struct SceneVector { float x, y, z, w; } SceneVector;
#define SCENE_POINTER(type) uint64_t
#define SCENE_UINT uint32_t
#define SCENE_INT int32_t
typedef struct ScenePackedVertex ScenePackedVertex;
typedef struct SceneInstance SceneInstance;
typedef struct SceneCandidate SceneCandidate;
typedef struct SceneDraw SceneDraw;
typedef struct ScenePush ScenePush;
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
struct SceneInstance {
    SceneVector rows[3];
    SCENE_POINTER(ScenePackedVertex) vertices;
    SCENE_UINT material;
    SCENE_UINT tint;
};
struct SceneCandidate {
    SCENE_UINT instance;
    SCENE_UINT batch;
};
struct SceneDraw {
    SCENE_UINT index_count;
    SCENE_UINT instance_count;
    SCENE_UINT first_index;
    SCENE_INT vertex_offset;
    SCENE_UINT first_instance;
};
struct ScenePush {
    SCENE_POINTER(SceneInstance) instances;
    SCENE_POINTER(SceneVector) bounds;
    SCENE_POINTER(SceneVector) materials;
    SCENE_POINTER(SceneCandidate) candidates;
    SCENE_POINTER(SceneDraw) commands;
    SCENE_POINTER(SCENE_UINT) visible;
    SCENE_UINT instance_count;
    SCENE_UINT padding[3];
    SceneVector clip_rows[4];
    SceneVector sun;
};
#undef SCENE_POINTER
#undef SCENE_UINT
#undef SCENE_INT
#endif
