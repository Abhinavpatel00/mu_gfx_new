#ifndef MU_GFX_VK_H
#define MU_GFX_VK_H

#include "src/helpers.h"
#include "external/mu/offset_allocator.h"
#include "external/mu/mu/mu_span.h"

typedef uint32_t TextureID;
typedef uint32_t SamplerID;
typedef uint32_t PipelineID;
typedef struct Texture {
    VkImage       image;
    VkImageView   view;
    VmaAllocation allocation;
} Texture;

typedef struct TextureInfo {
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;
    VkFormat format;

} TextureInfo;
typedef struct TextureCreateDesc {
    uint32_t width;
    uint32_t height;

    uint32_t depth;  // only relevant for 3D
    uint32_t layers; // only relevant for arrays/cubes
    uint32_t mip_count;

    VkFormat           format;
    VkImageUsageFlags  usage;
    VkImageCreateFlags flags;

    const char *debug_name;

} TextureCreateDesc;
typedef struct TextureSystem {
    Texture     textures[MAX_BINDLESS_TEXTURES];
    TextureInfo info[MAX_BINDLESS_TEXTURES];

    mu_id_pool id_pool;
} TextureSystem;
typedef struct {
    VkPhysicalDevice physical_device;
    // warm data
    VkDevice device;

    VkQueue present_queue;
    VkQueue graphics_queue;
    VkQueue compute_queue;
    VkQueue transfer_queue;

    uint32_t        present_queue_index;
    uint32_t        graphics_queue_index;
    uint32_t        compute_queue_index;
    uint32_t        transfer_queue_index;
    VmaAllocator    vmaallocator;
    VkPipelineCache pipeline_cache;
} DeviceContext;

typedef struct {
    VkInstance               instance;
    VkDebugUtilsMessengerEXT debug_messenger;
} InstanceContext;

// Touched rarely.

typedef struct VkFeatureChain {
    VkPhysicalDeviceFeatures2 core;

    VkPhysicalDeviceVulkan11Features v11;
    VkPhysicalDeviceVulkan12Features v12;
    VkPhysicalDeviceVulkan13Features v13;

    // ---- add this ----
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5;
    //    VkPhysicalDeviceShaderNonSemanticInfoFeaturesKHR shaderNonSemanticInfo;
} VkFeatureChain;

typedef struct {

    uint32_t width;
    uint32_t height;

    const char  *app_name;
    const char **instance_layers;
    const char **instance_extensions;
    const char **device_extensions;

    uint32_t instance_layer_count;
    uint32_t instance_extension_count;
    uint32_t device_extension_count;
    bool     enable_validation;
    bool     enable_gpu_based_validation;

    VkDebugUtilsMessageSeverityFlagsEXT validation_severity;
    VkDebugUtilsMessageTypeFlagsEXT     validation_types;
    bool                                use_custom_features;
    VkFeatureChain                      custom_features;
    VkPresentModeKHR                    swapchain_preferred_present_mode;
    VkFormat                            swapchain_preferred_format;

    VkColorSpaceKHR swapchain_preferred_color_space;

    VkImageUsageFlags swapchain_extra_usage_flags; /* Additional usage flags */
    bool              vsync;
    bool              enable_debug_printf; /* Enable VK_KHR_shader_non_semantic_info for shader
                                              debug printf */
    uint32_t bindless_sampled_image_count;
    uint32_t bindless_sampler_count;
    uint32_t bindless_storage_image_count;
    bool     enable_pipeline_stats;
    bool     enable_graphics_profiler;

    VkDeviceSize size_of_cpu_pool;

    VkDeviceSize size_of_gpu_pool;

    VkDeviceSize size_of_staging_pool;
} VkBackendDesc;

typedef enum ImageStateValidity {
    IMAGE_STATE_UNDEFINED = 0,
    IMAGE_STATE_VALID     = 1,
    IMAGE_STATE_EXTERNAL  = 2,
} ImageStateValidity;

typedef struct ALIGNAS(32) ImageState {
    VkPipelineStageFlags2 stage;        // 8
    VkAccessFlags2        access;       // 8
    VkImageLayout         layout;       // 4
    uint32_t              queue_family; // 4
    ImageStateValidity    validity;     // 4
    uint32_t              dirty_mips;   // 4
} ImageState;

typedef struct ALIGNAS(64) FlowSwapchain {
    // hot
    ImageState states[MAX_SWAPCHAIN_IMAGES];
    VkImage    images[MAX_SWAPCHAIN_IMAGES];
    uint32_t   current_image;

    VkImageView image_views[MAX_SWAPCHAIN_IMAGES];
    VkSemaphore render_finished[MAX_SWAPCHAIN_IMAGES];
    TextureID   bindless_index[MAX_SWAPCHAIN_IMAGES];

    // cold
    VkSwapchainKHR   swapchain;
    VkSurfaceKHR     surface;
    VkFormat         format;
    VkColorSpaceKHR  color_space;
    VkPresentModeKHR present_mode;
    VkExtent2D       extent;
    uint32_t         image_count;

    VkImageUsageFlags image_usage;

    bool vsync;
    bool needs_recreate;
} FlowSwapchain;

typedef struct FlowSwapchainCreateInfo {
    VkSurfaceKHR      surface;
    uint32_t          width;
    uint32_t          height;
    uint32_t          min_image_count;
    VkPresentModeKHR  preferred_present_mode;
    VkFormat          preferred_format;
    VkColorSpaceKHR   preferred_color_space; /* VK_COLOR_SPACE_SRGB_NONLINEAR_KHR default */
    VkImageUsageFlags extra_usage;           /* Additional usage flags */
    VkSwapchainKHR    old_swapchain;         /* For recreation */
} FlowSwapchainCreateInfo;

typedef enum SwapchainResult {
    SWAPCHAIN_OK,
    SWAPCHAIN_SUBOPTIMAL,
    SWAPCHAIN_OUT_OF_DATE,
} SwapchainResult;


typedef struct {
    VkPhysicalDevice physical;

    VkPhysicalDeviceProperties       properties;
    VkPhysicalDeviceFeatures         features;
    VkPhysicalDeviceMemoryProperties memory;

    VkFeatureChain feature_chain;

} DeviceInfo;

typedef struct {
    VkCommandBuffer cmdbuf;
    VkCommandPool   cmdbufpool;
    VkSemaphore     image_available_semaphore;
    uint64_t        timeline_value;  // timeline value signalled by this slot's last submission
    uint32_t        staging_tail;
} FrameContext;

// ---- Deferred destruction keyed on the submission timeline ----
//
// Resources destroyed while the GPU may still use them are retired with the
// timeline value of the submission that last referenced them. tick() runs the
// callbacks whose retire value is covered; drain() runs everything at shutdown.
typedef struct VkBackend VkBackend;

typedef void (*DeferredDestroyFn)(VkBackend *r, void *user);



typedef struct DeleteQueueEntry {
    uint64_t          retire_value;
    DeferredDestroyFn fn;
    void             *user;
} DeleteQueueEntry;

#define DELETE_QUEUE_CAPACITY 256

typedef struct DeleteQueue {
    DeleteQueueEntry entries[DELETE_QUEUE_CAPACITY];
    uint32_t         head;
    uint32_t         tail;
    uint32_t         count;
} DeleteQueue;

typedef struct Bindless {
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;

    VkPipelineLayout pipeline_layout;

} Bindless;

// Zero-value defaults: layers 0 -> 1, aspect 0 = inferred from format, mip_count
// 0 = auto-computed from extent (1 = no mips). width/height/format/usage must be
// set; call sites name only deltas.
typedef struct RenderTargetSpec {
    uint32_t           width;
    uint32_t           height;
    uint32_t           layers;    // 0 = single layer
    VkFormat           format;
    VkImageUsageFlags  usage;     // required, asserted non-zero
    VkImageAspectFlags aspect;    // 0 = infer from format
    uint32_t           mip_count; // 0 = auto-compute, 1 = no mips
    const char        *debug_name;
} RenderTargetSpec;

typedef struct RenderTarget {
    VkImage       image;
    VmaAllocation allocation;

    VkImageView view;
    VkImageView mip_views[RT_MAX_MIPS];

    VkFormat format;

    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t mip_count;

    VkImageUsageFlags  usage;
    VkImageAspectFlags aspect;

    ImageState mip_states[RT_MAX_MIPS];

    uint32_t bindless_index;

    char debug_name[64];

} RenderTarget;

typedef struct Buffer {
    VkBuffer        buffer;
    VkDeviceSize    buffer_size;
    VkDeviceAddress address;
    uint8_t        *mapping;
    VmaAllocation   allocation;
} Buffer;

typedef enum DefaultSamplerID {
    SAMPLER_LINEAR_WRAP = 0,
    SAMPLER_LINEAR_CLAMP,
    SAMPLER_NEAREST_WRAP,
    SAMPLER_NEAREST_CLAMP,
    SAMPLER_LINEAR_WRAP_ANISO,
    SAMPLER_SHADOW,
    SAMPLER_COUNT
} DefaultSamplerID;
typedef struct DefaultSamplerTable {
    SamplerID samplers[SAMPLER_COUNT];
} DefaultSamplerTable;

typedef enum BufferPoolType { BUFFER_POOL_LINEAR, BUFFER_POOL_RING, BUFFER_POOL_TLSF } BufferPoolType;

typedef struct BufferPool {
    VkBuffer      buffer;
    VmaAllocation allocation;
    VkDeviceSize  size_bytes;

    void *mapped;

    BufferPoolType type;
    union {
        mu_linear_allocator linear;
        mu_ring_allocator   ring;
        OA_Allocator        tlsf;
    };

    // config
    VkBufferUsageFlags       usage;
    VmaMemoryUsage           memory_usage;
    VmaAllocationCreateFlags alloc_flags;
} BufferPool;

typedef enum PipelineType { PIPELINE_TYPE_GRAPHICS, PIPELINE_TYPE_COMPUTE } PipelineType;

typedef struct ColorAttachmentBlend {
    bool blend_enable;

    VkBlendFactor src_color;
    VkBlendFactor dst_color;
    VkBlendOp     color_op;

    VkBlendFactor src_alpha;
    VkBlendFactor dst_alpha;
    VkBlendOp     alpha_op;

    VkColorComponentFlags write_mask;

} ColorAttachmentBlend;

typedef struct GraphicsPipelineConfig {
    // Rasterization
    //
    const char     *vert_path; // NULL = invalid, must be set
    const char     *frag_path; // NULL = invalid, must be set
    VkCullModeFlags cull_mode;
    VkFrontFace     front_face;
    VkPolygonMode   polygon_mode;

    VkPrimitiveTopology topology;

    bool        depth_test_enable;
    bool        depth_write_enable;
    VkCompareOp depth_compare_op;

    uint32_t        color_attachment_count; // 0 = no color attachments
    const VkFormat *color_formats;         // NULL when color_attachment_count == 0
    VkFormat        depth_format;           // 0 (VK_FORMAT_UNDEFINED) = no depth attachment

    // Per-attachment blend state. Unset entries (zeroed write mask) default to
    // no blending; opt in with blend_alpha() in the initializer.
    ColorAttachmentBlend blends[MAX_COLOR_ATTACHMENTS];

} GraphicsPipelineConfig;

typedef struct PipelineEntry {
    PipelineType type;

    union {
        GraphicsPipelineConfig graphics;

        struct {
            const char *path;
        } compute;
    };

    bool dirty;

} PipelineEntry;

typedef struct VkBackendPipelines {
    VkPipeline    pipelines[MAX_PIPELINES];
    PipelineEntry entries[MAX_PIPELINES];
    uint32_t      count;
    mu_id_pool    pipeline_id_pool;
} VkBackendPipelines;

typedef struct BarrierBatch {
    VkImageMemoryBarrier2 image_barriers[64];

    uint32_t image_count;
    // Number of barriers silently dropped when the batch was full. Flushed as
    // an error counter once per flush so API misuse surfaces as a loud,
    // once-per-frame log instead of a corrupt stack.
    uint32_t overflow_count;
} BarrierBatch;

// ---- Pass API ----
//
// One call declares a pass: the backend resolves targets, derives transitions
// from the ImageState tracker, flushes barriers, fills the Vulkan rendering
// structs, sets viewport/scissor, and binds the PSO. Call sites name intent,
// never layouts or stage masks.
//
typedef enum LoadOp {
    LOAD_KEEP    = VK_ATTACHMENT_LOAD_OP_LOAD,
    LOAD_CLEAR   = VK_ATTACHMENT_LOAD_OP_CLEAR,
    LOAD_DISCARD = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
} LoadOp;

typedef enum StoreOp {
    STORE_KEEP    = VK_ATTACHMENT_STORE_OP_STORE,
    STORE_DISCARD = VK_ATTACHMENT_STORE_OP_DONT_CARE,
} StoreOp;

// Zero-value defaults: load = LOAD_KEEP, store = STORE_KEEP, clear = {0,0,0,0}.
// DISCARD makes contents irrelevant; it does not clear them or mean Vulkan NONE.
// Set `swapchain_view` instead of `target` to render into a swapchain image;
// the backend tracks swapchain state, so those attachments skip rt transitions.
typedef struct PassAttachment {
    RenderTarget *target;
    VkImageView   swapchain_view; // used when target == NULL
    LoadOp        load;
    StoreOp       store;
    float         clear[4]; // color rgba; depth clear value in clear[0]
} PassAttachment;

typedef struct PassDesc {
    const PassAttachment *colors;            // NULL when compute-only
    uint32_t              color_count;       // 0..MAX_COLOR_ATTACHMENTS
    const PassAttachment *depth;             // NULL = no depth attachment
    RenderTarget *const  *shader_reads;      // sampled reads (sampled-read layout)
    uint32_t              shader_read_count;
    RenderTarget *const  *shader_writes;     // storage image writes (GENERAL layout)
    uint32_t              shader_write_count;
    PipelineID            pipeline;          // 1-based; 0 = caller binds later (e.g. Nuklear)
} PassDesc;


typedef struct BufferSlice {
    BufferPool   *pool;
    VkBuffer      buffer;
    VkDeviceSize  offset;
    VkDeviceSize  size;
    void         *mapped;
    OA_Allocation allocation;
} BufferSlice;
typedef struct SamplerDesc {
    VkFilter   min_filter;
    VkFilter   mag_filter;
    bool       nearest_mips;
    VkSamplerAddressMode address_u;
    VkSamplerAddressMode address_v;
    VkSamplerAddressMode address_w;
    bool       anisotropic; // 16x; only meaningful with linear filtering
    bool       compare_enabled;
    VkCompareOp compare;
    VkSamplerAddressMode clamp_mode_override; // 0 = none; shadow presets use clamp-to-border
    VkBorderColor border_color;
} SamplerDesc;

struct VkBackend {
    uint32_t current_frame;
    FrameContext frames[MAX_FRAMES_IN_FLIGHT];
    FlowSwapchain swapchain;
    VkCommandPool one_time_gfx_pool;
    VkCommandPool transfer_pool;
    InstanceContext instance;
    DeviceContext   devc;
    VkSurfaceKHR           surface;
    VkAllocationCallbacks *vk_allocator_callbacks;
    DeviceInfo             info;
    TextureSystem texture_system;
    Bindless         bindless_system;
    mu_id_pool       sampler_pool;
    GpuProfiler gpuprofiler[MAX_FRAMES_IN_FLIGHT];
    bool        enable_graphics_profiler;
    DefaultSamplerTable default_samplers;
    BufferPool cpu_pool;
    BufferPool gpu_pool;
    BufferPool staging_pool;
    VkBackendPipelines render_pipelines;
    DeleteQueue delete_queue;
    VkDeviceAddress   gpu_base_addr;
    VkSampler         samplers[MAX_BINDLESS_SAMPLERS];
    BarrierBatch barrierbatch;
    VkSemaphore timeline;
    uint64_t    timeline_last_submitted;
};

static inline ColorAttachmentBlend blend_disabled(void) {
    return (ColorAttachmentBlend){
        .src_color = VK_BLEND_FACTOR_ONE,
        .dst_color = VK_BLEND_FACTOR_ZERO,
        .color_op  = VK_BLEND_OP_ADD,
        .src_alpha = VK_BLEND_FACTOR_ONE,
        .dst_alpha = VK_BLEND_FACTOR_ZERO,
        .alpha_op  = VK_BLEND_OP_ADD,
        .write_mask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
}

static inline GraphicsPipelineConfig pipeline_config_default(void) {
    return (GraphicsPipelineConfig){
        .cull_mode          = VK_CULL_MODE_NONE,
        .front_face         = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .polygon_mode       = VK_POLYGON_MODE_FILL,
        .topology           = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .depth_test_enable  = true,
        .depth_write_enable = true,
        .depth_compare_op   = VK_COMPARE_OP_GREATER,
        .color_attachment_count = 0,
        .depth_format       = VK_FORMAT_UNDEFINED,
    };
}

static inline GraphicsPipelineConfig pipeline_config_fullscreen(void) {
    GraphicsPipelineConfig cfg = pipeline_config_default();
    cfg.depth_test_enable      = false;
    cfg.depth_write_enable     = false;
    cfg.color_attachment_count = 1;
    return cfg;
}

static inline ColorAttachmentBlend blend_alpha(void) {
    return (ColorAttachmentBlend){
        .blend_enable = true,
        .src_color    = VK_BLEND_FACTOR_SRC_ALPHA,
        .dst_color    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .color_op     = VK_BLEND_OP_ADD,
        .src_alpha    = VK_BLEND_FACTOR_ONE,
        .dst_alpha    = VK_BLEND_FACTOR_ZERO,
        .alpha_op     = VK_BLEND_OP_ADD,
        .write_mask   = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                      VK_COLOR_COMPONENT_A_BIT,
    };
}

// Destruction is immediate. Wait for final GPU use (or defer via DeleteQueue).
// At shutdown wait idle, drain delete queues, then destroy resources and backend.
// Create the instance, then create/set the platform surface before the backend.
void vk_instance_create(VkBackend *r, VkBackendDesc *desc);
// Destroy the platform surface after the backend and before the instance.
void vk_instance_destroy(VkBackend *r);
void vk_backend_create(VkBackend *r, VkBackendDesc *desc);
// Requires idle submissions, drained queues, and destroyed caller-owned buffers/targets.
void vk_backend_destroy(VkBackend *r);
void wait_idle(VkBackend *r);

void pipeline_cache_save(VkDevice device, VkPhysicalDevice phys, VkPipelineCache cache, const char *path);
VkFormat pick_depth_format(VkPhysicalDevice gpu);
VkPresentModeKHR vk_swapchain_select_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool vsync);
void vk_create_swapchain(VkDevice device, VkPhysicalDevice gpu, FlowSwapchain *out_swapchain,
                         const FlowSwapchainCreateInfo *info, VkQueue graphics_queue, VkCommandPool one_time_pool,
                         VkBackend *r);
void vk_swapchain_destroy(VkDevice device, FlowSwapchain *swapchain, mu_id_pool *id_pool);
void vk_swapchain_recreate(VkDevice device, VkPhysicalDevice gpu, FlowSwapchain *sc, uint32_t new_w, uint32_t new_h,
                           VkQueue graphics_queue, VkCommandPool one_time_pool, VkBackend *r);
bool vk_swapchain_acquire(VkDevice device, FlowSwapchain *sc, VkSemaphore image_available, VkFence fence,
                                       uint64_t timeout);
bool vk_swapchain_present(VkQueue present_queue, FlowSwapchain *sc, const VkSemaphore *waits,
                                       uint32_t wait_count);

TextureID create_texture(VkBackend *r, const TextureCreateDesc *desc);
void destroy_texture(VkBackend *r, TextureID id);
bool buffer_pool_init(VkBackend *r,

                      BufferPoolType type, BufferPool *pool, VkDeviceSize size_bytes, VkBufferUsageFlags usage,
                      VmaMemoryUsage memory_usage, VmaAllocationCreateFlags alloc_flags, oa_uint32 max_allocs);
void buffer_pool_destroy(VkBackend *r, BufferPool *pool);
void buffer_pool_linear_reset(BufferPool *pool);
void buffer_pool_ring_free_to(BufferPool *pool, uint32_t offset);
BufferSlice buffer_pool_alloc(BufferPool *pool, VkDeviceSize size_bytes, VkDeviceSize alignment);
void buffer_pool_free(BufferSlice slice);
bool renderer_upload_buffer_to_slice(VkBackend *r, VkCommandBuffer cmd, BufferSlice dst_slice, ByteSpan data);
BufferSlice renderer_upload_buffer(VkBackend *r, VkCommandBuffer cmd, ByteSpan data, VkDeviceSize dst_alignment);
bool create_buffer(VkBackend *r, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memory_usage, Buffer *out);
void destroy_buffer(VkBackend *r, Buffer *buffer);
bool rt_create(VkBackend *r, RenderTarget *rt, const RenderTargetSpec *spec);
void rt_destroy(VkBackend *r, RenderTarget *rt);
bool rt_resize(VkBackend *r, RenderTarget *rt, uint32_t width, uint32_t height);
bool sampler_create(VkBackend *r, const SamplerDesc *desc, uint32_t *out_sampler_id);
void sampler_destroy(VkBackend *r, SamplerID id);
VkPipeline create_graphics_pipeline(VkBackend *renderer, const GraphicsPipelineConfig *cfg);
VkPipeline create_compute_pipeline(VkBackend *renderer, const char *compute_path);
PipelineID pipeline_create_compute(VkBackend *r, const char *path);
PipelineID pipeline_create_graphics(VkBackend *r, GraphicsPipelineConfig *cfg);
void pipeline_rebuild(VkBackend *r);
void pipeline_mark_dirty(VkBackend *r, const char *changed);
// Commands
void vk_cmd_set_viewport_scissor(VkCommandBuffer cmd, VkExtent2D extent);
void image_transition_swapchain(VkBackend *r, VkCommandBuffer cmd, FlowSwapchain *sc, VkImageLayout new_layout,
                                VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
void cmd_transition_all_mips(VkBackend *r, VkCommandBuffer cmd, VkImage image, ImageState *state,
                                    VkImageAspectFlags aspect, uint32_t mipCount, VkPipelineStageFlags2 newStage,
                                    VkAccessFlags2 newAccess, VkImageLayout newLayout, uint32_t newQueueFamily);
void cmd_transition_mip(VkBackend *r, VkCommandBuffer cmd, VkImage image, ImageState *state, VkImageAspectFlags aspect,
                        uint32_t mip, VkPipelineStageFlags2 newStage, VkAccessFlags2 newAccess, VkImageLayout newLayout,
                        uint32_t newQueueFamily);
void flush_barriers(VkBackend *r, VkCommandBuffer cmd);
void rt_transition_mip(VkBackend *r, VkCommandBuffer cmd, RenderTarget *rt, uint32_t mip,
                                 VkImageLayout new_layout, VkPipelineStageFlags2 new_stage, VkAccessFlags2 new_access

);
void rt_transition_all(VkBackend *r, VkCommandBuffer cmd, RenderTarget *rt, VkImageLayout new_layout,
                                 VkPipelineStageFlags2 new_stage, VkAccessFlags2 new_access);
void push_constants(VkBackend *r, VkCommandBuffer cmd, ByteSpan data);
void cmd_draw(VkBackend *r, VkCommandBuffer cmd, ByteSpan root, uint32_t vertex_count,
                           uint32_t instance_count);
void dispatch_push(VkBackend *r, VkCommandBuffer cmd, ByteSpan root, uint32_t group_count_x,
                                uint32_t group_count_y, uint32_t group_count_z);
void end_pass(VkCommandBuffer cmd);
void begin_pass(VkBackend *r, VkCommandBuffer cmd, const PassDesc *desc);
void delete_queue_defer(VkBackend *r, uint64_t retire_value, DeferredDestroyFn fn, void *user);
void delete_queue_tick(VkBackend *r);
void delete_queue_drain(VkBackend *r);
bool vk_frame_acquire(VkBackend *r);
void vk_frame_submit(VkBackend *r);

#endif
