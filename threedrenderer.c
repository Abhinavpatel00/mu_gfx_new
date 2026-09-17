#include "external/cglm/include/cglm/mat4.h"








#include "external/cglm/include/cglm/types.h"
#include "external/debugbreak/debugbreak.h"
#include "mu/mu/mu_sparse_set.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define mu_malloc(size) malloc(size)
#define mu_calloc(count, size) calloc((count), (size))

#define mu_free(ptr)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        free(ptr);                                                                                                     \
        (ptr) = NULL;                                                                                                  \
    } while(0)

#include "mu/mu/mu_array.h"
#include "mu/mu/mu_bulk_storage.h"
#include "mu/mu/mu_hash_table.h"
#include "renderer.h"
#include "passes.h"
#include "tinytypes.h"
#include "vk.h"

static uint64_t mu_fnv1a64_local(const char* s)
{
    uint64_t h = 14695981039346656037ULL;
    for(const unsigned char* p = (const unsigned char*)s; *p; ++p)
        h = (h ^ (uint64_t)(*p)) * 1099511628211ULL;
    return h;
}

static char* mu_strdup_local(const char* s)
{
    if(!s)
        s = "";

    size_t len = strlen(s) + 1;
    char*  out = mu_malloc(len);
    if(out)
        memcpy(out, s, len);
    return out;
}

static char* asset_make_relative_path(const char* asset_path, const char* uri)
{
    if(!uri || !uri[0])
        return NULL;

    if(strstr(uri, "://") || uri[0] == '/' || strncmp(uri, "data:", 5) == 0)
        return mu_strdup_local(uri);

    const char* slash = strrchr(asset_path, '/');
    if(!slash)
        return mu_strdup_local(uri);

    size_t dir_len = (size_t)(slash - asset_path) + 1;
    size_t uri_len = strlen(uri);
    char*  out     = mu_malloc(dir_len + uri_len + 1);
    if(!out)
        return NULL;

    memcpy(out, asset_path, dir_len);
    memcpy(out + dir_len, uri, uri_len + 1);
    cgltf_decode_uri(out + dir_len);
    return out;
}

static TextureID asset_upload_rgba_texture(Renderer* renderer, const unsigned char* pixels, uint32_t width, uint32_t height)
{
    if(!renderer || !pixels || width == 0 || height == 0)
        return UINT32_MAX;

    TextureCreateDesc desc = {
        .width     = width,
        .height    = height,
        .mip_count = 1,
        .format    = VK_FORMAT_R8G8B8A8_SRGB,
        .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };

    TextureID id = create_texture(renderer, &desc);
    if(id == UINT32_MAX)
        return UINT32_MAX;

    Texture*     tex        = &textures[id];
    VkDeviceSize image_size = (VkDeviceSize)width * (VkDeviceSize)height * 4;
    VkCommandBuffer cmd = vk_begin_one_time_cmd(renderer->device, renderer->one_time_gfx_pool);
    if(!cmd)
    {
        destroy_texture(renderer, id);
        return UINT32_MAX;
    }

    VkImageMemoryBarrier barrier = {
        .sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcAccessMask               = 0,
        .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
        .image                       = tex->image,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    if(!renderer_upload_texture_2d(renderer, cmd, tex, pixels, image_size, width, height, 0))
    {
        vk_end_one_time_cmd(renderer->device, renderer->graphics_queue, renderer->one_time_gfx_pool, cmd);
        destroy_texture(renderer, id);
        return UINT32_MAX;
    }

    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    vk_end_one_time_cmd(renderer->device, renderer->graphics_queue, renderer->one_time_gfx_pool, cmd);
    return id;
}

static TextureID asset_load_gltf_image(Renderer* renderer, const char* asset_path, const cgltf_image* image)
{
    if(!renderer || !image)
        return UINT32_MAX;

    if(image->buffer_view)
    {
        const uint8_t* bytes = cgltf_buffer_view_data(image->buffer_view);
        if(!bytes || image->buffer_view->size == 0)
            return UINT32_MAX;

        int w = 0, h = 0, c = 0;
        unsigned char* pixels = stbi_load_from_memory(bytes, (int)image->buffer_view->size, &w, &h, &c, 4);
        if(!pixels)
            return UINT32_MAX;

        TextureID id = asset_upload_rgba_texture(renderer, pixels, (uint32_t)w, (uint32_t)h);
        stbi_image_free(pixels);
        return id;
    }

    if(image->uri)
    {
        char* path = asset_make_relative_path(asset_path, image->uri);
        if(!path)
            return UINT32_MAX;

        TextureID id = load_texture(renderer, path);
        mu_free(path);
        return id;
    }

    return UINT32_MAX;
}

static const char* GLTF_MODEL_PATHS[] = {
    "assets/cubepets/Models/GLB format/animal-beaver.glb",
    "assets/cubepets/Models/GLB format/animal-bee.glb",
    "assets/cubepets/Models/GLB format/animal-bunny.glb",
    "assets/cubepets/Models/GLB format/animal-cat.glb",
};
#define GLTF_MODEL_COUNT (sizeof(GLTF_MODEL_PATHS) / sizeof(GLTF_MODEL_PATHS[0]))
#define DEFAULT_MODEL_PATH "assets/3DGodotRobot.glb"
/* ============================================================================
   Packed Vertex Formats
   ============================================================================ */

typedef struct PackedVertex
{
    uint16_t vx, vy, vz;
    uint16_t tp;
    uint32_t np;
    uint16_t tu, tv;
} PackedVertex; /* 16B */

typedef struct PackedSkinVertex
{
    uint16_t vx, vy, vz;
    uint16_t tp;
    uint32_t np;
    uint16_t tu, tv;
    uint16_t joints[4];
    uint16_t weights[4];
} PackedSkinVertex; /* 32B */

_Static_assert(sizeof(PackedVertex) == 16, "PackedVertex shader layout mismatch");
_Static_assert(sizeof(PackedSkinVertex) == 32, "PackedSkinVertex shader layout mismatch");

MU_INLINE PackedVertex mu_pack_vertex(float px, float py, float pz, float nx, float ny, float nz, float tx, float ty, float u, float v, int bitangent_sign)
{
    PackedVertex pv;

    pv.vx = mu_quantize_half(px);
    pv.vy = mu_quantize_half(py);
    pv.vz = mu_quantize_half(pz);

    int qnx = mu_quantize_snorm(nx, 10);
    int qny = mu_quantize_snorm(ny, 10);
    int qnz = mu_quantize_snorm(nz, 10);

    pv.np = ((uint32_t)(qnx & 1023)) | ((uint32_t)(qny & 1023) << 10) | ((uint32_t)(qnz & 1023) << 20)
            | ((uint32_t)(bitangent_sign & 3) << 30);

    int qtx = mu_quantize_snorm(tx, 8);
    int qty = mu_quantize_snorm(ty, 8);

    pv.tp = ((uint16_t)(qtx & 255) << 8) | ((uint16_t)(qty & 255));

    pv.tu = mu_quantize_half(u);
    pv.tv = mu_quantize_half(v);

    return pv;
}


MU_INLINE PackedSkinVertex mu_pack_skin_vertex(float    px,
                                               float    py,
                                               float    pz,
                                               float    nx,
                                               float    ny,
                                               float    nz,
                                               float    tx,
                                               float    ty,
                                               float    u,
                                               float    v,
                                               uint16_t j0,
                                               uint16_t j1,
                                               uint16_t j2,
                                               uint16_t j3,
                                               uint16_t w0,
                                               uint16_t w1,
                                               uint16_t w2,
                                               uint16_t w3,
                                               int      bitangent_sign)
{
    PackedSkinVertex pv;

    pv.vx = mu_quantize_half(px);
    pv.vy = mu_quantize_half(py);
    pv.vz = mu_quantize_half(pz);

    int qnx = mu_quantize_snorm(nx, 10);
    int qny = mu_quantize_snorm(ny, 10);
    int qnz = mu_quantize_snorm(nz, 10);

    pv.np = ((uint32_t)(qnx & 1023)) | ((uint32_t)(qny & 1023) << 10) | ((uint32_t)(qnz & 1023) << 20)
            | ((uint32_t)(bitangent_sign & 3) << 30);

    int qtx = mu_quantize_snorm(tx, 8);
    int qty = mu_quantize_snorm(ty, 8);

    pv.tp = ((uint16_t)(qtx & 255) << 8) | ((uint16_t)(qty & 255));
    pv.tu = mu_quantize_half(u);
    pv.tv = mu_quantize_half(v);

    pv.joints[0] = j0;
    pv.joints[1] = j1;
    pv.joints[2] = j2;
    pv.joints[3] = j3;

    pv.weights[0] = w0;
    pv.weights[1] = w1;
    pv.weights[2] = w2;
    pv.weights[3] = w3;

    return pv;
}

/* ============================================================================
   Handles
   ============================================================================ */

typedef mu_weak_handle ModelAssetHandle;
typedef mu_weak_handle RenderInstanceHandle;
typedef mu_weak_handle AnimationClipHandle;
typedef mu_weak_handle SkeletonHandle;

/* ============================================================================
   Runtime Flags
   ============================================================================ */

typedef enum MaterialFlags
{
    MATERIAL_FLAG_NONE           = 0,
    MATERIAL_FLAG_ALPHA_MASKED   = 1 << 0,
    MATERIAL_FLAG_DOUBLE_SIDED   = 1 << 1,
    MATERIAL_FLAG_UNLIT          = 1 << 2,
    MATERIAL_FLAG_RECEIVE_FOG    = 1 << 3,
    MATERIAL_FLAG_EMISSIVE_BLOOM = 1 << 4,
} MaterialFlags;

typedef enum RenderFlags
{
    RENDER_FLAG_NONE    = 0,
    RENDER_FLAG_VISIBLE = 1 << 0,
    RENDER_FLAG_DYNAMIC = 1 << 1,
    RENDER_FLAG_STATIC  = 1 << 2,
    RENDER_FLAG_SKINNED = 1 << 3,
    RENDER_FLAG_OUTLINE = 1 << 4,
} RenderFlags;


/* ============================================================================
   GPU Draw Records
   ============================================================================ */

typedef struct ALIGNAS(16) GpuDraw
{
    uint32_t index_count;
    uint32_t first_index;
    uint32_t vertex_offset;
    uint32_t material_index;

    uint32_t vertex_stream;
    uint32_t _pad[3];
} GpuDraw;

/* ============================================================================
   Asset Layer (long-lived, immutable after load)
   ============================================================================ */

typedef struct MeshLod
{
    uint32_t index_offset;
    uint32_t index_count;
    float    error;
    uint32_t _pad;
} MeshLod;

typedef struct ALIGNAS(16) Mesh
{
    vec3  center;
    float radius;

    uint32_t vertex_offset;
    uint32_t vertex_count;

    uint32_t lod_count;
    uint32_t padding;

    MeshLod lods[8];
} Mesh;

typedef struct ALIGNAS(16) Material
{
    uint32_t albedo_texture;
    uint32_t normal_texture;
    uint32_t specular_texture;
    uint32_t emissive_texture;

    vec4 base_color_factor;
    vec4 specular_factor;
    vec4 emissive_factor;

    uint32_t flags;
    uint32_t _pad[3];
} Material;

typedef struct SkeletonJoint
{
    uint32_t node_index;
    int32_t  parent_index;
    uint32_t skin_index;
    uint32_t _pad;
    mat4     local_bind;
    mat4     inverse_bind;
} SkeletonJoint;

typedef struct SkeletonSkin
{
    uint32_t joint_offset;
    uint32_t joint_count;
} SkeletonSkin;

/*
    Imported asset truth.
    What the file IS.
*/
typedef struct SkeletonAsset
{
    SkeletonSkin*  skins;
    uint32_t       skin_count;
    SkeletonJoint* joints;
    uint32_t       joint_count;
} SkeletonAsset;

typedef struct AnimationSampler
{
    float*   input_times;
    float*   output_values;
    uint32_t input_count;
    uint32_t output_count;
    uint32_t output_components;
    uint32_t interpolation;
} AnimationSampler;

typedef struct AnimationChannel
{
    uint32_t sampler_index;
    uint32_t target_node_index;
    uint32_t target_path;
} AnimationChannel;

typedef struct AnimationClip
{
    char*    name;
    uint32_t sampler_offset;
    uint32_t sampler_count;
    uint32_t channel_offset;
    uint32_t channel_count;
    float    duration;
} AnimationClip;

typedef struct AssetNode
{
    int32_t parent_index;
    vec3    translation;
    versor  rotation;
    vec3    scale;
    mat4    local;
    mat4    global;
} AssetNode;

typedef struct AnimationAsset
{
    AnimationClip*    clips;
    uint32_t          clip_count;
    AnimationSampler* samplers;
    uint32_t          sampler_count;
    AnimationChannel* channels;
    uint32_t          channel_count;
} AnimationAsset;

typedef struct GeometryGpu
{
    BufferSlice static_vertex_buffer;
    BufferSlice skin_input_buffer;
    BufferSlice skinned_vertex_buffer;

    BufferSlice index_buffer;
    BufferSlice mesh_buffer;
    BufferSlice material_buffer;
    BufferSlice palette_buffer;
} GeometryGpu;

typedef struct AssetContent
{
    Mesh*    meshes;
    uint32_t mesh_count;

    Material* materials;
    uint32_t  material_count;

    SkeletonAsset  skeleton;
    AnimationAsset anims;
    AssetNode*     nodes;
    uint32_t       node_count;
    mat4*          palette_mats;
    uint32_t       palette_count;

    uint32_t flags;
    uint64_t path_hash;
} AssetContent;

typedef struct AssetGpu
{
    GeometryGpu gpu;

    BufferSlice draw_buffer;
    uint32_t    draw_offset;
    uint32_t    draw_count;

    TextureID* texture_ids;
    uint32_t   texture_count;
} AssetGpu;

typedef struct ModelAsset
{
    AssetContent content;
    AssetGpu     gpu;

    uint32_t ref_count;
} ModelAsset;

/* ============================================================================
   Scene Layer (authoritative world truth)
   ============================================================================ */

typedef struct RenderInstance
{
    ModelAssetHandle asset; /* what this instance is */

    uint32_t flags; /* visible/static/dynamic/etc */

    vec3   position;    /* world translation */
    float  scale;       /* uniform scale */
    versor orientation; /* world rotation */

    uint32_t transform_anim_index; /* transform animation state handle/index */
    uint32_t skeletal_anim_index;  /* skeletal animation state handle/index */

    uint32_t user_data; /* gameplay hook / procedural tag */
} RenderInstance;
typedef struct SceneState
{
    mu_bulk_storage instances;

    mu_sparse_set active_instances;
    mu_sparse_set dirty_transforms;
    mu_sparse_set dirty_skinning;
} SceneState;

/* ============================================================================
   Animation Layer
   ============================================================================ */

typedef struct TransformAnimState
{
    uint32_t clip_index;
    float    time;
    float    speed;
    uint32_t flags;
} TransformAnimState;

typedef struct SkeletalAnimState
{
    uint32_t clip_index;
    uint32_t skeleton_index;

    float time;
    float speed;

    uint32_t palette_offset;
    uint32_t flags;
} SkeletalAnimState;

/* ============================================================================
   Frame Layer (CPU transient, rebuilt every frame)
   ============================================================================ */

typedef struct FrameInstance
{
    mat4 model;
    vec4 bounds;

    uint32_t draw_index;
    uint32_t instance_id;

    uint32_t flags;
    uint32_t lod_bias;
} FrameInstance;

typedef struct SkinJob
{
    uint32_t instance_id;
    uint32_t skeleton_state;

    uint32_t palette_offset;
    uint32_t joint_count;

    uint32_t vertex_offset;
    uint32_t vertex_count;

    uint32_t output_offset;
    uint32_t _pad;


    VkDeviceAddress skin_input_addr;
    VkDeviceAddress palette_addr;
    VkDeviceAddress skinned_out_addr;
} SkinJob;

typedef struct FrameState
{
    FrameInstance* instances;
    SkinJob*       skin_jobs;
} FrameState;
/* ============================================================================
   GPU Scene Layer (GPU transient, published every frame)
   ============================================================================ */

typedef struct GpuScene
{
    BufferSlice instance_buffer;
    BufferSlice skin_job_buffer;

    BufferSlice indirect_cmd_buffer;
    BufferSlice indirect_count_buffer;
    uint32_t    indirect_cmd_count;
} GpuScene;

/* ============================================================================
   GPU Execution Layer
   ============================================================================ */

typedef struct RenderExec
{
    BufferSlice visibility_buffer;
    BufferSlice visible_instance_ids;
} RenderExec;

/* ============================================================================
   Systems
   ============================================================================ */

typedef struct AssetSystem
{
    mu_bulk_storage models;
    mu_hash32_t     path_to_assets;
} AssetSystem;

typedef struct Scene
{
    SceneState state;
} Scene;

typedef struct AnimationSystem
{
    mu_bulk_storage clips;
    mu_bulk_storage skeletons;

    mu_bulk_storage transform_states;
    mu_bulk_storage skeletal_states;

    mu_sparse_set  active_animators;
    mu_multi_index skeleton_to_users;
} AnimationSystem;

typedef struct RenderingSystem
{
    FrameState frame;
    GpuScene   gpu;
    RenderExec exec;
} RenderingSystem;

/* ============================================================================
   Asset Load Descriptions
   ============================================================================ */

typedef struct ModelLoadDesc
{
    const char* path;
    bool        build_lods;
    bool        allow_skinning;
} ModelLoadDesc;

typedef struct MaterialOverrideDesc
{
    uint32_t material_index;
    uint32_t flags;
} MaterialOverrideDesc;

RenderInstanceHandle scene_spawn(Scene* scene, ModelAssetHandle asset);

bool scene_init(Scene* scene)
{
    MU_ASSERT(scene);
    memset(scene, 0, sizeof(*scene));

    /*
        SceneState layout
        ┌──────────────────────────────────────┐
        │ instances        : authoritative objs│
        │ active_instances : live dense set    │
        │ dirty_transforms : needs TRS rebuild │
        │ dirty_skinning   : needs palette     │
        └──────────────────────────────────────┘
    */

    if(!mu_bulk_storage_init(&scene->state.instances, sizeof(RenderInstance), 256))
        return false;

    mu_sparse_set_init(&scene->state.active_instances, 256);

    mu_sparse_set_init(&scene->state.dirty_transforms, 256);

    mu_sparse_set_init(&scene->state.dirty_skinning, 256);

    return true;
}
void scene_shutdown(Scene* scene)
{
    MU_ASSERT(scene);

    mu_sparse_set_destroy(&scene->state.dirty_skinning);
    mu_sparse_set_destroy(&scene->state.dirty_transforms);
    mu_sparse_set_destroy(&scene->state.active_instances);
    mu_bulk_storage_deinit(&scene->state.instances);

    memset(scene, 0, sizeof(*scene));
}

RenderInstanceHandle scene_spawn(Scene* scene, ModelAssetHandle asset)
{


    /*
        Spawn contract:
            prototype handle
                 |
                 v
            allocate instance slot
                 |
                 v
            initialize authoritative world state
                 |
                 v
            mark active + dirty
    */

    RenderInstanceHandle invalid = {0};

    uint32_t id = mu_bulk_storage_alloc(&scene->state.instances);
    if(id == 0)
        return invalid;

    RenderInstance* inst = (RenderInstance*)mu_bulk_storage_ptr(&scene->state.instances, id);
    if(!inst)
    {
        mu_bulk_storage_free(&scene->state.instances, id);
        return invalid;
    }

    memset(inst, 0, sizeof(*inst));

    inst->asset = asset;

    /*
        World-default state.
        Not render defaults.
        World defaults.
    */
    inst->flags = RENDER_FLAG_VISIBLE | RENDER_FLAG_DYNAMIC;

    glm_vec3_zero(inst->position);
    inst->scale = 1.0f;
    glm_quat_identity(inst->orientation);

    inst->transform_anim_index = 0;
    inst->skeletal_anim_index  = 0;
    inst->user_data            = 0;

    /*
        Instance is now alive.
        Add to authoritative live set.
    */
    mu_sparse_set_insert(&scene->state.active_instances, id);

    /*
        New objects are dirty by definition.
        They do not exist in Frame yet.
    */
    mu_sparse_set_insert(&scene->state.dirty_transforms, id);

    /*
        Skinning dirtiness is harmless for static objects and required for skinned.
        Frame can cheaply ignore non-skinned later.
        Simpler now, less branching nonsense.
    */
    mu_sparse_set_insert(&scene->state.dirty_skinning, id);

    return mu_bulk_storage_make_handle(&scene->state.instances, id);
}

void scene_destroy(Scene* scene, RenderInstanceHandle handle)
{
    MU_ASSERT(scene);

    if(!mu_bulk_storage_validate_handle(&scene->state.instances, handle))
        return;

    /*
        Kill world object.
        Remove from all authoritative tracking first.
        Free slot last.
    */
    mu_sparse_set_remove(&scene->state.active_instances, handle.id);
    mu_sparse_set_remove(&scene->state.dirty_transforms, handle.id);
    mu_sparse_set_remove(&scene->state.dirty_skinning, handle.id);

    mu_bulk_storage_free(&scene->state.instances, handle.id);
}


RenderInstance* scene_write(Scene* scene, RenderInstanceHandle handle)
{
    MU_ASSERT(scene);

    if(!mu_bulk_storage_validate_handle(&scene->state.instances, handle))
        return NULL;

    /*
        Scene mutation path.
        Any mutable access implies caller may change authoritative truth.
        Mark transform dirty immediately.

        Why immediately?
        Because humans forget.
        Dirty tracking should not depend on discipline.
    */
    mu_sparse_set_insert(&scene->state.dirty_transforms, handle.id);

    return (RenderInstance*)mu_bulk_storage_resolve_handle(&scene->state.instances, handle);
}

const RenderInstance* scene_read(const Scene* scene, RenderInstanceHandle handle)
{
    MU_ASSERT(scene);

    if(!mu_bulk_storage_validate_handle(&scene->state.instances, handle))
        return NULL;

    return (const RenderInstance*)mu_bulk_storage_resolve_handle_const(&scene->state.instances, handle);
}


static void build_model_matrix(RenderInstance* inst, mat4 out_model)
{
    mat4 T, R, S, RS;

    /*
        Build TRS transform the sane way.

        Model = T * R * S

        ┌─────────────┐
        │ Scale       │  local size
        └─────┬───────┘
              ↓
        ┌─────────────┐
        │ Rotate      │  local orientation
        └─────┬───────┘
              ↓
        ┌─────────────┐
        │ Translate   │  world position
        └─────┬───────┘
              ↓
        ┌─────────────┐
        │ Final Model │
        └─────────────┘

        Order matters.
        Always.
        Matrix math is not a democracy.
    */

    glm_translate_make(T, inst->position);                             // T
    glm_quat_mat4(inst->orientation, R);                               // R
    glm_scale_make(S, (vec3){inst->scale, inst->scale, inst->scale});  // S

    glm_mat4_mul(R, S, RS);          // RS = R * S
    glm_mat4_mul(T, RS, out_model);  // M  = T * (R * S)
}

static void asset_build_node_local_transform(const cgltf_node* node, mat4 out_local)
{
    if(!node || !out_local)
        return;

    if(node->has_matrix)
    {
        for(int r = 0; r < 4; ++r)
            for(int c = 0; c < 4; ++c)
                out_local[r][c] = (float)node->matrix[r * 4 + c];
        return;
    }

    mat4 T, R, S, RS;
    glm_translate_make(T, (vec3){(float)node->translation[0], (float)node->translation[1], (float)node->translation[2]});
    glm_quat_mat4((versor){(float)node->rotation[0], (float)node->rotation[1], (float)node->rotation[2], (float)node->rotation[3]}, R);
    glm_scale_make(S, (vec3){(float)node->scale[0], (float)node->scale[1], (float)node->scale[2]});

    glm_mat4_mul(R, S, RS);
    glm_mat4_mul(T, RS, out_local);
}

static void asset_node_make_local(const vec3 translation, const versor rotation, const vec3 scale, mat4 out_local)
{
    mat4 T, R, S, RS;
    vec3   t;
    versor r;
    vec3   s;
    glm_vec3_copy((vec3){translation[0], translation[1], translation[2]}, t);
    glm_quat_copy((versor){rotation[0], rotation[1], rotation[2], rotation[3]}, r);
    glm_vec3_copy((vec3){scale[0], scale[1], scale[2]}, s);
    glm_translate_make(T, t);
    glm_quat_mat4(r, R);
    glm_scale_make(S, s);
    glm_mat4_mul(R, S, RS);
    glm_mat4_mul(T, RS, out_local);
}

static void asset_node_init_from_gltf(AssetNode* out_node, const cgltf_data* data, const cgltf_node* node)
{
    memset(out_node, 0, sizeof(*out_node));
    if(data && node && node->parent && data->nodes && node->parent >= data->nodes && node->parent < data->nodes + data->nodes_count)
        out_node->parent_index = (int32_t)(node->parent - data->nodes);
    else
        out_node->parent_index = -1;

    glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, out_node->translation);
    glm_quat_identity(out_node->rotation);
    glm_vec3_copy((vec3){1.0f, 1.0f, 1.0f}, out_node->scale);

    if(node)
    {
        if(node->has_translation)
            glm_vec3_copy((vec3){(float)node->translation[0], (float)node->translation[1], (float)node->translation[2]},
                          out_node->translation);
        if(node->has_rotation)
            glm_quat_copy((versor){(float)node->rotation[0], (float)node->rotation[1], (float)node->rotation[2],
                                   (float)node->rotation[3]},
                          out_node->rotation);
        if(node->has_scale)
            glm_vec3_copy((vec3){(float)node->scale[0], (float)node->scale[1], (float)node->scale[2]}, out_node->scale);
    }

    asset_build_node_local_transform(node, out_node->local);
    glm_mat4_identity(out_node->global);
}

static void animation_sample_values(const AnimationSampler* sampler, float time, float* out_values)
{
    if(!sampler || !out_values || sampler->input_count == 0 || sampler->output_count == 0)
        return;

    const uint32_t comps = sampler->output_components;
    uint32_t       a     = 0;
    uint32_t       b     = 0;

    if(time <= sampler->input_times[0])
    {
        a = b = 0;
    }
    else if(time >= sampler->input_times[sampler->input_count - 1])
    {
        a = b = sampler->input_count - 1;
    }
    else
    {
        for(uint32_t i = 0; i + 1 < sampler->input_count; ++i)
        {
            if(time >= sampler->input_times[i] && time <= sampler->input_times[i + 1])
            {
                a = i;
                b = i + 1;
                break;
            }
        }
    }

    const float* va = &sampler->output_values[a * comps];
    const float* vb = &sampler->output_values[b * comps];
    float        t  = 0.0f;
    if(a != b)
    {
        float denom = sampler->input_times[b] - sampler->input_times[a];
        if(denom > 0.0f)
            t = (time - sampler->input_times[a]) / denom;
    }

    if(sampler->interpolation == cgltf_interpolation_type_step || a == b)
    {
        memcpy(out_values, va, (size_t)comps * sizeof(float));
        return;
    }

    for(uint32_t i = 0; i < comps; ++i)
        out_values[i] = va[i] + (vb[i] - va[i]) * t;
}

static void model_asset_evaluate_animation(ModelAsset* model, uint32_t clip_index, float time)
{
    if(!model || model->content.node_count == 0 || model->content.palette_count == 0 || !model->content.palette_mats)
        return;

    const AnimationAsset* anims = &model->content.anims;
    const AnimationClip*  clip  = clip_index < anims->clip_count ? &anims->clips[clip_index] : NULL;
    if(clip && clip->duration > 0.0f)
        time = fmodf(time, clip->duration);

    for(uint32_t i = 0; i < model->content.node_count; ++i)
    {
        AssetNode* node = &model->content.nodes[i];
        asset_node_make_local(node->translation, node->rotation, node->scale, node->local);
    }

    for(uint32_t ci = 0; clip && ci < clip->channel_count; ++ci)
    {
        const AnimationChannel* channel = &anims->channels[clip->channel_offset + ci];
        if(channel->target_node_index >= model->content.node_count || channel->sampler_index >= anims->sampler_count)
            continue;

        const AnimationSampler* sampler = &anims->samplers[channel->sampler_index];
        AssetNode*              node    = &model->content.nodes[channel->target_node_index];
        float                   v[4]    = {0};
        animation_sample_values(sampler, time, v);

        if(channel->target_path == cgltf_animation_path_type_translation && sampler->output_components >= 3)
            glm_vec3_copy((vec3){v[0], v[1], v[2]}, node->translation);
        else if(channel->target_path == cgltf_animation_path_type_scale && sampler->output_components >= 3)
            glm_vec3_copy((vec3){v[0], v[1], v[2]}, node->scale);
        else if(channel->target_path == cgltf_animation_path_type_rotation && sampler->output_components >= 4)
        {
            glm_quat_copy((versor){v[0], v[1], v[2], v[3]}, node->rotation);
            glm_quat_normalize(node->rotation);
        }

        asset_node_make_local(node->translation, node->rotation, node->scale, node->local);
    }

    for(uint32_t i = 0; i < model->content.node_count; ++i)
    {
        AssetNode* node = &model->content.nodes[i];
        if(node->parent_index >= 0 && (uint32_t)node->parent_index < model->content.node_count)
            glm_mat4_mul(model->content.nodes[node->parent_index].global, node->local, node->global);
        else
            glm_mat4_copy(node->local, node->global);
    }

    for(uint32_t i = 0; i < model->content.skeleton.joint_count; ++i)
    {
        const SkeletonJoint* joint = &model->content.skeleton.joints[i];
        if(joint->node_index >= model->content.node_count)
            continue;

        mat4 inverse_bind;
        memcpy(inverse_bind, joint->inverse_bind, sizeof(mat4));
        glm_mat4_mul(model->content.nodes[joint->node_index].global, inverse_bind, model->content.palette_mats[i]);
    }
}

static void model_asset_print_animations(const ModelAsset* model)
{
    if(!model)
        return;

    printf("animations: %u\n", model->content.anims.clip_count);
    for(uint32_t i = 0; i < model->content.anims.clip_count; ++i)
    {
        const AnimationClip* clip = &model->content.anims.clips[i];
        printf("  [%u] %s duration=%.3fs channels=%u\n", i, clip->name && clip->name[0] ? clip->name : "(unnamed)",
               clip->duration, clip->channel_count);
    }
}

static bool print_gltf_animation_names(const char* path)
{
    cgltf_options options = {0};
    cgltf_data*   data    = NULL;

    if(cgltf_parse_file(&options, path, &data) != cgltf_result_success)
    {
        fprintf(stderr, "failed to parse %s\n", path);
        return false;
    }

    if(cgltf_load_buffers(&options, data, path) != cgltf_result_success)
    {
        fprintf(stderr, "failed to load buffers for %s\n", path);
        cgltf_free(data);
        return false;
    }

    printf("animations in %s: %u\n", path, (uint32_t)data->animations_count);
    for(cgltf_size ai = 0; ai < data->animations_count; ++ai)
    {
        const cgltf_animation* anim     = &data->animations[ai];
        float                  duration = 0.0f;
        for(cgltf_size si = 0; si < anim->samplers_count; ++si)
        {
            const cgltf_animation_sampler* sampler = &anim->samplers[si];
            if(!sampler->input || sampler->input->count == 0)
                continue;

            float last_time = 0.0f;
            if(cgltf_accessor_read_float(sampler->input, sampler->input->count - 1, &last_time, 1))
                duration = fmaxf(duration, last_time);
        }

        printf("  [%u] %s duration=%.3fs channels=%u\n", (uint32_t)ai,
               anim->name && anim->name[0] ? anim->name : "(unnamed)", duration, (uint32_t)anim->channels_count);
    }

    cgltf_free(data);
    return true;
}

static int32_t asset_find_joint_index(const cgltf_skin* skin, const cgltf_node* node)
{
    if(!skin || !node)
        return -1;

    for(cgltf_size i = 0; i < skin->joints_count; ++i)
    {
        if(skin->joints[i] == node)
            return (int32_t)i;
    }

    return -1;
}


typedef struct GltfCpuBuild
{
    PackedVertex* vertices;
    uint32_t      vertex_count;

    uint32_t* indices;
    uint32_t  index_count;

    Mesh*    meshes;
    uint32_t mesh_count;

    Material* materials;
    uint32_t  material_count;

    GpuDraw* draws;
    uint32_t draw_count;
} GltfCpuBuild;

/* Forward declarations for renderer helpers used by the importer. */
bool geometry_gpu_upload(Renderer*    renderer,
                         const void*  static_vertices,
                         size_t       static_vertices_size,
                         const void*  skin_vertices,
                         size_t       skin_vertices_size,
                         const void*  static_indices,
                         size_t       static_indices_size,
                         const void*  skin_indices,
                         size_t       skin_indices_size,
                         const void*  meshes,
                         size_t       meshes_size,
                         const void*  materials,
                         size_t       materials_size,
                         const void*  palette_mats,
                         size_t       palette_size,
                         GeometryGpu* out_gpu);

BufferSlice renderer_upload_draws(Renderer* r, const GpuDraw* draws, uint32_t draw_count);

static void asset_destroy_gpu(Renderer* renderer, AssetGpu* gpu);

static bool asset_upload_slice(Renderer* renderer, VkCommandBuffer cmd, BufferSlice* out_slice, const void* data, size_t bytes, VkDeviceSize alignment)
{
    if(!out_slice)
        return false;

    memset(out_slice, 0, sizeof(*out_slice));

    if(bytes == 0)
        return true;

    *out_slice = buffer_pool_alloc(&renderer->gpu_pool, bytes, alignment);
    if(!out_slice->buffer)
        return false;

    if(data && !renderer_upload_buffer_to_slice(renderer, cmd, *out_slice, data, bytes, alignment))
    {
        buffer_pool_free(*out_slice);
        memset(out_slice, 0, sizeof(*out_slice));
        return false;
    }

    return true;
}

bool geometry_gpu_upload(Renderer*    renderer,
                         const void*  static_vertices,
                         size_t       static_vertices_size,
                         const void*  skin_vertices,
                         size_t       skin_vertices_size,
                         const void*  static_indices,
                         size_t       static_indices_size,
                         const void*  skin_indices,
                         size_t       skin_indices_size,
                         const void*  meshes,
                         size_t       meshes_size,
                         const void*  materials,
                         size_t       materials_size,
                         const void*  palette_mats,
                         size_t       palette_size,
                         GeometryGpu* out_gpu)
{
    if(!renderer || !out_gpu)
        return false;

    memset(out_gpu, 0, sizeof(*out_gpu));

    const size_t skinned_vertex_bytes = (skin_vertices_size / sizeof(PackedSkinVertex)) * sizeof(PackedVertex);
    const size_t combined_index_bytes = static_indices_size + skin_indices_size;
    uint8_t*     combined_indices     = NULL;

    if(combined_index_bytes > 0)
    {
        combined_indices = mu_malloc(combined_index_bytes);
        if(!combined_indices)
            return false;

        if(static_indices && static_indices_size > 0)
            memcpy(combined_indices, static_indices, static_indices_size);
        if(skin_indices && skin_indices_size > 0)
            memcpy(combined_indices + static_indices_size, skin_indices, skin_indices_size);
    }

    VkCommandBuffer cmd = vk_begin_one_time_cmd(renderer->device, renderer->one_time_gfx_pool);
    if(!cmd)
    {
        mu_free(combined_indices);
        return false;
    }

    bool ok = true;
    ok &= asset_upload_slice(renderer, cmd, &out_gpu->static_vertex_buffer, static_vertices, static_vertices_size, 16);
    ok &= asset_upload_slice(renderer, cmd, &out_gpu->skin_input_buffer, skin_vertices, skin_vertices_size, 16);
    ok &= asset_upload_slice(renderer, cmd, &out_gpu->skinned_vertex_buffer, NULL, skinned_vertex_bytes, 16);
    ok &= asset_upload_slice(renderer, cmd, &out_gpu->index_buffer, combined_indices, combined_index_bytes, 16);
    ok &= asset_upload_slice(renderer, cmd, &out_gpu->mesh_buffer, meshes, meshes_size, 16);
    ok &= asset_upload_slice(renderer, cmd, &out_gpu->material_buffer, materials, materials_size, 16);
    ok &= asset_upload_slice(renderer, cmd, &out_gpu->palette_buffer, palette_mats, palette_size, 16);

    vk_end_one_time_cmd(renderer->device, renderer->graphics_queue, renderer->one_time_gfx_pool, cmd);
    mu_free(combined_indices);

    if(!ok)
    {
        buffer_pool_free(out_gpu->static_vertex_buffer);
        buffer_pool_free(out_gpu->skin_input_buffer);
        buffer_pool_free(out_gpu->skinned_vertex_buffer);
        buffer_pool_free(out_gpu->index_buffer);
        buffer_pool_free(out_gpu->mesh_buffer);
        buffer_pool_free(out_gpu->material_buffer);
        buffer_pool_free(out_gpu->palette_buffer);
        memset(out_gpu, 0, sizeof(*out_gpu));
        return false;
    }

    return true;
}

BufferSlice renderer_upload_draws(Renderer* r, const GpuDraw* draws, uint32_t draw_count)
{
    BufferSlice slice = {0};

    if(!r || !draws || draw_count == 0)
        return slice;

    VkCommandBuffer cmd = vk_begin_one_time_cmd(r->device, r->one_time_gfx_pool);
    if(!cmd)
        return slice;

    slice = renderer_upload_buffer(r, cmd, draws, (VkDeviceSize)draw_count * sizeof(GpuDraw), 16, 16);
    vk_end_one_time_cmd(r->device, r->graphics_queue, r->one_time_gfx_pool, cmd);

    return slice;
}


static bool asset_build_from_gltf(Renderer* renderer, const ModelLoadDesc* desc, ModelAsset* out_asset)
{
    /*
        Full importer pass:
        - static geometry
        - skinned geometry
        - material import
        - palette / inverse bind import
        - animation presence / validation
        - GPU upload

        What this does NOT persist yet:
        - runtime animation clips
        - runtime skeleton clips/state tables

        That belongs to a separate animation asset container.
    */

    enum
    {
        ASSET_FLAG_HAS_SKINNING   = 1u << 0,
        ASSET_FLAG_HAS_ANIMATIONS = 1u << 1,
    };

    MU_ASSERT(renderer);
    MU_ASSERT(desc);
    MU_ASSERT(desc->path);
    MU_ASSERT(out_asset);

    memset(out_asset, 0, sizeof(*out_asset));

    cgltf_options options = {0};
    cgltf_data*   data    = NULL;

    Mesh*             meshes               = NULL;
    Material*         materials            = NULL;
    GpuDraw*          draws                = NULL;
    SkeletonSkin*     skeleton_skins       = NULL;
    SkeletonJoint*    skeleton_joints      = NULL;
    AnimationClip*    anim_clips           = NULL;
    AnimationSampler* anim_samplers        = NULL;
    AnimationChannel* anim_channels        = NULL;
    AssetNode*        nodes                = NULL;
    PackedVertex*     static_vertices      = NULL;
    PackedSkinVertex* skin_vertices        = NULL;
    uint32_t*         static_indices       = NULL;
    uint32_t*         skin_indices         = NULL;
    mat4*             palette_mats         = NULL;
    uint32_t*         skin_palette_offsets = NULL;
    TextureID*        texture_ids          = NULL;

    uint32_t total_meshes       = 0;
    uint32_t total_draws        = 0;
    uint32_t total_static_verts = 0;
    uint32_t total_skin_verts   = 0;
    uint32_t total_static_inds  = 0;
    uint32_t total_skin_inds    = 0;
    uint32_t total_materials    = 0;
    uint32_t total_skins        = 0;
    uint32_t total_joints       = 0;
    uint32_t total_clip_count   = 0;
    uint32_t total_samplers     = 0;
    uint32_t total_channels     = 0;
    uint32_t total_textures     = 0;
    uint32_t total_nodes        = 0;

    cgltf_result result = cgltf_parse_file(&options, desc->path, &data);
    if(result != cgltf_result_success)
    {
        goto fail;
    }

    result = cgltf_load_buffers(&options, data, desc->path);
    if(result != cgltf_result_success)
    {
        goto fail;
    }

    result = cgltf_validate(data);
    if(result != cgltf_result_success)
    {
        goto fail;
    }

    /*
        ------------------------------------------------------------------------
        Pass 1: count totals
        ------------------------------------------------------------------------
    */

    total_materials  = (uint32_t)(data->materials_count ? data->materials_count : 1);
    total_skins      = (uint32_t)data->skins_count;
    total_clip_count = (uint32_t)data->animations_count;
    total_textures   = (uint32_t)data->textures_count;
    total_nodes      = (uint32_t)data->nodes_count;

    for(cgltf_size si = 0; si < data->skins_count; ++si)
        total_joints += (uint32_t)data->skins[si].joints_count;

    for(cgltf_size ai = 0; ai < data->animations_count; ++ai)
    {
        total_samplers += (uint32_t)data->animations[ai].samplers_count;
        total_channels += (uint32_t)data->animations[ai].channels_count;
    }

    bool has_skinning  = false;
    bool has_animation = (data->animations_count > 0);

    for(cgltf_size ai = 0; ai < data->animations_count; ++ai)
    {
        const cgltf_animation* anim = &data->animations[ai];
        /*
            Validate animation track structure.
            We store the clip metadata later, but we need all pointers valid now.
        */
        for(cgltf_size si = 0; si < anim->samplers_count; ++si)
        {
            const cgltf_animation_sampler* samp = &anim->samplers[si];
            if(!samp->input || !samp->output)
            {
                goto fail;
            }
        }

        for(cgltf_size ci = 0; ci < anim->channels_count; ++ci)
        {
            const cgltf_animation_channel* ch = &anim->channels[ci];
            if(!ch->sampler || !ch->target_node)
            {
                goto fail;
            }
        }
    }

    /*
        Count primitives and classify them.
        Phase 1 importer uses flat primitive records.
    */
    for(cgltf_size mi = 0; mi < data->meshes_count; ++mi)
    {
        const cgltf_mesh* mesh = &data->meshes[mi];

        for(cgltf_size pi = 0; pi < mesh->primitives_count; ++pi)
        {
            const cgltf_primitive* prim = &mesh->primitives[pi];

            if(prim->type != cgltf_primitive_type_triangles)
                continue;

            const cgltf_accessor* pos_accessor = cgltf_find_accessor(prim, cgltf_attribute_type_position, 0);
            const cgltf_accessor* idx_accessor = prim->indices;

            if(!pos_accessor || !idx_accessor)
                continue;

            bool prim_has_skin    = false;
            bool prim_has_joints  = false;
            bool prim_has_weights = false;

            for(cgltf_size ai = 0; ai < prim->attributes_count; ++ai)
            {
                const cgltf_attribute* attr = &prim->attributes[ai];
                if(attr->type == cgltf_attribute_type_joints)
                    prim_has_joints = true;
                if(attr->type == cgltf_attribute_type_weights)
                    prim_has_weights = true;
            }

            prim_has_skin = (prim_has_joints && prim_has_weights);

            if(prim_has_skin && desc->allow_skinning)
            {
                has_skinning = true;
                total_skin_verts += (uint32_t)pos_accessor->count;
                total_skin_inds += (uint32_t)idx_accessor->count;
            }
            else
            {
                total_static_verts += (uint32_t)pos_accessor->count;
                total_static_inds += (uint32_t)idx_accessor->count;
            }

            total_meshes++;
            total_draws++;
        }
    }

    if(total_meshes == 0 || (total_static_verts == 0 && total_skin_verts == 0))
    {
        cgltf_free(data);
        return false;
    }

    if(has_skinning)
        out_asset->content.flags |= 1u; /* internal: asset contains skinning */

    if(has_animation)
        out_asset->content.flags |= 2u; /* internal: asset contains animations */

    /*
        ------------------------------------------------------------------------
        Allocate CPU staging
        ------------------------------------------------------------------------
    */

    meshes    = mu_calloc(total_meshes, sizeof(Mesh));
    materials = mu_calloc(total_materials, sizeof(Material));
    draws     = mu_calloc(total_draws, sizeof(GpuDraw));

    skeleton_skins  = (total_skins ? mu_calloc(total_skins, sizeof(SkeletonSkin)) : NULL);
    skeleton_joints = (total_joints ? mu_calloc(total_joints, sizeof(SkeletonJoint)) : NULL);
    anim_clips      = (total_clip_count ? mu_calloc(total_clip_count, sizeof(AnimationClip)) : NULL);
    anim_samplers   = (total_samplers ? mu_calloc(total_samplers, sizeof(AnimationSampler)) : NULL);
    anim_channels   = (total_channels ? mu_calloc(total_channels, sizeof(AnimationChannel)) : NULL);
    nodes           = (total_nodes ? mu_calloc(total_nodes, sizeof(AssetNode)) : NULL);

    static_vertices = (total_static_verts ? mu_malloc(total_static_verts * sizeof(PackedVertex)) : NULL);
    skin_vertices   = (total_skin_verts ? mu_malloc(total_skin_verts * sizeof(PackedSkinVertex)) : NULL);

    static_indices = (total_static_inds ? mu_malloc(total_static_inds * sizeof(uint32_t)) : NULL);
    skin_indices   = (total_skin_inds ? mu_malloc(total_skin_inds * sizeof(uint32_t)) : NULL);

    /*
        Palette buffer stores inverse bind matrices.
        One mat4 per joint in all skins combined.
    */
    uint32_t total_palette_mats = 0;
    for(cgltf_size si = 0; si < data->skins_count; ++si)
        total_palette_mats += (uint32_t)data->skins[si].joints_count;

    palette_mats = (total_palette_mats ? mu_malloc(total_palette_mats * sizeof(mat4)) : NULL);

    skin_palette_offsets = (data->skins_count ? mu_malloc((size_t)data->skins_count * sizeof(uint32_t)) : NULL);
    if(skin_palette_offsets)
    {
        uint32_t off = 0;
        for(cgltf_size si = 0; si < data->skins_count; ++si)
        {
            skin_palette_offsets[si] = off;
            off += (uint32_t)data->skins[si].joints_count;
        }
    }

    texture_ids = (total_textures ? mu_malloc((size_t)total_textures * sizeof(TextureID)) : NULL);
    for(uint32_t i = 0; i < total_textures; ++i)
        texture_ids[i] = UINT32_MAX;

    if(!meshes || !materials || !draws || (total_skins && !skeleton_skins) || (total_joints && !skeleton_joints)
       || (total_clip_count && !anim_clips) || (total_samplers && !anim_samplers) || (total_channels && !anim_channels)
       || (total_static_verts && !static_vertices) || (total_skin_verts && !skin_vertices)
       || (total_static_inds && !static_indices) || (total_skin_inds && !skin_indices) || (total_palette_mats && !palette_mats)
       || (total_nodes && !nodes) || (total_textures && !texture_ids))
    {
        goto fail;
    }

    for(uint32_t i = 0; i < total_nodes; ++i)
        asset_node_init_from_gltf(&nodes[i], data, &data->nodes[i]);

    for(uint32_t i = 0; i < total_textures; ++i)
    {
        const cgltf_texture* texture = &data->textures[i];
        const cgltf_image*   image   = texture->image ? texture->image : texture->basisu_image;
        texture_ids[i] = asset_load_gltf_image(renderer, desc->path, image);
        if(texture_ids[i] == UINT32_MAX)
            texture_ids[i] = renderer->dummy_texture;
    }

    uint32_t sampler_cursor = 0;
    uint32_t channel_cursor = 0;
    for(cgltf_size ai = 0; ai < data->animations_count; ++ai)
    {
        const cgltf_animation* anim = &data->animations[ai];
        AnimationClip*         clip = &anim_clips[ai];

        clip->name           = mu_strdup_local(anim->name && anim->name[0] ? anim->name : "unnamed");
        clip->sampler_offset = sampler_cursor;
        clip->sampler_count  = (uint32_t)anim->samplers_count;
        clip->channel_offset = channel_cursor;
        clip->channel_count  = (uint32_t)anim->channels_count;
        clip->duration       = 0.0f;

        for(cgltf_size si = 0; si < anim->samplers_count; ++si)
        {
            const cgltf_animation_sampler* samp     = &anim->samplers[si];
            AnimationSampler*              samp_out = &anim_samplers[sampler_cursor + (uint32_t)si];

            cgltf_size input_components  = cgltf_num_components(samp->input->type);
            cgltf_size output_components = cgltf_num_components(samp->output->type);
            if(input_components != 1 || output_components == 0 || samp->input->count == 0 || samp->output->count == 0)
                goto fail;

            samp_out->input_count       = (uint32_t)samp->input->count;
            samp_out->output_count      = (uint32_t)samp->output->count;
            samp_out->output_components = (uint32_t)output_components;
            samp_out->interpolation     = (uint32_t)samp->interpolation;
            samp_out->input_times       = mu_malloc((size_t)samp_out->input_count * sizeof(float));
            samp_out->output_values =
                mu_malloc((size_t)samp_out->output_count * (size_t)samp_out->output_components * sizeof(float));

            if(!samp_out->input_times || !samp_out->output_values)
                goto fail;

            for(uint32_t k = 0; k < samp_out->input_count; ++k)
            {
                if(!cgltf_accessor_read_float(samp->input, k, &samp_out->input_times[k], 1))
                    goto fail;
            }

            for(uint32_t k = 0; k < samp_out->output_count; ++k)
            {
                float* dst = &samp_out->output_values[k * samp_out->output_components];
                if(!cgltf_accessor_read_float(samp->output, k, dst, samp_out->output_components))
                    goto fail;
            }

            clip->duration = fmaxf(clip->duration, samp_out->input_times[samp_out->input_count - 1]);
        }

        for(cgltf_size ci = 0; ci < anim->channels_count; ++ci)
        {
            const cgltf_animation_channel* ch     = &anim->channels[ci];
            AnimationChannel*              ch_out = &anim_channels[channel_cursor + (uint32_t)ci];

            ch_out->sampler_index     = sampler_cursor + (uint32_t)cgltf_animation_sampler_index(anim, ch->sampler);
            ch_out->target_node_index = (uint32_t)cgltf_node_index(data, ch->target_node);
            ch_out->target_path       = (uint32_t)ch->target_path;
        }

        sampler_cursor += (uint32_t)anim->samplers_count;
        channel_cursor += (uint32_t)anim->channels_count;
    }

    /*
        ------------------------------------------------------------------------
        Materials
        ------------------------------------------------------------------------
    */

    for(uint32_t i = 0; i < total_materials; ++i)
    {
        Material* dst = &materials[i];

        glm_vec4_one(dst->base_color_factor);
        glm_vec4_one(dst->specular_factor);
        glm_vec4_zero(dst->emissive_factor);

        dst->albedo_texture   = UINT32_MAX;
        dst->normal_texture   = UINT32_MAX;
        dst->specular_texture = UINT32_MAX;
        dst->emissive_texture = UINT32_MAX;
        dst->flags            = MATERIAL_FLAG_NONE;

        if(i >= data->materials_count)
            continue;

        const cgltf_material* src = &data->materials[i];

        if(src->double_sided)
            dst->flags |= MATERIAL_FLAG_DOUBLE_SIDED;

        if(src->unlit)
            dst->flags |= MATERIAL_FLAG_UNLIT;

        if(src->alpha_mode == cgltf_alpha_mode_mask)
            dst->flags |= MATERIAL_FLAG_ALPHA_MASKED;

        if(src->has_pbr_metallic_roughness)
        {
            const cgltf_pbr_metallic_roughness* pbr = &src->pbr_metallic_roughness;

            dst->base_color_factor[0] = pbr->base_color_factor[0];
            dst->base_color_factor[1] = pbr->base_color_factor[1];
            dst->base_color_factor[2] = pbr->base_color_factor[2];
            dst->base_color_factor[3] = pbr->base_color_factor[3];

            dst->specular_factor[0] = pbr->metallic_factor;
            dst->specular_factor[1] = pbr->roughness_factor;
        }

        if(src->has_emissive_strength)
        {
            dst->emissive_factor[3] = src->emissive_strength.emissive_strength;
            dst->flags |= MATERIAL_FLAG_EMISSIVE_BLOOM;
        }

        if(src->normal_texture.texture)
        {
            uint32_t tex_index = (uint32_t)cgltf_texture_index(data, src->normal_texture.texture);
            if(tex_index < total_textures)
                dst->normal_texture = texture_ids[tex_index];
        }

        if(src->emissive_texture.texture)
        {
            uint32_t tex_index = (uint32_t)cgltf_texture_index(data, src->emissive_texture.texture);
            if(tex_index < total_textures)
                dst->emissive_texture = texture_ids[tex_index];
        }

        if(src->has_pbr_metallic_roughness)
        {
            const cgltf_pbr_metallic_roughness* pbr = &src->pbr_metallic_roughness;
            if(pbr->base_color_texture.texture)
            {
                uint32_t tex_index = (uint32_t)cgltf_texture_index(data, pbr->base_color_texture.texture);
                if(tex_index < total_textures)
                    dst->albedo_texture = texture_ids[tex_index];
            }
            if(pbr->metallic_roughness_texture.texture)
            {
                uint32_t tex_index = (uint32_t)cgltf_texture_index(data, pbr->metallic_roughness_texture.texture);
                if(tex_index < total_textures)
                    dst->specular_texture = texture_ids[tex_index];
            }
        }
    }

    /*
        ------------------------------------------------------------------------
        Skin and palette import
        ------------------------------------------------------------------------
        We store one flattenable skeleton table:
        - skins[] gives per-skin joint ranges
        - joints[] gives hierarchy + bind pose + inverse bind matrices
        Mesh.padding stores base palette offset for the first skinned primitive
        that uses that skin.
    */

    uint32_t palette_cursor = 0;
    uint32_t joint_cursor   = 0;
    for(cgltf_size si = 0; si < data->skins_count; ++si)
    {
        const cgltf_skin* skin     = &data->skins[si];
        SkeletonSkin*     skin_out = &skeleton_skins[si];
        skin_out->joint_offset     = joint_cursor;
        skin_out->joint_count      = (uint32_t)skin->joints_count;

        for(cgltf_size ji = 0; ji < skin->joints_count; ++ji)
        {
            const cgltf_node* joint_node = skin->joints[ji];
            SkeletonJoint*    joint_out  = &skeleton_joints[joint_cursor + (uint32_t)ji];
            mat4              ibm        = GLM_MAT4_IDENTITY_INIT;

            joint_out->node_index   = (uint32_t)cgltf_node_index(data, joint_node);
            joint_out->skin_index   = (uint32_t)si;
            joint_out->parent_index = -1;
            asset_build_node_local_transform(joint_node, joint_out->local_bind);

            for(const cgltf_node* parent = joint_node ? joint_node->parent : NULL; parent; parent = parent->parent)
            {
                int32_t parent_local = asset_find_joint_index(skin, parent);
                if(parent_local >= 0)
                {
                    joint_out->parent_index = (int32_t)(skin_out->joint_offset + (uint32_t)parent_local);
                    break;
                }
            }

            if(skin->inverse_bind_matrices)
            {
                float tmp[16] = {0};
                if(!cgltf_accessor_read_float(skin->inverse_bind_matrices, ji, tmp, 16))
                {
                    goto fail;
                }

                for(int k = 0; k < 16; ++k)
                    ibm[k / 4][k % 4] = tmp[k];
            }

            glm_mat4_copy(ibm, palette_mats[palette_cursor++]);
            glm_mat4_copy(ibm, joint_out->inverse_bind);
        }

        joint_cursor += (uint32_t)skin->joints_count;
    }

    /*
        ------------------------------------------------------------------------
        Flatten primitives into CPU arrays
        ------------------------------------------------------------------------
    */

    uint32_t mesh_cursor     = 0;
    uint32_t draw_cursor     = 0;
    uint32_t static_v_cursor = 0;
    uint32_t static_i_cursor = 0;
    uint32_t skin_v_cursor   = 0;
    uint32_t skin_i_cursor   = 0;
    (void)0;

    for(cgltf_size mi = 0; mi < data->meshes_count; ++mi)
    {
        const cgltf_mesh* mesh = &data->meshes[mi];

        for(cgltf_size pi = 0; pi < mesh->primitives_count; ++pi)
        {
            const cgltf_primitive* prim = &mesh->primitives[pi];

            if(prim->type != cgltf_primitive_type_triangles)
                continue;

            const cgltf_accessor* pos_accessor = cgltf_find_accessor(prim, cgltf_attribute_type_position, 0);
            const cgltf_accessor* nrm_accessor = cgltf_find_accessor(prim, cgltf_attribute_type_normal, 0);
            const cgltf_accessor* uv_accessor  = cgltf_find_accessor(prim, cgltf_attribute_type_texcoord, 0);
            const cgltf_accessor* jnt_accessor = cgltf_find_accessor(prim, cgltf_attribute_type_joints, 0);
            const cgltf_accessor* wgt_accessor = cgltf_find_accessor(prim, cgltf_attribute_type_weights, 0);
            const cgltf_accessor* idx_accessor = prim->indices;

            if(!pos_accessor || !idx_accessor)
                continue;

            bool              prim_is_skinned = false;
            const cgltf_skin* prim_skin       = NULL;
            if(desc->allow_skinning && jnt_accessor && wgt_accessor)
            {
                for(cgltf_size ni = 0; ni < data->nodes_count; ++ni)
                {
                    const cgltf_node* node = &data->nodes[ni];
                    if(node->mesh != mesh)
                        continue;

                    for(cgltf_size k = 0; k < mesh->primitives_count; ++k)
                    {
                        if(&mesh->primitives[k] == prim)
                        {
                            if(node->skin)
                            {
                                prim_is_skinned = true;
                                prim_skin       = node->skin;
                            }
                            break;
                        }
                    }

                    if(prim_is_skinned)
                        break;
                }
            }

            Mesh*    out_mesh = &meshes[mesh_cursor];
            GpuDraw* out_draw = &draws[draw_cursor];

            vec3 bmin = {FLT_MAX, FLT_MAX, FLT_MAX};
            vec3 bmax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

            uint32_t local_vert_count = (uint32_t)pos_accessor->count;
            uint32_t local_idx_count  = (uint32_t)idx_accessor->count;

            if(prim_is_skinned)
            {
                uint32_t skin_base = 0;
                for(cgltf_size si = 0; si < data->skins_count; ++si)
                {
                    if(&data->skins[si] == prim_skin)
                    {
                        skin_base = skin_palette_offsets ? skin_palette_offsets[si] : 0;
                        break;
                    }
                }

                out_mesh->padding = skin_base;

                for(uint32_t v = 0; v < local_vert_count; ++v)
                {
                    float    p[3]       = {0};
                    float    n[3]       = {0, 1, 0};
                    float    uv[2]      = {0};
                    uint16_t joints[4]  = {0, 0, 0, 0};
                    uint16_t weights[4] = {0, 0, 0, 0};

                    cgltf_accessor_read_float(pos_accessor, v, p, 3);
                    if(nrm_accessor)
                        cgltf_accessor_read_float(nrm_accessor, v, n, 3);
                    if(uv_accessor)
                        cgltf_accessor_read_float(uv_accessor, v, uv, 2);

                    if(jnt_accessor)
                    {
                        cgltf_uint ji[4] = {0};
                        if(cgltf_accessor_read_uint(jnt_accessor, v, ji, 4))
                        {
                            joints[0] = (uint16_t)ji[0];
                            joints[1] = (uint16_t)ji[1];
                            joints[2] = (uint16_t)ji[2];
                            joints[3] = (uint16_t)ji[3];
                        }
                    }

                    if(wgt_accessor)
                    {
                        float wf[4] = {0};
                        if(cgltf_accessor_read_float(wgt_accessor, v, wf, 4))
                        {
                            weights[0] = (uint16_t)fminf(fmaxf(wf[0] * 65535.0f, 0.0f), 65535.0f);
                            weights[1] = (uint16_t)fminf(fmaxf(wf[1] * 65535.0f, 0.0f), 65535.0f);
                            weights[2] = (uint16_t)fminf(fmaxf(wf[2] * 65535.0f, 0.0f), 65535.0f);
                            weights[3] = (uint16_t)fminf(fmaxf(wf[3] * 65535.0f, 0.0f), 65535.0f);
                        }
                    }

                    skin_vertices[skin_v_cursor + v] =
                        mu_pack_skin_vertex(p[0], p[1], p[2], n[0], n[1], n[2], 1.0f, 0.0f, uv[0], uv[1],
                                            joints[0], joints[1], joints[2], joints[3], weights[0], weights[1],
                                            weights[2], weights[3], 1);

                    bmin[0] = fminf(bmin[0], p[0]);
                    bmin[1] = fminf(bmin[1], p[1]);
                    bmin[2] = fminf(bmin[2], p[2]);

                    bmax[0] = fmaxf(bmax[0], p[0]);
                    bmax[1] = fmaxf(bmax[1], p[1]);
                    bmax[2] = fmaxf(bmax[2], p[2]);
                }

                for(uint32_t i = 0; i < local_idx_count; ++i)
                {
                    skin_indices[skin_i_cursor + i] = skin_v_cursor + (uint32_t)cgltf_accessor_read_index(idx_accessor, i);
                }

                out_mesh->vertex_offset        = skin_v_cursor;
                out_mesh->vertex_count         = local_vert_count;
                out_mesh->lod_count            = 1;
                out_mesh->lods[0].index_offset = skin_i_cursor;
                out_mesh->lods[0].index_count  = local_idx_count;
                out_mesh->lods[0].error        = 0.0f;
                out_draw->vertex_stream        = 1; /* skinned stream */
            }
            else
            {
                for(uint32_t v = 0; v < local_vert_count; ++v)
                {
                    float p[3]  = {0};
                    float n[3]  = {0, 1, 0};
                    float uv[2] = {0};

                    cgltf_accessor_read_float(pos_accessor, v, p, 3);
                    if(nrm_accessor)
                        cgltf_accessor_read_float(nrm_accessor, v, n, 3);
                    if(uv_accessor)
                        cgltf_accessor_read_float(uv_accessor, v, uv, 2);

                    static_vertices[static_v_cursor + v] =
                        mu_pack_vertex(p[0], p[1], p[2], n[0], n[1], n[2], 1.0f, 0.0f, uv[0], uv[1], 1);

                    bmin[0] = fminf(bmin[0], p[0]);
                    bmin[1] = fminf(bmin[1], p[1]);
                    bmin[2] = fminf(bmin[2], p[2]);

                    bmax[0] = fmaxf(bmax[0], p[0]);
                    bmax[1] = fmaxf(bmax[1], p[1]);
                    bmax[2] = fmaxf(bmax[2], p[2]);
                }

                for(uint32_t i = 0; i < local_idx_count; ++i)
                {
                    static_indices[static_i_cursor + i] = static_v_cursor + (uint32_t)cgltf_accessor_read_index(idx_accessor, i);
                }

                out_mesh->vertex_offset        = static_v_cursor;
                out_mesh->vertex_count         = local_vert_count;
                out_mesh->lod_count            = 1;
                out_mesh->lods[0].index_offset = static_i_cursor;
                out_mesh->lods[0].index_count  = local_idx_count;
                out_mesh->lods[0].error        = 0.0f;
                out_draw->vertex_stream        = 0; /* static stream */
            }

            vec3 center;
            glm_vec3_add(bmin, bmax, center);
            glm_vec3_scale(center, 0.5f, center);

            vec3 extent;
            glm_vec3_sub(bmax, center, extent);

            out_mesh->center[0] = center[0];
            out_mesh->center[1] = center[1];
            out_mesh->center[2] = center[2];
            out_mesh->radius    = glm_vec3_norm(extent);

            out_draw->index_count    = local_idx_count;
            out_draw->first_index    = prim_is_skinned ? skin_i_cursor : static_i_cursor;
            out_draw->vertex_offset  = 0;
            out_draw->material_index = prim->material ? (uint32_t)(prim->material - data->materials) : 0;
            out_draw->_pad[0] = out_draw->_pad[1] = out_draw->_pad[2] = 0;

            mesh_cursor++;
            draw_cursor++;

            if(prim_is_skinned)
            {
                skin_v_cursor += local_vert_count;
                skin_i_cursor += local_idx_count;
            }
            else
            {
                static_v_cursor += local_vert_count;
                static_i_cursor += local_idx_count;
            }
        }
    }

    /*
        ------------------------------------------------------------------------
        Upload
        ------------------------------------------------------------------------
    */

    if(has_skinning)
    {
        if(!geometry_gpu_upload(renderer, static_vertices, static_v_cursor * sizeof(PackedVertex), skin_vertices,
                                skin_v_cursor * sizeof(PackedSkinVertex), static_indices,
                                static_i_cursor * sizeof(uint32_t), skin_indices, skin_i_cursor * sizeof(uint32_t),
                                meshes, mesh_cursor * sizeof(Mesh), materials, total_materials * sizeof(Material),
                                palette_mats, total_palette_mats * sizeof(mat4), &out_asset->gpu.gpu))
        {
            goto fail;
        }
    }
    else
    {
        if(!geometry_gpu_upload(renderer, static_vertices, static_v_cursor * sizeof(PackedVertex), NULL, 0, static_indices,
                                static_i_cursor * sizeof(uint32_t), NULL, 0, meshes, mesh_cursor * sizeof(Mesh),
                                materials, total_materials * sizeof(Material), NULL, 0, &out_asset->gpu.gpu))
        {
            goto fail;
        }
    }

    out_asset->gpu.draw_buffer = renderer_upload_draws(renderer, draws, draw_cursor);
    if(!out_asset->gpu.draw_buffer.buffer)
    {
        goto fail;
    }

    out_asset->gpu.draw_offset = (uint32_t)(out_asset->gpu.draw_buffer.offset / sizeof(GpuDraw));
    out_asset->gpu.draw_count  = draw_cursor;

    /*
        ------------------------------------------------------------------------
        Persist runtime asset
        ------------------------------------------------------------------------
    */

    out_asset->content.meshes               = meshes;
    out_asset->content.mesh_count           = mesh_cursor;
    out_asset->content.materials            = materials;
    out_asset->content.material_count       = total_materials;
    out_asset->content.skeleton.skins       = skeleton_skins;
    out_asset->content.skeleton.skin_count  = total_skins;
    out_asset->content.skeleton.joints      = skeleton_joints;
    out_asset->content.skeleton.joint_count = total_joints;
    out_asset->content.anims.clips          = anim_clips;
    out_asset->content.anims.clip_count     = total_clip_count;
    out_asset->content.anims.samplers       = anim_samplers;
    out_asset->content.anims.sampler_count  = total_samplers;
    out_asset->content.anims.channels       = anim_channels;
    out_asset->content.anims.channel_count  = total_channels;
    out_asset->content.nodes                = nodes;
    out_asset->content.node_count           = total_nodes;
    out_asset->content.palette_mats         = palette_mats;
    out_asset->content.palette_count        = total_palette_mats;
    out_asset->content.path_hash            = mu_hash64_mix(mu_fnv1a64_local(desc->path));
    out_asset->gpu.texture_ids              = texture_ids;
    out_asset->gpu.texture_count            = total_textures;

    if(has_skinning)
        out_asset->content.flags |= 1u;
    if(has_animation)
        out_asset->content.flags |= 2u;

    /*
        Scratch dies here.
        Runtime survives.
    */
    cgltf_free(data);
    mu_free(draws);
    mu_free(static_vertices);
    mu_free(skin_vertices);
    mu_free(static_indices);
    mu_free(skin_indices);
    mu_free(skin_palette_offsets);

    return true;

fail:
    asset_destroy_gpu(renderer, &out_asset->gpu);
    cgltf_free(data);
    mu_free(meshes);
    mu_free(materials);
    mu_free(draws);
    mu_free(skeleton_skins);
    mu_free(skeleton_joints);

    if(anim_samplers)
    {
        for(uint32_t i = 0; i < total_samplers; ++i)
        {
            mu_free(anim_samplers[i].input_times);
            mu_free(anim_samplers[i].output_values);
        }
    }

    if(anim_clips)
    {
        for(uint32_t i = 0; i < total_clip_count; ++i)
            mu_free(anim_clips[i].name);
    }
    mu_free(anim_clips);
    mu_free(anim_samplers);
    mu_free(anim_channels);
    mu_free(nodes);
    mu_free(static_vertices);
    mu_free(skin_vertices);
    mu_free(static_indices);
    mu_free(skin_indices);
    mu_free(palette_mats);
    mu_free(skin_palette_offsets);
    if(texture_ids)
    {
        for(uint32_t i = 0; i < total_textures; ++i)
            if(texture_ids[i] != UINT32_MAX && texture_ids[i] != renderer->dummy_texture)
                destroy_texture(renderer, texture_ids[i]);
    }
    mu_free(texture_ids);
    return false;
}

/*
===============================================================================
INTERNAL: Asset cleanup helpers
===============================================================================
*/

static void asset_destroy_content(AssetContent* content)
{
    if(!content)
        return;

    mu_free(content->meshes);
    mu_free(content->materials);
    mu_free(content->skeleton.skins);
    mu_free(content->skeleton.joints);

    if(content->anims.samplers)
    {
        for(uint32_t i = 0; i < content->anims.sampler_count; ++i)
        {
            mu_free(content->anims.samplers[i].input_times);
            mu_free(content->anims.samplers[i].output_values);
        }
    }

    if(content->anims.clips)
    {
        for(uint32_t i = 0; i < content->anims.clip_count; ++i)
            mu_free(content->anims.clips[i].name);
    }

    mu_free(content->anims.clips);
    mu_free(content->anims.samplers);
    mu_free(content->anims.channels);
    mu_free(content->nodes);
    mu_free(content->palette_mats);
    memset(content, 0, sizeof(*content));
}

static void asset_destroy_gpu(Renderer* renderer, AssetGpu* gpu)
{
    (void)renderer;

    if(!gpu)
        return;

    buffer_pool_free(gpu->draw_buffer);
    buffer_pool_free(gpu->gpu.static_vertex_buffer);
    buffer_pool_free(gpu->gpu.skin_input_buffer);
    buffer_pool_free(gpu->gpu.skinned_vertex_buffer);
    buffer_pool_free(gpu->gpu.index_buffer);
    buffer_pool_free(gpu->gpu.mesh_buffer);
    buffer_pool_free(gpu->gpu.material_buffer);
    buffer_pool_free(gpu->gpu.palette_buffer);

    if(gpu->texture_ids)
    {
        for(uint32_t i = 0; i < gpu->texture_count; ++i)
            if(gpu->texture_ids[i] != UINT32_MAX && gpu->texture_ids[i] != renderer->dummy_texture)
                destroy_texture(renderer, gpu->texture_ids[i]);
    }
    mu_free(gpu->texture_ids);

    memset(gpu, 0, sizeof(*gpu));
}

/*
===============================================================================
LAYER 1: ASSET CONTENT API
Public interface for asset loading/unloading
===============================================================================
*/

bool asset_import_model(AssetSystem* assets, Renderer* renderer, const ModelLoadDesc* desc, ModelAssetHandle* out_handle)
{
    MU_ASSERT(assets);
    MU_ASSERT(renderer);
    MU_ASSERT(desc);
    MU_ASSERT(out_handle);

    uint64_t path_hash = mu_hash64_mix(mu_fnv1a64_local(desc->path));
    uint32_t cached_id = 0;

    if(mu_hash32_get(&assets->path_to_assets, path_hash, &cached_id))
    {
        if(mu_bulk_storage_is_live(&assets->models, cached_id))
        {
            ModelAsset* cached_asset = (ModelAsset*)mu_bulk_storage_ptr(&assets->models, cached_id);
            if(cached_asset)
            {
                cached_asset->ref_count++;
                *out_handle = mu_bulk_storage_make_handle(&assets->models, cached_id);
                return true;
            }
        }

        mu_hash32_remove(&assets->path_to_assets, path_hash);
    }

    /* Build asset from glTF file (includes full importer pipeline with animation/skinning) */
    ModelAsset temp_asset = {0};
    if(!asset_build_from_gltf(renderer, desc, &temp_asset))
        return false;

    /* Allocate ID in bulk storage */
    uint32_t id = mu_bulk_storage_alloc(&assets->models);
    if(id == 0) /* ID 0 is invalid */
    {
        /* Clean up asset if allocation failed */
        asset_destroy_gpu(renderer, &temp_asset.gpu);
        asset_destroy_content(&temp_asset.content);
        return false;
    }

    /* Store asset in bulk storage at the allocated ID */
    ModelAsset* stored_asset = (ModelAsset*)mu_bulk_storage_ptr(&assets->models, id);
    *stored_asset            = temp_asset;
    stored_asset->ref_count  = 1;

    mu_hash32_set(&assets->path_to_assets, path_hash, id);

    /* Create weak handle from ID for external use */
    *out_handle = mu_bulk_storage_make_handle(&assets->models, id);

    return true;
}

void asset_release_model(AssetSystem* assets, Renderer* renderer, ModelAssetHandle handle)
{
    MU_ASSERT(assets);
    MU_ASSERT(renderer);

    /* Validate handle against current generation */
    if(!mu_bulk_storage_validate_handle(&assets->models, handle))
        return;

    /* Resolve handle to actual pointer */
    ModelAsset* asset = (ModelAsset*)mu_bulk_storage_resolve_handle(&assets->models, handle);
    if(!asset)
        return;

    /* Decrement reference count */
    if(asset->ref_count > 0)
        asset->ref_count--;

    if(asset->ref_count == 0)
    {
        mu_hash32_remove(&assets->path_to_assets, asset->content.path_hash);

        /* Clean up GPU resources */
        asset_destroy_gpu(renderer, &asset->gpu);

        /* Clean up CPU content */
        asset_destroy_content(&asset->content);

        /* Free the slot */
        mu_bulk_storage_free(&assets->models, handle.id);
    }
}

const ModelAsset* asset_get_model(const AssetSystem* assets, ModelAssetHandle handle)
{
    MU_ASSERT(assets);

    /* Validate and resolve handle */
    if(!mu_bulk_storage_validate_handle(&assets->models, handle))
        return NULL;

    return (const ModelAsset*)mu_bulk_storage_resolve_handle_const(&assets->models, handle);
}


bool asset_system_init(AssetSystem* assets, Renderer* renderer)
{
    (void)renderer;

    memset(assets, 0, sizeof(*assets));

    if(!mu_bulk_storage_init(&assets->models, sizeof(ModelAsset), 128))
        return false;

    if(!mu_hash32_init(&assets->path_to_assets, 256))
    {
        mu_bulk_storage_deinit(&assets->models);
        return false;
    }

    return true;
}
/* ============================================================================
   FRAME STATE BUILD
   CPU transient publish list
   ============================================================================ */

/*
    Frame memory is transient.
    We do not preserve history here.
    We preserve capacity, clear counts, and rebuild truth every frame.

    Scene  = authoritative world
    Frame  = flattened render facts

    Scene says:
        "a building exists"

    Frame says:
        "this building emits 14 draw packets right now"

    That distinction is the whole damn architecture.
*/

/* ============================================================================
   Frame Layer (CPU transient, rebuilt every frame)
   ============================================================================ */
static bool frame_state_init(FrameState* fs)
{
    MU_ASSERT(fs);
    memset(fs, 0, sizeof(*fs));

    array_reserve(fs->instances, 256);
    array_reserve(fs->skin_jobs, 64);

    return fs->instances && fs->skin_jobs;
}

static void frame_state_shutdown(FrameState* fs)
{
    if(!fs)
        return;

    array_free(fs->instances);
    array_free(fs->skin_jobs);
    memset(fs, 0, sizeof(*fs));
}
static void frame_emit_skin_job(Renderer* renderer, FrameState* fs, const RenderInstance* inst, uint32_t instance_id, const ModelAsset* asset)
{
    if(!(inst->flags & RENDER_FLAG_SKINNED))
        return;

    if(!(asset->content.flags & 1u))
        return;

    if(asset->content.skeleton.skin_count == 0)
        return;

    const SkeletonSkin* skin = &asset->content.skeleton.skins[0];

    for(uint32_t i = 0; i < asset->content.mesh_count; ++i)
    {
        const Mesh* mesh = &asset->content.meshes[i];

        SkinJob job = {
            .instance_id      = instance_id,
            .skeleton_state   = inst->skeletal_anim_index,
            .palette_offset   = mesh->padding ? mesh->padding : skin->joint_offset,
            .joint_count      = skin->joint_count,
            .vertex_offset    = mesh->vertex_offset,
            .vertex_count     = mesh->vertex_count,
            .output_offset    = mesh->vertex_offset,
            .skin_input_addr  = slice_device_address(renderer, asset->gpu.gpu.skin_input_buffer),
            .palette_addr     = slice_device_address(renderer, asset->gpu.gpu.palette_buffer),
            .skinned_out_addr = slice_device_address(renderer, asset->gpu.gpu.skinned_vertex_buffer),
        };

        array_push(fs->skin_jobs, job);
    }
}
static MU_INLINE void frame_state_begin(FrameState* fs)
{
    MU_ASSERT(fs);

    /*
        Keep capacity.
        Clear counts.
        Rebuild truth.

        Stretchy arrays make this cheap.
        Miraculously, the thing you wrote for this exact problem
        does in fact solve this exact problem.
    */
    array_clear(fs->instances);
    array_clear(fs->skin_jobs);
}
static void frame_emit_draws(FrameState* fs, const RenderInstance* inst, uint32_t instance_id, const ModelAsset* asset, mat4 model)
{
    for(uint32_t i = 0; i < asset->gpu.draw_count; ++i)
    {
        const Mesh* mesh = &asset->content.meshes[i];

        FrameInstance fi = {0};

        glm_mat4_copy(model, fi.model);

        fi.bounds[0] = mesh->center[0];
        fi.bounds[1] = mesh->center[1];
        fi.bounds[2] = mesh->center[2];
        fi.bounds[3] = mesh->radius;

        fi.draw_index  = asset->gpu.draw_offset + i;
        fi.instance_id = instance_id;
        fi.flags       = inst->flags;
        fi.lod_bias    = 0;

        array_push(fs->instances, fi);
    }
}
void rendering_system_build_frame(Renderer* renderer, RenderingSystem* rs, const Scene* scene, const AssetSystem* assets, const AnimationSystem* anims)
{
    (void)anims;

    FrameState* fs = &rs->frame;

    const uint32_t  live_count = scene->state.active_instances.count;
    const uint32_t* live_ids   = scene->state.active_instances.dense;

    for(uint32_t n = 0; n < live_count; ++n)
    {
        uint32_t instance_id = live_ids[n];

        const RenderInstance* inst =
            (const RenderInstance*)mu_bulk_storage_ptr((mu_bulk_storage*)&scene->state.instances, instance_id);

        if(!inst || !(inst->flags & RENDER_FLAG_VISIBLE))
            continue;

        const ModelAsset* asset = asset_get_model(assets, inst->asset);
        if(!asset)
            continue;

        mat4 model;
        build_model_matrix((RenderInstance*)inst, model);

        frame_emit_skin_job(renderer, fs, inst, instance_id, asset);
        frame_emit_draws(fs, inst, instance_id, asset, model);
    }
}


/*
===============================================================================
GPU-side packed instance record
Flattened version of FrameInstance for GPU consumption.

CPU FrameInstance is comfortable for CPU code.
GPU wants tighter, flatter, less emotionally complicated data.
===============================================================================
*/
typedef struct GpuInstance
{
    mat4 model;
    vec4 bounds;

    uint32_t draw_index;
    uint32_t instance_id;
    uint32_t flags;
    uint32_t lod_bias;
} GpuInstance;

static void frame_build_gpu_instances(const FrameState* fs, GpuInstance** out_instances)
{
    MU_ASSERT(fs);
    MU_ASSERT(out_instances);

    *out_instances = NULL;

    const uint32_t count = array_size(fs->instances);
    if(count == 0)
        return;

    array_reserve(*out_instances, count);

    for(uint32_t i = 0; i < count; ++i)
    {
        const FrameInstance* src = &fs->instances[i];

        GpuInstance dst = {0};
memcpy(dst.model,  src->model,  sizeof(mat4));
memcpy(dst.bounds, src->bounds, sizeof(vec4));
        dst.draw_index  = src->draw_index;
        dst.instance_id = src->instance_id;
        dst.flags       = src->flags;
        dst.lod_bias    = src->lod_bias;

        array_push(*out_instances, dst);
    }
}

static bool gpu_scene_upload_slice(Renderer* renderer, VkCommandBuffer cmd, BufferSlice* out, const void* data, size_t bytes, VkDeviceSize alignment)
{
    MU_ASSERT(renderer);
    MU_ASSERT(out);

    memset(out, 0, sizeof(*out));

    if(bytes == 0)
        return true;

    *out = buffer_pool_alloc(&renderer->gpu_pool, bytes, alignment);
    if(!out->buffer)
        return false;

    if(data && !renderer_upload_buffer_to_slice(renderer, cmd, *out, data, bytes, alignment))
    {
        buffer_pool_free(*out);
        memset(out, 0, sizeof(*out));
        return false;
    }

    return true;
}

static void gpu_scene_release(GpuScene* gpu)
{
    if(!gpu)
        return;

    buffer_pool_free(gpu->instance_buffer);
    buffer_pool_free(gpu->skin_job_buffer);
    buffer_pool_free(gpu->indirect_cmd_buffer);
    buffer_pool_free(gpu->indirect_count_buffer);

    memset(gpu, 0, sizeof(*gpu));
}

bool rendering_system_upload_frame(RenderingSystem* rs, Renderer* renderer)
{
    MU_ASSERT(rs);
    MU_ASSERT(renderer);

    FrameState* fs = &rs->frame;
    GpuScene*   gs = &rs->gpu;

    gpu_scene_release(gs);

    /*
        CPU FrameInstance is fine for CPU.
        GPU gets a tightly packed mirror.
    */
    GpuInstance* gpu_instances = NULL;
    frame_build_gpu_instances(fs, &gpu_instances);

    const uint32_t instance_count = array_size(gpu_instances);
    const uint32_t skin_job_count = array_size(fs->skin_jobs);

    /*
        Worst-case indirect count:
        one indirect draw per visible frame packet.
        Cull pass compacts this later.
    */
    const uint32_t max_indirect_count = instance_count;

    uint32_t indirect_zero = 0;

    VkCommandBuffer cmd = vk_begin_one_time_cmd(renderer->device, renderer->one_time_gfx_pool);
    if(!cmd)
    {

        array_free(gpu_instances);
        return false;
    }

    bool ok = true;

    ok &= gpu_scene_upload_slice(renderer, cmd, &gs->instance_buffer, gpu_instances,
                                 (size_t)instance_count * sizeof(GpuInstance), 16);

    ok &= gpu_scene_upload_slice(renderer, cmd, &gs->skin_job_buffer, fs->skin_jobs, (size_t)skin_job_count * sizeof(SkinJob), 16);

    ok &= gpu_scene_upload_slice(renderer, cmd, &gs->indirect_cmd_buffer, NULL,
                                 (size_t)max_indirect_count * sizeof(VkDrawIndirectCommand), 16);

    ok &= gpu_scene_upload_slice(renderer, cmd, &gs->indirect_count_buffer, &indirect_zero, sizeof(uint32_t), 4);

    vk_end_one_time_cmd(renderer->device, renderer->graphics_queue, renderer->one_time_gfx_pool, cmd);

    array_free(gpu_instances);

    if(!ok)
    {

debug_break();
        gpu_scene_release(gs);
        return false;
    }

    return true;
}


PUSH_CONSTANT(SkinningPC, VkDeviceAddress skin_jobs_addr; uint32_t skin_job_count; uint32_t _pad0; uint32_t _pad1; uint32_t _pad2;);
PUSH_CONSTANT(MeshPC,
              VkDeviceAddress static_vertex_addr;
              VkDeviceAddress skinned_vertex_addr;
              VkDeviceAddress index_addr;
              VkDeviceAddress draw_addr;
              VkDeviceAddress material_addr;
              VkDeviceAddress instance_addr;
              VkDeviceAddress indirect_cmd_addr;
              mat4            view_proj;
              uint32_t        sampler_id;
              uint32_t        draw_base_index;
              uint32_t        indirect_cmd_count;
              uint32_t        _paGGGd1;);
static VkDeviceAddress slice_device_address_or_zero(const Renderer* renderer, BufferSlice slice)
{
    if(!renderer || !slice.buffer)
        return 0;

    return slice_device_address(renderer, slice);
}

static void model_asset_upload_palette(Renderer* renderer, ModelAsset* model, VkCommandBuffer cmd)
{
    if(!renderer || !model || !cmd || !model->content.palette_mats || model->content.palette_count == 0)
        return;

    VkDeviceSize bytes = (VkDeviceSize)model->content.palette_count * sizeof(mat4);
    if(!renderer_upload_buffer_to_slice(renderer, cmd, model->gpu.gpu.palette_buffer, model->content.palette_mats, bytes, 16))
        return;

    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };

    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &barrier,
    };

    vkCmdPipelineBarrier2(cmd, &dep);
}

void rendering_system_run_skinning(RenderingSystem* rs, Renderer* renderer, VkCommandBuffer cmd)
{
    MU_ASSERT(rs);
    MU_ASSERT(renderer);
    MU_ASSERT(cmd);

    const uint32_t skin_job_count = array_size(rs->frame.skin_jobs);
    if(skin_job_count == 0)
        return;

    GpuScene* gs = &rs->gpu;

    SkinningPC pc     = {0};
    pc.skin_jobs_addr = slice_device_address(renderer, gs->skin_job_buffer);

    pc.skin_job_count = skin_job_count;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_get(pipelines.skinning));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer->bindless_system.pipeline_layout, 0, 1,

                            &renderer->bindless_system.set, 0, NULL);

    vkCmdPushConstants(cmd, renderer->bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(SkinningPC), &pc);
    uint32_t max_chunks = 0;

    for(uint32_t i = 0; i < skin_job_count; ++i)
    {
        const SkinJob* job    = &rs->frame.skin_jobs[i];
        uint32_t       chunks = (job->vertex_count + 63u) / 64u;
        if(chunks > max_chunks)
            max_chunks = chunks;
    }

    vkCmdDispatch(cmd, skin_job_count, max_chunks, 1);

    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
    };

    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &barrier,
    };

    vkCmdPipelineBarrier2(cmd, &dep);
}

/*
    Build VkDrawIndirectCommand array for multidraw indirect rendering.
    Each command corresponds to one frame instance with its mesh LOD info.
    
    This function:
    1. Iterates through frame instances
    2. Looks up mesh LOD data from the model
    3. Creates a VkDrawIndirectCommand for each valid instance
    4. Returns the populated command array via output parameter
*/
static void build_indirect_commands(
    const RenderingSystem* rs,
    const ModelAsset*      model,
    VkDrawIndirectCommand** out_commands)
{
    MU_ASSERT(rs);
    MU_ASSERT(model);
    MU_ASSERT(out_commands);

    *out_commands = NULL;

    const uint32_t instance_count = array_size(rs->frame.instances);
    if(instance_count == 0)
        return;

    /* Pre-allocate for all instances */
    array_reserve(*out_commands, instance_count);

    /* Build command for each instance */
    for(uint32_t i = 0; i < instance_count; ++i)
    {
        const FrameInstance* fi = &rs->frame.instances[i];
        const uint32_t       global_draw_index = fi->draw_index;
        const uint32_t       local_mesh_index = global_draw_index - model->gpu.draw_offset;

        /* Validate mesh_index is within model bounds */
        if(local_mesh_index >= model->content.mesh_count)
            continue;

        const Mesh* mesh = &model->content.meshes[local_mesh_index];

        VkDrawIndirectCommand cmd = {
            .vertexCount   = mesh->lods[0].index_count,
            .instanceCount = 1,
            .firstVertex   = 0,
            .firstInstance = i,  /* GPU shader uses this to index frame instances */
        };

        array_push(*out_commands, cmd);
    }
}

static bool rendering_system_prepare_indirect_draws(Renderer* renderer, RenderingSystem* rs, const ModelAsset* model, VkCommandBuffer cmd)
{
    MU_ASSERT(renderer);
    MU_ASSERT(rs);
    MU_ASSERT(model);
    MU_ASSERT(cmd);

    GpuScene* gs = &rs->gpu;
    gs->indirect_cmd_count = 0;

    VkDrawIndirectCommand* indirect_commands = NULL;
    build_indirect_commands(rs, model, &indirect_commands);

    const uint32_t cmd_count = array_size(indirect_commands);
    gs->indirect_cmd_count   = cmd_count;

    if(cmd_count == 0)
    {
        array_free(indirect_commands);
        return true;
    }

    VkDeviceSize cmd_bytes = (VkDeviceSize)cmd_count * sizeof(VkDrawIndirectCommand);
    bool         ok        = renderer_upload_buffer_to_slice(renderer, cmd, gs->indirect_cmd_buffer, indirect_commands, cmd_bytes, 16);

    array_free(indirect_commands);
    return ok;
}


void render_frame(Renderer* renderer, RenderingSystem* rs, const ModelAsset* model, const Camera* cam, VkCommandBuffer cmd)
{
    if(!renderer || !rs || !model || !cam || !cmd)
        return;

    if(rs->gpu.indirect_cmd_count == 0)
        return;

    /* Bind pipeline and push constants for the already-prepared indirect commands. */
    MeshPC pc               = {0};
    pc.static_vertex_addr   = slice_device_address_or_zero(renderer, model->gpu.gpu.static_vertex_buffer);
    pc.skinned_vertex_addr  = slice_device_address_or_zero(renderer, model->gpu.gpu.skinned_vertex_buffer);
    pc.index_addr           = slice_device_address_or_zero(renderer, model->gpu.gpu.index_buffer);
    pc.draw_addr            = slice_device_address_or_zero(renderer, model->gpu.draw_buffer);
    pc.material_addr        = slice_device_address_or_zero(renderer, model->gpu.gpu.material_buffer);
    pc.instance_addr        = slice_device_address_or_zero(renderer, rs->gpu.instance_buffer);
    pc.indirect_cmd_addr    = slice_device_address_or_zero(renderer, rs->gpu.indirect_cmd_buffer);
    pc.sampler_id           = renderer->default_samplers.samplers[SAMPLER_LINEAR_WRAP];
    pc.draw_base_index      = model->gpu.draw_offset;
    pc.indirect_cmd_count   = rs->gpu.indirect_cmd_count;
    memcpy(pc.view_proj, cam->view_proj, sizeof(mat4));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_get(pipelines.gltf_minimal));
    vk_cmd_set_viewport_scissor(cmd, renderer->swapchain.extent);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->bindless_system.pipeline_layout, 0, 1,
                            &renderer->bindless_system.set, 0, NULL);
    vkCmdPushConstants(cmd, renderer->bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(MeshPC), &pc);

    vkCmdDrawIndirect(cmd, rs->gpu.indirect_cmd_buffer.buffer, rs->gpu.indirect_cmd_buffer.offset,
                      rs->gpu.indirect_cmd_count, sizeof(VkDrawIndirectCommand));
}
int main(int argc, char** argv)
{
    const char* model_path          = "/home/lk/myprojects/tinyglade/meshes/brick.glb";
    for(int i = 1; i < argc; ++i)
    {
        if(strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            model_path = argv[++i];
        else if(strcmp(argv[i], "--list-animations") == 0)
            return print_gltf_animation_names(model_path) ? 0 : 1;
    }

    graphics_init();

    AssetSystem assets;
    asset_system_init(&assets, &renderer);

    ModelAssetHandle godottestmodel = {0};

    ModelLoadDesc desc = {
        .path           = model_path,
        .build_lods     = false,
        .allow_skinning = true,
    };

    asset_import_model(&assets, &renderer, &desc, &godottestmodel);
    ModelAsset* model = (ModelAsset*)asset_get_model(&assets, godottestmodel);
    if(!model)
        return 1;

    printf("meshes: %u\n", model->content.mesh_count);
    printf("materials: %u\n", model->content.material_count);

    printf("skin: %u\n", model->content.skeleton.skin_count);
    printf("draws: %u\n", model->gpu.draw_count);
    printf("flags: %u\n", model->content.flags);


    model_asset_print_animations(model);
   

    uint32_t selected_animation = 0;
    bool     run_animation      = model->content.anims.clip_count > 0;
    const char* env_anim = getenv("FUKUNA_ANIM");


    if(env_anim && env_anim[0])
        selected_animation = (uint32_t)atoi(env_anim);

    for(int i = 1; i < argc; ++i)
    {
        if(strcmp(argv[i], "--list-animations") == 0)
        {
            renderer_destroy(&renderer);
            return 0;
        }
        else if(strcmp(argv[i], "--model") == 0 && i + 1 < argc)
        {
            ++i;
        }
        else if(strcmp(argv[i], "--anim") == 0 && i + 1 < argc)
        {
            selected_animation = (uint32_t)atoi(argv[++i]);
        }
        else if(strcmp(argv[i], "--no-anim") == 0)
        {
            run_animation = false;
        }
    }

    if(selected_animation >= model->content.anims.clip_count)
    {
        printf("selected animation %u is out of range, using 0\n", selected_animation);
        selected_animation = 0;
    }

    if(run_animation && model->content.anims.clip_count > 0)
    {
        const AnimationClip* clip = &model->content.anims.clips[selected_animation];
        printf("running animation [%u] %s\n", selected_animation, clip->name && clip->name[0] ? clip->name : "(unnamed)");
    }



    Scene scene = {0};


    scene_init(&scene);

    RenderingSystem rs = {0};


    frame_state_init(&rs.frame);

    RenderInstanceHandle robot = scene_spawn(&scene, godottestmodel);

    RenderInstance*      inst  = scene_write(&scene, robot);




    //  inst->flags |= RENDER_FLAG_SKINNED;
    inst->scale       = 1.0f;
    inst->position[1] = 0.0f;
    Camera cam        = {0};
    camera_defaults_3d(&cam);
    camera3d_set_position(&cam, 0.0f, 0.6f, 4.0f);
    camera3d_set_rotation_yaw_pitch(&cam, 0.0f, 0.0f);


    while(!glfwWindowShouldClose(renderer.window))
    {
        TracyCFrameMark;
        pipeline_rebuild(&renderer);
        frame_start(&renderer, &cam);


        VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
        GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];

        vk_cmd_begin(cmd, false);
        gpu_profiler_begin_frame(frame_prof, cmd);
        {
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.bindless_system.pipeline_layout,
                                        0, 1, &renderer.bindless_system.set, 0, NULL);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.bindless_system.pipeline_layout,
                                        0, 1, &renderer.bindless_system.set, 0, NULL);

                rt_transition_all(cmd, &renderer.depth[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

                rt_transition_all(cmd, &renderer.hdr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

                image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_GENERAL,
                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                flush_barriers(cmd);
            }
            frame_state_begin(&rs.frame);

            if(model->content.palette_count > 0)
            {
                uint32_t clip_to_run = (run_animation && model->content.anims.clip_count > 0) ? selected_animation : UINT32_MAX;
                model_asset_evaluate_animation(model, clip_to_run, (float)glfwGetTime());
                model_asset_upload_palette(&renderer, model, cmd);
            }

            rendering_system_build_frame(&renderer, &rs, &scene, &assets, NULL);

            if(!rendering_system_upload_frame(&rs, &renderer))
            {
                debug_break();
            }

            if(!rendering_system_prepare_indirect_draws(&renderer, &rs, model, cmd))
            {
                debug_break();
            }
            VkRenderingAttachmentInfo color = {
                .sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView        = renderer.hdr_color[renderer.swapchain.current_image].view,
                .imageLayout      = renderer.hdr_color[renderer.swapchain.current_image].mip_states[0].layout,
                .loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp          = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue.color = {{0.10f, 0.12f, 0.15f, 1.0f}},
            };

            VkRenderingAttachmentInfo depth = {
                .sType                   = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView               = renderer.depth[renderer.swapchain.current_image].view,
                .imageLayout             = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .loadOp                  = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp                 = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue.depthStencil = {0.0f, 0},
            };

            VkRenderingInfo rendering = {
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea.extent    = renderer.swapchain.extent,
                .layerCount           = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments    = &color,
                .pDepthAttachment     = &depth,
            };

            rendering_system_run_skinning(&rs, &renderer, cmd);
            vkCmdBeginRendering(cmd, &rendering);
            render_frame(&renderer, &rs, model, &cam, cmd);
            vkCmdEndRendering(cmd);

            post_pass();
            pass_smaa();
            pass_ldr_to_swapchain();

            image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
            flush_barriers(cmd);
        }
        vk_cmd_end(cmd);

        submit_frame(&renderer);
    }

    renderer_destroy(&renderer);
    return 0;
}
