#ifndef MU_GFX_SCENE3D_SHARED_H
#define MU_GFX_SCENE3D_SHARED_H

#ifdef __STDC__
#include <stdint.h>
typedef struct SceneVector {
    float x, y, z, w;
} SceneVector;
#define SCENE_POINTER(type) uint64_t
#define SCENE_UINT uint32_t
#define SCENE_INT int32_t
typedef struct SceneInstance SceneInstance;
typedef struct SceneDraw SceneDraw;
typedef struct ScenePush ScenePush;
#else
#define SceneVector float4
#define SCENE_POINTER(type) type *
#define SCENE_UINT uint
#define SCENE_INT int
#endif

struct SceneInstance {
    SceneVector center_scale;
    SceneVector color;
};

struct SceneDraw {
    SCENE_UINT index_count;
    SCENE_UINT instance_count;
    SCENE_UINT first_index;
    SCENE_INT vertex_offset;
    SCENE_UINT first_instance;
};

struct ScenePush {
    SCENE_POINTER(SceneVector) vertices;
    SCENE_POINTER(SceneInstance) instances;
    SCENE_POINTER(SceneDraw) commands;
    SCENE_UINT instance_count;
    SCENE_UINT pad;
    SceneVector clip_rows[4];
};

#undef SCENE_POINTER
#undef SCENE_UINT
#undef SCENE_INT
#endif
