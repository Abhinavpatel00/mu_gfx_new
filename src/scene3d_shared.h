#ifndef MU_GFX_SCENE3D_SHARED_H
#define MU_GFX_SCENE3D_SHARED_H
#ifdef __STDC__
#include <stdint.h>
typedef struct SceneVector { float x, y, z, w; } SceneVector;
#define SCENE_POINTER(type) uint64_t
#define SCENE_UINT uint32_t
#define SCENE_INT int32_t
typedef struct ScenePackedVertex ScenePackedVertex;
typedef struct SceneSkinPackedVertex SceneSkinPackedVertex;
typedef struct SceneSkeletonJoint SceneSkeletonJoint;
typedef struct SceneSkeletonSkin SceneSkeletonSkin;
typedef struct SceneSampler SceneSampler;
typedef struct SceneChannel SceneChannel;
typedef struct SceneClip SceneClip;
typedef struct SceneNode SceneNode;
typedef struct SceneGpuMaterial SceneGpuMaterial;
typedef struct SceneInstance SceneInstance;
typedef struct SceneCandidate SceneCandidate;
typedef struct SceneDraw SceneDraw;
typedef struct ScenePush ScenePush;
typedef struct Frustum Frustum;
typedef enum FrustumPlane {
    TopPlane = 0,
    BottomPlane,
    LeftPlane,
    RightPlane,
    NearPlane,
    FarPlane,
    FrustumPlaneCount
} FrustumPlane;
#else
/* Slang sees SceneVector as a real float4 so dot()/swizzles work; C keeps the
   plain struct. Field layout is identical (16 bytes, 16-aligned). */
#define SceneVector float4
#define SCENE_POINTER(type) type *
#define SCENE_UINT uint
#define SCENE_INT int
static const uint TopPlane          = 0;
static const uint BottomPlane       = 1;
static const uint LeftPlane         = 2;
static const uint RightPlane        = 3;
static const uint NearPlane         = 4;
static const uint FarPlane          = 5;
static const uint FrustumPlaneCount = 6;
#endif

struct ScenePackedVertex {
    SCENE_UINT position_xy;  /* half2 */
    SCENE_UINT position_z;   /* half in low 16, tangent xyznorm in high 16 */
    SCENE_UINT normal_oct;   /* snorm16x2 */
    SCENE_UINT uv;           /* half2 */
};

/* Skinning input: packed vertex + 4 joint indices + 4 unorm16 weights. */
struct SceneSkinPackedVertex {
    SCENE_UINT position_xy;
    SCENE_UINT position_z;
    SCENE_UINT normal_oct;
    SCENE_UINT uv;
    SCENE_UINT joints01;
    SCENE_UINT joints23;
    SCENE_UINT weights01;
    SCENE_UINT weights23;
};

struct SceneSkeletonJoint {
    SCENE_UINT node; /* node this joint mirrors */
    SCENE_UINT pad[3];
};

struct SceneSkeletonSkin {
    SCENE_UINT joint_offset;
    SCENE_UINT joint_count;
};

struct SceneSampler {
    SCENE_UINT input_offset;  /* index into SceneCpuData.sampler_times */
    SCENE_UINT output_offset; /* index into SceneCpuData.sampler_values */
    SCENE_UINT input_count;
    SCENE_UINT output_count;
    SCENE_UINT components;
    SCENE_UINT interpolation;
};

struct SceneChannel {
    SCENE_UINT sampler;
    SCENE_UINT node;
    SCENE_UINT path; /* 0 translation, 1 rotation, 2 scale */
};

struct SceneClip {
    SCENE_UINT sampler_offset;
    SCENE_UINT sampler_count;
    SCENE_UINT channel_offset;
    SCENE_UINT channel_count;
    float       duration;
    SCENE_UINT pad[3];
};

struct SceneNode {
    float       translation[3];
    float       scale[3];
    float       rotation[4]; /* xyzw */
    SCENE_INT   parent;
    SCENE_UINT  pad[3];
};

/* CPU-only narrative: the C side owns these, the shaders never see them. */
#ifdef __STDC__
typedef struct SceneNodeData SceneNodeData;
#endif

struct SceneInstance {
    SceneVector rows[3];
    SceneVector bounds;  /* xyz world center, w radius */
    SCENE_UINT  tint;
    SCENE_UINT  pad[3];
};
struct SceneCandidate {
    SCENE_UINT instance;
    SCENE_UINT batch;
};
struct SceneDraw {
    SCENE_UINT index_count;
    SCENE_UINT instance_count; /* GPU-written by the cull kernel */
    SCENE_UINT first_index;
    SCENE_INT  vertex_offset;
    SCENE_UINT first_instance; /* base into the visible slot array */
    SCENE_UINT pad[3];
};
/* ax + by + cz + d = 0, each plane vec is (a, b, c, d) normalized. */
struct Frustum {
    SceneVector planes[6];
};
/* Padded so the array stride matches the C struct exactly (112 bytes). */
struct SceneGpuMaterial {
    SceneVector base_color;
    SCENE_UINT albedo_texture;
    SCENE_UINT sampler;
    SCENE_UINT flags;
    SCENE_UINT pad;
};
/* Vector data first so every float4 sits at a 16-byte offset under both the C
   rules (align 4) and Slang cbuffer rules (align 16). 248 bytes. */
struct ScenePush {
    SceneVector clip_rows[4];
    SceneVector sun;      /* xyz direction, w ambient floor */
    SceneVector material; /* rgb base color, a = albedo TextureID (-1 = none) */
    Frustum frustum;
    SCENE_UINT instance_count;
    SCENE_UINT sampler; /* bindless sampler id for the albedo */
    SCENE_UINT padding[2];
    SCENE_POINTER(SceneInstance) instances;
    SCENE_POINTER(SceneCandidate) candidates;
    SCENE_POINTER(SceneDraw) commands;
    SCENE_POINTER(SCENE_UINT) visible;
    SCENE_POINTER(ScenePackedVertex) vertex_stream;
};
#undef SCENE_POINTER
#undef SCENE_UINT
#undef SCENE_INT
#ifdef __STDC__
#include <math.h>
/* Build normalized world-space frustum planes from clip rows once per frame
   on the CPU. Shader then does 6 dot products, no per-thread length(). */
static inline void frustum_extract(Frustum *out, const SceneVector rows[4]) {
    SceneVector combo[6] = {
        {rows[3].x + rows[0].x, rows[3].y + rows[0].y, rows[3].z + rows[0].z, rows[3].w + rows[0].w},
        {rows[3].x - rows[0].x, rows[3].y - rows[0].y, rows[3].z - rows[0].z, rows[3].w - rows[0].w},
        {rows[3].x + rows[1].x, rows[3].y + rows[1].y, rows[3].z + rows[1].z, rows[3].w + rows[1].w},
        {rows[3].x - rows[1].x, rows[3].y - rows[1].y, rows[3].z - rows[1].z, rows[3].w - rows[1].w},
        {rows[3].x + rows[2].x, rows[3].y + rows[2].y, rows[3].z + rows[2].z, rows[3].w + rows[2].w},
        {rows[3].x - rows[2].x, rows[3].y - rows[2].y, rows[3].z - rows[2].z, rows[3].w - rows[2].w},
    };
    /* Order: left, right, bottom, top, near, far. */
    const FrustumPlane order[6] = {LeftPlane, RightPlane, BottomPlane, TopPlane, NearPlane, FarPlane};
    for (int i = 0; i < 6; i++) {
        float len = sqrtf(combo[i].x * combo[i].x + combo[i].y * combo[i].y + combo[i].z * combo[i].z);
        float inv = len > 1e-8f ? 1.0f / len : 0.0f;
        out->planes[order[i]].x = combo[i].x * inv;
        out->planes[order[i]].y = combo[i].y * inv;
        out->planes[order[i]].z = combo[i].z * inv;
        out->planes[order[i]].w = combo[i].w * inv;
    }
}
_Static_assert(sizeof(ScenePush) <= 256, "ScenePush must fit the 256-byte push-constant range");
#endif
#endif
