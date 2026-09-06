#include "external/mu/mu/mu_perf.h"
#include "external/mu/offset_allocator.h"
#include "external/stb/stb_image.h"
#include "src/helpers.h"
#include "src/slangtypes.h"
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#define DMON_IMPL
#include "external/dmon/dmon.h"
// ids

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

    VkDeviceSize size_of_cpu_pool;

    VkDeviceSize size_of_gpu_pool;

    VkDeviceSize size_of_staging_pool;
} RendererDesc;

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
bool is_instance_extension_supported(const char *extension_name);

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
    VkFence         in_flight_fence;
    uint32_t        staging_tail;
} FrameContext;

typedef struct Bindless {
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;

    VkPipelineLayout pipeline_layout;

} Bindless;

typedef struct RenderTargetSpec {
    uint32_t           width;
    uint32_t           height;
    uint32_t           layers;
    VkFormat           format;
    VkImageUsageFlags  usage;
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
    const char     *vert_path;
    const char     *frag_path;
    VkCullModeFlags cull_mode;
    VkFrontFace     front_face;
    VkPolygonMode   polygon_mode;

    VkPrimitiveTopology topology;

    bool        depth_test_enable;
    bool        depth_write_enable;
    VkCompareOp depth_compare_op;

    uint32_t        color_attachment_count;
    const VkFormat *color_formats;
    VkFormat        depth_format;

    // Per-attachment blend state
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

typedef struct RendererPipelines {
    VkPipeline    pipelines[MAX_PIPELINES];
    PipelineEntry entries[MAX_PIPELINES];
    uint32_t      count;
    mu_id_pool    pipeline_id_pool;
} RendererPipelines;

typedef struct BarrierBatch {
    VkImageMemoryBarrier2 image_barriers[32];

    uint32_t image_count;
} BarrierBatch;

typedef struct {
    // ---- CPU profiling ----
    double   cpu_frame_ns;      // total frame time (e.g., from glfwGetTime)
    double   cpu_active_ns;     // time spent in engine work
    double   cpu_wait_ns;       // time waiting for GPU
    double   cpu_wait_accum_ns; // accumulated wait over several frames
    uint64_t cpu_prev_frame;    // timestamp from previous frame
    uint32_t frame_count;

    // ---- HOT: touched every frame ----
    // FrameContext   frames[MAX_FRAMES_IN_FLIGHT];
    uint32_t current_frame; // 0..MAX_FRAMES_IN_FLIGHT-1
    float    dt;

    FrameContext frames[MAX_FRAMES_IN_FLIGHT];

    FlowSwapchain swapchain;

    VkCommandPool one_time_gfx_pool;

    VkCommandPool transfer_pool;

    // rarely touched

    InstanceContext instance;
    DeviceContext   devc;

    // window
    GLFWwindow            *window;
    VkSurfaceKHR           surface;
    VkAllocationCallbacks *vk_allocator_callbacks;
    DeviceInfo             info;

    TextureSystem texture_system;

    Bindless         bindless_system;
    VkDescriptorPool imgui_descriptor_pool;
    mu_id_pool       sampler_pool;

    // render targets (BIG = cold)
    RenderTarget depth[MAX_SWAPCHAIN_IMAGES];
    RenderTarget hdr_color[MAX_SWAPCHAIN_IMAGES];
    RenderTarget ldr_color[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_final[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_edges[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_weights[MAX_SWAPCHAIN_IMAGES];

    // textures
    TextureID dummy_texture;
    TextureID smaa_area_tex;
    TextureID smaa_search_tex;

    // pipelines / resources
    struct {
        uint32_t smaa_edge;
        uint32_t smaa_weight;
        uint32_t smaa_blend;
    } smaa_pipelines;

    // frequently used GPU resources
    Buffer global_ubo[MAX_FRAMES_IN_FLIGHT];

    GpuProfiler gpuprofiler[MAX_FRAMES_IN_FLIGHT];

    DefaultSamplerTable default_samplers;

    BufferPool cpu_pool;
    BufferPool gpu_pool;
    BufferPool staging_pool;

    Buffer            readback_buffer;
    RendererPipelines render_pipelines;
    VkDeviceAddress   gpu_base_addr;
    VkSampler         samplers[MAX_BINDLESS_SAMPLERS];

    BarrierBatch barrierbatch;

    struct {
        uint32_t fullscreen;
        uint32_t postprocess;
        uint32_t gltf_minimal;
        uint32_t fire;
        uint32_t sprite;
        uint32_t slug_text;

        uint32_t beam;
        uint32_t sky;
        uint32_t skinning;
    } EnginePipelines;

} Renderer;
// renderer would be heap allocated   since we dont want to crash staack
bool is_instance_extension_supported(const char *extension_name) {
    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &extensionCount, NULL);
    VkExtensionProperties *extensions = malloc(extensionCount * sizeof(VkExtensionProperties));
    vkEnumerateInstanceExtensionProperties(NULL, &extensionCount, extensions);

    forEach(i, extensionCount) {
        if (strcmp(extension_name, extensions[i].extensionName) == 0) {
            free(extensions);
            return true;
        }
    }

    free(extensions);
    return false;
}

bool is_instance_layer_supported(const char *layer_name) {
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, NULL);
    VkLayerProperties *layers = malloc(layer_count * sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&layer_count, layers);

    forEach(i, layer_count) {
        if (strcmp(layer_name, layers[i].layerName) == 0) {
            free(layers);
            return true;
        }
    }

    free(layers);
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
                                              VkDebugUtilsMessageTypeFlagsEXT             type,
                                              const VkDebugUtilsMessengerCallbackDataEXT *data, void *user_data) {
    (void)type;
    (void)user_data;

    const char *tag = "MSG";

    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        tag = "ERROR";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        tag = "WARN";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        tag = "INFO";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
        tag = "VERBOSE";

    fprintf(stderr, "[VULKAN %s] %s\n", tag, data->pMessage);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        debug_break();
    }

    // {
    //     raise(SIGTRAP);
    // }
    return VK_FALSE;
}

bool device_supports_extensions(VkPhysicalDevice gpu, const char **req, uint32_t req_count) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL);

    VkExtensionProperties *props = malloc(sizeof(*props) * count);
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, props);

    for (uint32_t i = 0; i < req_count; i++) {
        bool found = false;

        for (uint32_t j = 0; j < count; j++) {
            if (strcmp(req[i], props[j].extensionName) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            log_error("[extensions] missing: %s", req[i]);
            free(props);
            return false;
        }

        log_info("[extensions] enabled: %s", req[i]);
    }

    free(props);
    return true;
}

typedef struct GpuScore {
    VkPhysicalDevice device;
    uint32_t         score;
} GpuScore;

static uint32_t score_physical_device(VkPhysicalDevice gpu, VkSurfaceKHR surface, const char **required_exts,
                                      uint32_t required_ext_count) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);

    fprintf(stderr, "[GPU] Evaluating: %s\n", props.deviceName);

    if (!device_supports_extensions(gpu, required_exts, required_ext_count)) {
        fprintf(stderr, "  -> rejected: missing required extensions\n");
        return 0;
    }

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, NULL);

    VkQueueFamilyProperties qprops[32];
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, qprops);

    VkBool32 can_present = VK_FALSE;
    forEach(i, queue_count) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present);
        if (present) {
            can_present = VK_TRUE;
            break;
        }
    }

    if (!can_present) {
        fprintf(stderr, "  -> rejected: cannot present to surface\n");
        return 0;
    }

    uint32_t score = 0;

    switch (props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        score += 1000;
        fprintf(stderr, "  + discrete bonus: 1000\n");
        break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        score += 600;
        fprintf(stderr, "  + integrated bonus: 600\n");
        break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        score += 300;
        fprintf(stderr, "  + virtual bonus: 300\n");
        break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        score += 50;
        fprintf(stderr, "  + cpu fallback: 50\n");
        break;
    default:
        fprintf(stderr, "  + unknown type: 0\n");
        break;
    }

    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);

    for (uint32_t i = 0; i < mem.memoryHeapCount; i++) {
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            uint32_t add = (uint32_t)(mem.memoryHeaps[i].size / (1024 * 1024 * 64));
            score += add;
            fprintf(stderr, "  + VRAM factor: %u\n", add);
        }
    }

    if (score == 0)
        score = 1;

    fprintf(stderr, "  -> final score: %u\n\n", score);
    return score;
}

VkPhysicalDevice pick_physical_device(VkInstance instance, VkSurfaceKHR surface, RendererDesc *rcd) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (count == 0) {
        fprintf(stderr, "[GPU] No Vulkan devices found. Tragic.\n");
        return VK_NULL_HANDLE;
    }

    VkPhysicalDevice devices[16];
    vkEnumeratePhysicalDevices(instance, &count, devices);

    GpuScore best = {0};

    log_info("[GPU] Found %u device(s). Scoring...", count);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t score =
            score_physical_device(devices[i], surface, rcd->device_extensions, rcd->device_extension_count);

        if (score > best.score) {
            best.device = devices[i];
            best.score  = score;
        }
    }

    if (best.device == VK_NULL_HANDLE) {
        fprintf(stderr, "[GPU] No suitable device found. Time to rethink life choices.\n");
    } else {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(best.device, &props);
        log_info("[GPU] Selected device: %s (score %u)", props.deviceName, best.score);
    }

    return best.device;
}

void query_device_features(VkPhysicalDevice gpu, VkFeatureChain *out) {
    memset(out, 0, sizeof(*out));

    out->core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    out->v11.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    out->v12.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    out->v13.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    // maintenance5 feature struct
    out->maintenance5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;

    // shader non-semantic info feature struct (for debug printf)

    // Chain: core -> v11 -> v12 -> v13 -> maintenance5 -> shaderNonSemanticInfo
    out->core.pNext = &out->v11;
    out->v11.pNext  = &out->v12;
    out->v12.pNext  = &out->v13;
    out->v13.pNext  = &out->maintenance5;

    vkGetPhysicalDeviceFeatures2(gpu, &out->core);
}

typedef struct RendererCaps {
    bool dynamic_rendering;
    bool sync2;
    bool descriptor_indexing;
    bool timeline_semaphores;
    bool multi_draw_indirect;
    bool multi_draw_indirect_count;
    bool buffer_device_address;
    bool maintenance4;
    bool bindless_textures;

    bool sampler_anisotropy;    // NEW
    bool atomic_int64;          // NEW
    bool scalar_block_layout;   // NEW
    bool robustness2;           // NEW
    bool index_type_uint8;      // NEW
    bool subgroup_size_control; // NEW
    bool debug_printf;          // Debug Printf support (VK_KHR_shader_non_semantic_info)
    bool pipeline_statistics_query;
} RendererCaps;

RendererCaps default_caps(void) {
    return (RendererCaps){
        .dynamic_rendering         = true,
        .sync2                     = true,
        .descriptor_indexing       = true,
        .timeline_semaphores       = true,
        .multi_draw_indirect       = true,
        .multi_draw_indirect_count = true,
        .buffer_device_address     = true,
        .maintenance4              = true,
        .bindless_textures         = true,

        .sampler_anisotropy        = true,
        .atomic_int64              = true,
        .scalar_block_layout       = true,
        .robustness2               = false, // I’ll explain below
        .index_type_uint8          = true,
        .subgroup_size_control     = false, // enable later if you need it
        .pipeline_statistics_query = false,
    };
}
static void enable_desc_indexing_feature(VkBool32 *feature_field, const char *name) {
    // feature_field currently contains "supported?" from vkGetPhysicalDeviceFeatures2
    if (*feature_field) {
        *feature_field = VK_TRUE; // enable
        log_info("[bindless] enabled: %s", name);
    } else {
        log_info("[bindless] NOT available: %s", name);
        // leave it VK_FALSE
    }
}
static void apply_caps(VkFeatureChain *f, const RendererCaps *caps) {
#define TRY_ENABLE(flag, supported, name)                                                                              \
    do {                                                                                                               \
        if ((caps->flag) && (supported)) {                                                                             \
            (supported) = VK_TRUE;                                                                                     \
            log_info("[features] enabled: %s", name);                                                                  \
        } else if (caps->flag) {                                                                                       \
            log_info("[features] unavailable: %s", name);                                                              \
        }                                                                                                              \
    } while (0)
    if (f->v11.shaderDrawParameters) {
        f->v11.shaderDrawParameters = VK_TRUE;
        log_info("[features] enabled: shaderDrawParameters (vulkan 1.1)");
    } else {
        log_info("[features] unavailable: shaderDrawParameters (vulkan 1.1)");
    }
    if (f->maintenance5.maintenance5) {
        f->maintenance5.maintenance5 = VK_TRUE;
        log_info("[features] enabled: maintenance5 (VK_KHR_maintenance5)");
    } else {
        log_info("[features] unavailable: maintenance5 (VK_KHR_maintenance5)");
    }

    TRY_ENABLE(sampler_anisotropy, f->core.features.samplerAnisotropy, "samplerAnisotropy");
    TRY_ENABLE(multi_draw_indirect, f->core.features.multiDrawIndirect, "multi-draw indirect");
    TRY_ENABLE(pipeline_statistics_query, f->core.features.pipelineStatisticsQuery, "pipeline statistics query");
    TRY_ENABLE(dynamic_rendering, f->v13.dynamicRendering, "dynamic rendering");
    TRY_ENABLE(sync2, f->v13.synchronization2, "synchronization2");
    TRY_ENABLE(descriptor_indexing, f->v12.descriptorIndexing, "descriptor indexing (vulkan 1.2)");
    TRY_ENABLE(timeline_semaphores, f->v12.timelineSemaphore, "timeline semaphores");
    TRY_ENABLE(multi_draw_indirect_count, f->v12.drawIndirectCount, "multi-draw indirect count (v1.2)");
    TRY_ENABLE(buffer_device_address, f->v12.bufferDeviceAddress, "buffer device address");
    TRY_ENABLE(maintenance4, f->v13.maintenance4, "maintenance4");

    if (caps->bindless_textures) {
        if (!f->v12.descriptorIndexing) {
            log_info("[bindless] descriptor indexing umbrella feature not supported -> bindless likely impossible");
        }

        enable_desc_indexing_feature(&f->v12.runtimeDescriptorArray, "runtimeDescriptorArray");

        enable_desc_indexing_feature(&f->v12.descriptorBindingVariableDescriptorCount,
                                     "descriptorBindingVariableDescriptorCount");

        enable_desc_indexing_feature(&f->v12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");

        enable_desc_indexing_feature(&f->v12.descriptorBindingSampledImageUpdateAfterBind,
                                     "descriptorBindingSampledImageUpdateAfterBind");

        enable_desc_indexing_feature(&f->v12.shaderSampledImageArrayNonUniformIndexing,
                                     "shaderSampledImageArrayNonUniformIndexing");
    }

#undef TRY_ENABLE
}

typedef struct queue_families {
    VkQueue graphics_queue;
    VkQueue present_queue;
    VkQueue compute_queue;
    VkQueue transfer_queue;

    uint32_t graphics_family;
    uint32_t present_family;
    uint32_t compute_family;
    uint32_t transfer_family;

    int has_graphics;
    int has_present;
    int has_compute;
    int has_transfer;
} queue_families;
//  pick GPU → choose queue families → create VkDevice → get queues
// Fills `out` with available queue families.
// Must be called BEFORE logical device creation.
void find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface, queue_families *out);
// Call AFTER vkCreateDevice.
// Uses the family indices already stored in queue_families.
void init_device_queues(VkDevice device, queue_families *q);

void find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface, queue_families *out) {
    // initialize everything
    *out = (queue_families){.graphics_queue = VK_NULL_HANDLE,
                            .present_queue  = VK_NULL_HANDLE,
                            .compute_queue  = VK_NULL_HANDLE,
                            .transfer_queue = VK_NULL_HANDLE,

                            .graphics_family = 0,
                            .present_family  = 0,
                            .compute_family  = 0,
                            .transfer_family = 0,

                            .has_graphics = 0,
                            .has_present  = 0,
                            .has_compute  = 0,
                            .has_transfer = 0};

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);
    if (count == 0)
        return;

    VkQueueFamilyProperties *families = (VkQueueFamilyProperties *)malloc(sizeof(VkQueueFamilyProperties) * count);

    if (!families)
        return;

    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);

    forEach(i, count) {
        const VkQueueFamilyProperties *f = &families[i];

        if (!out->has_graphics && (f->queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            out->graphics_family = i;
            out->has_graphics    = 1;
        }

        if (!out->has_compute && (f->queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            out->compute_family = i;
            out->has_compute    = 1;
        }

        if (!out->has_transfer && (f->queueFlags & VK_QUEUE_TRANSFER_BIT)) {
            out->transfer_family = i;
            out->has_transfer    = 1;
        }

        if (!out->has_present) {
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if (presentSupport) {
                out->present_family = i;
                out->has_present    = 1;
            }
        }

        if (out->has_graphics && out->has_present && out->has_compute && out->has_transfer) {
            break;
        }
    }

    free(families);
}

void init_device_queues(VkDevice device, queue_families *q) {
    if (q->has_graphics)
        vkGetDeviceQueue(device, q->graphics_family, 0, &q->graphics_queue);

    if (q->has_present)
        vkGetDeviceQueue(device, q->present_family, 0, &q->present_queue);

    if (q->has_compute)
        vkGetDeviceQueue(device, q->compute_family, 0, &q->compute_queue);

    if (q->has_transfer)
        vkGetDeviceQueue(device, q->transfer_family, 0, &q->transfer_queue);
}
static bool device_has_extension(VkPhysicalDevice gpu, const char *ext) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL);

    VkExtensionProperties *props = malloc(sizeof(*props) * count);
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, props);

    bool found = false;
    forEach(i, count) {
        if (strcmp(props[i].extensionName, ext) == 0) {
            found = true;
            break;
        }
    }

    free(props);
    return found;
}

#ifndef PIPELINE_CACHE_MAGIC
#define PIPELINE_CACHE_MAGIC 0xCAFEBABE
#endif

typedef struct PipelineCachePrefixHeader {
    uint32_t magic;
    uint32_t dataSize;
    uint64_t dataHash;

    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t driverVersion;
    uint32_t driverABI;

    uint8_t uuid[VK_UUID_SIZE];
} PipelineCachePrefixHeader;

static int write_all(FILE *f, const void *data, size_t size) { return fwrite(data, 1, size, f) == size; }

static int read_all(FILE *f, void *data, size_t size) { return fread(data, 1, size, f) == size; }

static void get_device_props(VkPhysicalDevice phys, VkPhysicalDeviceProperties *out) {
    vkGetPhysicalDeviceProperties(phys, out);
}

static int validate_header(const PipelineCachePrefixHeader *h, const VkPhysicalDeviceProperties *props) {
    if (h->magic != PIPELINE_CACHE_MAGIC)
        return 0;
    if (h->driverABI != sizeof(void *))
        return 0;
    if (h->vendorID != props->vendorID)
        return 0;
    if (h->deviceID != props->deviceID)
        return 0;
    if (h->driverVersion != props->driverVersion)
        return 0;
    if (memcmp(h->uuid, props->pipelineCacheUUID, VK_UUID_SIZE) != 0)
        return 0;
    return 1;
}

VkPipelineCache pipeline_cache_load_or_create(VkDevice device, VkPhysicalDevice phys, const char *path) {
    VkPhysicalDeviceProperties props;
    get_device_props(phys, &props);

    VkPipelineCache cache = VK_NULL_HANDLE;

    FILE *f = fopen(path, "rb");
    if (!f) {
        // file missing, build empty cache
        VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        vkCreatePipelineCache(device, &ci, NULL, &cache);
        return cache;
    }

    PipelineCachePrefixHeader hdr;
    if (!read_all(f, &hdr, sizeof(hdr))) {
        fclose(f);
        goto fallback;
    }

    if (!validate_header(&hdr, &props)) {
        fclose(f);
        goto fallback;
    }

    void *blob = malloc(hdr.dataSize);
    if (!blob) {
        fclose(f);
        goto fallback;
    }

    if (!read_all(f, blob, hdr.dataSize)) {
        free(blob);
        fclose(f);
        goto fallback;
    }

    fclose(f);

    if (hash64_bytes(blob, hdr.dataSize) != hdr.dataHash) {
        free(blob);
        goto fallback;
    }

    VkPipelineCacheCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = hdr.dataSize, .pInitialData = blob};

    VkResult res = vkCreatePipelineCache(device, &ci, NULL, &cache);
    free(blob);

    if (res != VK_SUCCESS) {

    fallback: {
        VkPipelineCacheCreateInfo empty = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};

        vkCreatePipelineCache(device, &empty, NULL, &cache);
    }
    }

    return cache;
}

void pipeline_cache_save(VkDevice device, VkPhysicalDevice phys, VkPipelineCache cache, const char *path) {
    size_t size = 0;
    vkGetPipelineCacheData(device, cache, &size, NULL);
    if (size == 0)
        return;

    void *blob = malloc(size);
    if (!blob)
        return;

    vkGetPipelineCacheData(device, cache, &size, blob);

    VkPhysicalDeviceProperties props;
    get_device_props(phys, &props);

    PipelineCachePrefixHeader hdr = {.magic         = PIPELINE_CACHE_MAGIC,
                                     .dataSize      = (uint32_t)size,
                                     .dataHash      = hash64_bytes(blob, size),
                                     .vendorID      = props.vendorID,
                                     .deviceID      = props.deviceID,
                                     .driverVersion = props.driverVersion,
                                     .driverABI     = sizeof(void *)};
    memcpy(hdr.uuid, props.pipelineCacheUUID, VK_UUID_SIZE);

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(blob);
        return;
    }

    write_all(f, &hdr, sizeof(hdr));
    write_all(f, blob, size);
    fclose(f);

    rename(tmp, path);
    free(blob);
}

VkFormat pick_depth_format(VkPhysicalDevice gpu) {
    VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
    };

    for (uint32_t i = 0; i < ARRAY_COUNT(candidates); i++) {
        VkFormat           fmt = candidates[i];
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(gpu, fmt, &props);

        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return fmt;
    }

    return VK_FORMAT_UNDEFINED;
}

VkPresentModeKHR vk_swapchain_select_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool vsync) {
    uint32_t count = 0;

    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, NULL));

    VkPresentModeKHR modes[16];

    if (count > 16)
        count = 16; // sanity clamp

    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, modes));

    // ============================================================
    // VSYNC ON → must use FIFO (guaranteed by Vulkan spec)
    // ============================================================

    if (vsync) {
        for (uint32_t i = 0; i < count; i++) {
            if (modes[i] == VK_PRESENT_MODE_FIFO_KHR)
                return VK_PRESENT_MODE_FIFO_KHR;
        }

        // Spec guarantees FIFO exists, but fallback anyway
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // ============================================================
    // VSYNC OFF → prefer MAILBOX, then IMMEDIATE, fallback FIFO
    // ============================================================

    VkPresentModeKHR best = VK_PRESENT_MODE_FIFO_KHR;

    for (uint32_t i = 0; i < count; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            return VK_PRESENT_MODE_MAILBOX_KHR;

        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            best = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    return best;
}

VkSurfaceCapabilities2KHR query_surface_capabilities(VkPhysicalDevice gpu, VkSurfaceKHR surface) {
    VkPhysicalDeviceSurfaceInfo2KHR info = {
        .sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
        .surface = surface,
    };

    VkSurfaceCapabilities2KHR caps = {
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
    };

    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilities2KHR(gpu, &info, &caps));
    return caps;
}

VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR *caps, uint32_t desired_w, uint32_t desired_h) {
    if (caps->currentExtent.width != 0xFFFFFFFF)
        return caps->currentExtent;
    VkExtent2D extent = {.width = desired_w, .height = desired_h};
    extent.width      = CLAMP(extent.width, caps->minImageExtent.width, caps->maxImageExtent.width);
    extent.height     = CLAMP(extent.height, caps->minImageExtent.height, caps->maxImageExtent.height);
    return extent;
}

VkSurfaceFormatKHR select_surface_format(VkPhysicalDevice gpu, VkSurfaceKHR surface, VkFormat preferred,
                                         VkColorSpaceKHR preferred_cs) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, NULL);

    VkSurfaceFormatKHR formats[32];
    if (count > 32)
        count = 32;

    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats);

    forEach(i, count) if (formats[i].format == preferred && formats[i].colorSpace == preferred_cs) return formats[i];

    return formats[0];
}

// Choose minImageCount given a user hint, but always respect Vulkan caps.
static uint32_t choose_min_image_count(const VkSurfaceCapabilities2KHR *caps, uint32_t preferred_hint) {
    const uint32_t min_cap = caps->surfaceCapabilities.minImageCount;

    // Never go below Vulkan's minimum, even if the hint is silly.
    uint32_t preferred = (preferred_hint > min_cap) ? preferred_hint : min_cap;

    // maxImageCount == 0 means "no upper bound"
    const uint32_t raw_max = caps->surfaceCapabilities.maxImageCount;
    const uint32_t max_cap = (raw_max == 0) ? preferred : raw_max;

    // Clamp to [min_cap, max_cap]
    if (preferred < min_cap)
        preferred = min_cap;
    if (preferred > max_cap)
        preferred = max_cap;

    return preferred;
}
void vk_create_swapchain(VkDevice device, VkPhysicalDevice gpu, FlowSwapchain *out_swapchain,
                         const FlowSwapchainCreateInfo *info, VkQueue graphics_queue, VkCommandPool one_time_pool,
                         Renderer *r) {
    VkSurfaceCapabilities2KHR caps = query_surface_capabilities(gpu, info->surface);

    // Query formats and present modes up-front to satisfy validation and pick supported values.
    VkSurfaceFormatKHR surface_format =
        select_surface_format(gpu, info->surface, info->preferred_format, info->preferred_color_space);

    VkExtent2D extent = choose_extent(&caps.surfaceCapabilities, info->width, info->height);

    if (extent.width == 0 || extent.height == 0)
        return; // minimized, wait later

    VkImageUsageFlags usage =
        (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | info->extra_usage) & caps.surfaceCapabilities.supportedUsageFlags;
    VkSwapchainCreateInfoKHR ci = {.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .surface          = info->surface,
                                   .minImageCount    = choose_min_image_count(&caps, info->min_image_count),
                                   .imageFormat      = surface_format.format,
                                   .imageColorSpace  = surface_format.colorSpace,
                                   .imageExtent      = extent,
                                   .imageArrayLayers = 1,
                                   .imageUsage       = usage,
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform     = caps.surfaceCapabilities.currentTransform,
                                   .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                   .presentMode      = info->preferred_present_mode,
                                   .clipped          = VK_TRUE,
                                   .oldSwapchain     = info->old_swapchain};

    log_info("[swapchain] create: extent=%ux%u minImageCount=%u format=%u colorSpace=%u presentMode=%u usage=0x%x",
             extent.width, extent.height, ci.minImageCount, ci.imageFormat, ci.imageColorSpace, ci.presentMode, usage);

    VK_CHECK(vkCreateSwapchainKHR(device, &ci, NULL, &out_swapchain->swapchain));

    out_swapchain->surface       = info->surface;
    out_swapchain->extent        = extent;
    out_swapchain->format        = surface_format.format;
    out_swapchain->color_space   = surface_format.colorSpace;
    out_swapchain->present_mode  = info->preferred_present_mode;
    out_swapchain->current_image = 0;
    out_swapchain->image_usage   = usage;
    // Query swapchain images
    VK_CHECK(vkGetSwapchainImagesKHR(device, out_swapchain->swapchain, &out_swapchain->image_count, NULL));
    log_info("[swapchain] images: %u", out_swapchain->image_count);

    if (out_swapchain->image_count > MAX_SWAPCHAIN_IMAGES)
        out_swapchain->image_count = MAX_SWAPCHAIN_IMAGES; // don’t blow the stack

    VK_CHECK(
        vkGetSwapchainImagesKHR(device, out_swapchain->swapchain, &out_swapchain->image_count, out_swapchain->images));

    // Create image views
    forEach(i, out_swapchain->image_count) {
        VkImageViewCreateInfo view_ci = VK_IMAGE_VIEW_DEFAULT(out_swapchain->images[i], out_swapchain->format);
        VK_CHECK(vkCreateImageView(device, &view_ci, NULL, &out_swapchain->image_views[i]));
    }
    forEach(i, out_swapchain->image_count) {
        out_swapchain->states[i] = (ImageState){.layout   = VK_IMAGE_LAYOUT_UNDEFINED,
                                                .stage    = VK_PIPELINE_STAGE_2_NONE,
                                                .access   = 0,
                                                .validity = IMAGE_STATE_UNDEFINED};

        mu_id_pool_create_id(&r->texture_system.id_pool, &out_swapchain->bindless_index[i]);

        if (info->extra_usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
            VkDescriptorImageInfo img = {.imageView   = out_swapchain->image_views[i],
                                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

            VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                          .dstSet          = r->bindless_system.set,
                                          .dstBinding      = BINDLESS_TEXTURE_BINDING,
                                          .dstArrayElement = out_swapchain->bindless_index[i],

                                          .descriptorCount = 1,
                                          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          .pImageInfo      = &img};

            vkUpdateDescriptorSets(r->devc.device, 1, &write, 0, NULL);
        }
        if (info->extra_usage & VK_IMAGE_USAGE_STORAGE_BIT) {
            VkDescriptorImageInfo img = {.imageView   = out_swapchain->image_views[i],
                                         .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

            VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                          .dstSet     = r->bindless_system.set,
                                          .dstBinding = BINDLESS_STORAGE_IMAGE_BINDING,

                                          .dstArrayElement = out_swapchain->bindless_index[i],
                                          .descriptorCount = 1,
                                          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                          .pImageInfo      = &img};

            vkUpdateDescriptorSets(r->devc.device, 1, &write, 0, NULL);
        }
    }
    vk_create_semaphores(device, out_swapchain->image_count, out_swapchain->render_finished);
}

void vk_swapchain_destroy(VkDevice device, FlowSwapchain *swapchain, mu_id_pool *id_pool) {
    if (!swapchain)
        return;

    forEach(i, swapchain->image_count) {
        mu_id_pool_destroy_id(id_pool, swapchain->bindless_index[i]);
        if (swapchain->image_views[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, swapchain->image_views[i], NULL);
        }
    }

    vk_destroy_semaphores(device, swapchain->image_count, swapchain->render_finished);
    if (swapchain->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain->swapchain, NULL);
    }

    memset(swapchain, 0, sizeof(*swapchain));
}

void vk_swapchain_recreate(VkDevice device, VkPhysicalDevice gpu, FlowSwapchain *sc, uint32_t new_w, uint32_t new_h,
                           VkQueue graphics_queue, VkCommandPool one_time_pool, Renderer *r)

{
    if (new_w == 0 || new_h == 0)
        return;
    vkDeviceWaitIdle(device);

    forEach(i, sc->image_count) {
        if (sc->image_views[i])
            vkDestroyImageView(device, sc->image_views[i], NULL);
    }

    vk_destroy_semaphores(device, sc->image_count, sc->render_finished);
    FlowSwapchainCreateInfo info = {0};
    info.surface                 = sc->surface;
    info.width                   = new_w;
    info.height                  = new_h;
    info.min_image_count         = MAX(3u, sc->image_count);
    info.preferred_format        = sc->format;
    info.preferred_color_space   = sc->color_space;
    info.preferred_present_mode  = sc->present_mode;
    info.extra_usage             = sc->image_usage & ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.old_swapchain           = sc->swapchain;

    VkSwapchainKHR old = sc->swapchain;

    vk_create_swapchain(device, gpu, sc, &info, graphics_queue, one_time_pool, r);

    if (old)
        vkDestroySwapchainKHR(device, old, NULL);
}

static PFN_vkVoidFunction imgui_vk_loader(const char *function_name, void *user_data) {
    VkInstance instance = (VkInstance)user_data;
    return vkGetInstanceProcAddr(instance, function_name);
}

void imgui_init(GLFWwindow *window, VkInstance instance, VkPhysicalDevice gpu, VkDevice device, uint32_t queue_family,
                VkQueue queue, VkDescriptorPool imgui_pool, uint32_t min_image_count, uint32_t image_count,
                VkFormat swapchain_format, VkFormat depth_format, VkImageUsageFlags swapchain_usage) {

    igCreateContext(NULL);

    ImGuiIO *io     = igGetIO_Nil();
    io->IniFilename = NULL;
    io->LogFilename = NULL;

    igStyleColorsDark(NULL);

    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imgui_vk_loader, instance);

    ImGui_ImplVulkan_InitInfo info    = {0};
    info.ApiVersion                   = VK_API_VERSION_1_3;
    info.Instance                     = instance;
    info.PhysicalDevice               = gpu;
    info.Device                       = device;
    info.QueueFamily                  = queue_family;
    info.Queue                        = queue;
    info.DescriptorPool               = imgui_pool;
    info.MinImageCount                = min_image_count;
    info.ImageCount                   = image_count;
    info.UseDynamicRendering          = true;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    info.PipelineInfoMain.PipelineRenderingCreateInfo = (VkPipelineRenderingCreateInfoKHR){
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &swapchain_format,
        .depthAttachmentFormat   = depth_format,
    };

    info.PipelineInfoMain.SwapChainImageUsage = swapchain_usage;

    ImGui_ImplVulkan_Init(&info);
}

static MU_INLINE VkImageAspectFlags get_image_aspect(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;

    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;

    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
TextureID create_texture(Renderer *r, const TextureCreateDesc *desc) {
    TextureID    id;
    Texture     *tex;
    TextureInfo *info;
    uint32_t     depth     = desc->depth ? desc->depth : 1;
    uint32_t     layers    = desc->layers ? desc->layers : 1;
    uint32_t     mip_count = desc->mip_count ? desc->mip_count : 1;

    VkImageCreateInfo       image_info;
    VmaAllocationCreateInfo alloc_info;

    VkImageViewCreateInfo view_info;

    /*
        ------------------------------------------------------------
        1. Allocate a TextureID
        ------------------------------------------------------------

        TextureID is currently both:

            CPU texture slot
            GPU bindless descriptor slot

        Therefore:

            id
              │
              ├── texture_system.textures[id]
              │
              └── bindless descriptor[id]
    */

    if (!mu_id_pool_create_id(&r->texture_system.id_pool, &id)) {
        fprintf(stderr, "Texture pool exhausted\n");
        return UINT32_MAX;
    }

    tex  = &r->texture_system.textures[id];
    info = &r->texture_system.info[id];

    /*
        ------------------------------------------------------------
        2. Store metadata
        ------------------------------------------------------------
    */

    info->width     = desc->width;
    info->height    = desc->height;
    info->mip_count = mip_count;
    info->format    = desc->format;

    /*
        ------------------------------------------------------------
        3. Create VkImage
        ------------------------------------------------------------

        Normal 2D texture:

            imageType = 2D
            depth     = 1
            layers    = 1

        3D texture:

            imageType = 3D
            depth     > 1
            layers    = 1

        Array texture:

            imageType = 2D
            depth     = 1
            layers    > 1

        Cube:

            imageType = 2D
            depth     = 1
            layers    = 6
            flags     = CUBE_COMPATIBLE
    */

    image_info = (VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = NULL,

        .flags = desc->flags,

        .imageType = (depth > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,

        .format = desc->format,

        .extent =
            {
                .width  = desc->width,
                .height = desc->height,
                .depth  = depth,
            },

        .mipLevels   = mip_count,
        .arrayLayers = layers,

        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling  = VK_IMAGE_TILING_OPTIMAL,

        .usage = desc->usage,

        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,

        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = NULL,

        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    /*
        ------------------------------------------------------------
        4. Allocate GPU memory using VMA
        ------------------------------------------------------------
    */

    alloc_info = (VmaAllocationCreateInfo){
        .usage = VMA_MEMORY_USAGE_GPU_ONLY,
    };

    VK_CHECK(vmaCreateImage(r->devc.vmaallocator, &image_info, &alloc_info, &tex->image, &tex->allocation, NULL));

    /*
        ------------------------------------------------------------
        5. Determine image view type
        ------------------------------------------------------------

        2D:
            layers == 1

        2D array:
            layers > 1

        Cube:
            CUBE_COMPATIBLE + 6 layers

        Cube array:
            CUBE_COMPATIBLE + multiple-of-6 layers

        3D:
            depth > 1
    */

    VkImageViewType view_type;

    if (depth > 1) {
        view_type = VK_IMAGE_VIEW_TYPE_3D;
    } else if (desc->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
        if (layers == 6) {
            view_type = VK_IMAGE_VIEW_TYPE_CUBE;
        } else {
            view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        }
    } else if (layers > 1) {
        view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    } else {
        view_type = VK_IMAGE_VIEW_TYPE_2D;
    }

    /*
        ------------------------------------------------------------
        6. Create image view
        ------------------------------------------------------------

        The image is the actual storage.

        The view describes how shaders access that storage.

            VkImage
                │
                ▼
            VkImageView
                │
                ▼
            shader
    */

    view_info = (VkImageViewCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = NULL,

        .image = tex->image,

        .viewType = view_type,
        .format   = desc->format,

        .components =
            {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },

        .subresourceRange =
            {
                .aspectMask = get_image_aspect(desc->format),

                .baseMipLevel = 0,
                .levelCount   = mip_count,

                .baseArrayLayer = 0,

                /*
                    3D images use layerCount = 1.
                    Array/cubemap images use the actual layer count.
                */
                .layerCount = (depth > 1) ? 1 : layers,
            },
    };

    VK_CHECK(vkCreateImageView(r->devc.device, &view_info, r->vk_allocator_callbacks, &tex->view));

    /*
        ------------------------------------------------------------
        7. Update bindless sampled-image descriptor
        ------------------------------------------------------------

            TextureID
                │
                ▼
            descriptor[id]
                │
                ▼
            VkImageView
    */

    if (desc->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        VkDescriptorImageInfo image_info = {
            .sampler   = VK_NULL_HANDLE,
            .imageView = tex->view,

            /*
                This is the layout the shader expects when it
                actually accesses the image.

                The image must be transitioned to this layout
                before the draw/dispatch that uses it.
            */
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = NULL,

            .dstSet          = r->bindless_system.set,
            .dstBinding      = BINDLESS_TEXTURE_BINDING,
            .dstArrayElement = id,

            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,

            .pImageInfo = &image_info,
        };

        vkUpdateDescriptorSets(r->devc.device, 1, &write, 0, NULL);
    }

    /*
        ------------------------------------------------------------
        8. Update bindless storage-image descriptor
        ------------------------------------------------------------

            storage image
                  │
                  ▼
            descriptor[id]
                  │
                  ▼
                VkImageView
    */

    if (desc->usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        VkDescriptorImageInfo image_info = {
            .sampler     = VK_NULL_HANDLE,
            .imageView   = tex->view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };

        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = NULL,

            .dstSet          = r->bindless_system.set,
            .dstBinding      = BINDLESS_STORAGE_IMAGE_BINDING,
            .dstArrayElement = id,

            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,

            .pImageInfo = &image_info,
        };

        vkUpdateDescriptorSets(r->devc.device, 1, &write, 0, NULL);
    }

    /*
        ------------------------------------------------------------
        9. Optional debug name
        ------------------------------------------------------------

    */

    /*
        ------------------------------------------------------------
        IMPORTANT:
        The image is currently:

            VK_IMAGE_LAYOUT_UNDEFINED

        It is NOT magically in:

            SHADER_READ_ONLY_OPTIMAL
            or
            GENERAL

        Updating a descriptor does not transition the image.

        The upload / initialization path must transition it before
        the GPU actually uses it.
    */

    return id;
}

bool create_buffer(Renderer *r, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memory_usage, Buffer *out);

void destroy_buffer(Renderer *r, Buffer *b);

bool buffer_pool_init(Renderer *r,

                      BufferPoolType type, BufferPool *pool, VkDeviceSize size_bytes, VkBufferUsageFlags usage,
                      VmaMemoryUsage memory_usage, VmaAllocationCreateFlags alloc_flags, oa_uint32 max_allocs) {
    if (!r || !pool || size_bytes == 0)
        return false;

    if (size_bytes > UINT32_MAX) {
        log_error("[buffer_pool] size exceeds 4GB: %llu", (unsigned long long)size_bytes);
        return false;
    }

    memset(pool, 0, sizeof(*pool));

    VkBufferCreateInfo buffer_info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size_bytes,
        .usage       = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo alloc_info = {
        .usage = memory_usage,
        .flags = alloc_flags,
    };

    VmaAllocationInfo out_info = {0};
    VkResult          res =
        vmaCreateBuffer(r->devc.vmaallocator, &buffer_info, &alloc_info, &pool->buffer, &pool->allocation, &out_info);
    if (res != VK_SUCCESS) {
        log_error("[buffer_pool] vmaCreateBuffer failed: %d", res);
        return false;
    }

    pool->size_bytes   = size_bytes;
    pool->usage        = usage;
    pool->memory_usage = memory_usage;
    pool->alloc_flags  = alloc_flags;
    pool->mapped       = out_info.pMappedData;

    pool->type = type;

    if (type == BUFFER_POOL_LINEAR) {
        mu_linear_init(&pool->linear, pool->mapped, (uint32_t)size_bytes);
    } else if (type == BUFFER_POOL_RING) {
        mu_ring_init(&pool->ring, pool->mapped, (uint32_t)size_bytes);
    } else if (type == BUFFER_POOL_TLSF) {
        oa_init(&pool->tlsf, (oa_uint32)size_bytes, max_allocs);
    }

    return true;
}

typedef struct BufferSlice {
    BufferPool   *pool;
    VkBuffer      buffer;
    VkDeviceSize  offset;
    VkDeviceSize  size;
    void         *mapped;
    OA_Allocation allocation;
} BufferSlice;

void buffer_pool_destroy(Renderer *r, BufferPool *pool) {
    if (!r || !pool)
        return;
    if (pool->type == BUFFER_POOL_TLSF) {
        oa_destroy(&pool->tlsf);
    }

    if (pool->buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(r->devc.vmaallocator, pool->buffer, pool->allocation);

    memset(pool, 0, sizeof(*pool));
}
void buffer_pool_linear_reset(BufferPool *pool) {
    if (pool->type == BUFFER_POOL_LINEAR) {
        mu_linear_reset(&pool->linear);
    }
}

void buffer_pool_ring_free_to(BufferPool *pool, uint32_t offset) {
    if (pool->type == BUFFER_POOL_RING) {
        mu_ring_free_to(&pool->ring, offset);
    }
}
BufferSlice buffer_pool_alloc(BufferPool *pool, VkDeviceSize size_bytes, VkDeviceSize alignment) {
    BufferSlice slice = {0};

    if (!pool || size_bytes == 0)
        return slice;

    if (alignment == 0)
        alignment = 1;

    if (size_bytes > UINT32_MAX || alignment > UINT32_MAX)
        return slice;
    uint32_t size  = (uint32_t)size_bytes;
    uint32_t align = (uint32_t)alignment;

    uint32_t offset = 0;

    switch (pool->type) {
    case BUFFER_POOL_LINEAR: {
        void *ptr = mu_linear_alloc(&pool->linear, size, align);
        if (!ptr)
            return slice;

        offset = (uint32_t)((uint8_t *)ptr - (uint8_t *)pool->mapped);
    } break;

    case BUFFER_POOL_RING: {
        void *ptr = mu_ring_alloc(&pool->ring, size, align, &offset);
        if (!ptr)
            return slice;
    } break;

    case BUFFER_POOL_TLSF: {
        OA_Allocation a = (align > 1) ? oa_allocate_aligned(&pool->tlsf, size, align) : oa_allocate(&pool->tlsf, size);

        if (a.offset == OA_NO_SPACE)
            return slice;

        offset           = a.offset;
        slice.allocation = a;
    } break;
    }

    slice.pool   = pool;
    slice.buffer = pool->buffer;
    slice.offset = offset;
    slice.size   = size_bytes;

    if (pool->mapped)
        slice.mapped = (uint8_t *)pool->mapped + offset;
    return slice;
}

void buffer_pool_free(BufferSlice slice) {
    if (!slice.pool)
        return;

    BufferPool *pool = slice.pool;

    if (pool->type == BUFFER_POOL_TLSF) {
        oa_free(&pool->tlsf, slice.allocation);
    }
}

bool renderer_upload_buffer_to_slice(Renderer *r, VkCommandBuffer cmd, BufferSlice dst_slice, const void *src_data,
                                     VkDeviceSize size_bytes, VkDeviceSize staging_alignment) {
    if (!r || !cmd || !src_data || !dst_slice.buffer || size_bytes == 0)
        return false;

    if (size_bytes > dst_slice.size)
        return false;

    BufferSlice staging_slice = buffer_pool_alloc(&r->staging_pool, size_bytes, staging_alignment);
    if (!staging_slice.buffer || !staging_slice.mapped)
        return false;

    memcpy(staging_slice.mapped, src_data, (size_t)size_bytes);

    VkBufferCopy copy = {
        .srcOffset = staging_slice.offset,
        .dstOffset = dst_slice.offset,
        .size      = size_bytes,
    };
    vkCmdCopyBuffer(cmd, staging_slice.buffer, dst_slice.buffer, 1, &copy);

    return true;
}

BufferSlice renderer_upload_buffer(Renderer *r, VkCommandBuffer cmd, const void *src_data, VkDeviceSize size_bytes,
                                   VkDeviceSize staging_alignment, VkDeviceSize dst_alignment) {
    BufferSlice dst_slice = {0};
    if (!r || !cmd || !src_data || size_bytes == 0)
        return dst_slice;

    dst_slice = buffer_pool_alloc(&r->gpu_pool, size_bytes, dst_alignment);
    if (!dst_slice.buffer)
        return dst_slice;

    if (!renderer_upload_buffer_to_slice(r, cmd, dst_slice, src_data, size_bytes, staging_alignment)) {
        buffer_pool_free(dst_slice);
        memset(&dst_slice, 0, sizeof(dst_slice));
    }

    return dst_slice;
}
bool create_buffer(Renderer *r, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memory_usage, Buffer *out) {
    VkBufferCreateInfo buffer_info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc_info = {.usage = memory_usage,
                                          .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                   VMA_ALLOCATION_CREATE_MAPPED_BIT};

    if (vmaCreateBuffer(r->devc.vmaallocator, &buffer_info, &alloc_info, &out->buffer, &out->allocation, NULL) !=
        VK_SUCCESS) {
        return false;
    }
    out->buffer_size = size;
    out->mapping     = NULL;

    VmaAllocationInfo info;
    vmaGetAllocationInfo(r->devc.vmaallocator, out->allocation, &info);

    out->mapping = info.pMappedData;

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo addr_info = {.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                               .buffer = out->buffer};

        out->address = vkGetBufferDeviceAddress(r->devc.device, &addr_info);
    } else {
        out->address = 0;
    }

    return true;
}
void destroy_buffer(Renderer *r, Buffer *buffer) {
    if (!r || !buffer)
        return;

    /*
        Buffer ownership:

            Buffer
             |
             ├── VkBuffer
             └── VmaAllocation

        VMA owns the memory relationship, so destroy both through
        vmaDestroyBuffer().
    */

    if (buffer->buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(r->devc.vmaallocator, buffer->buffer, buffer->allocation);
    }

    /*
        The CPU mapping belongs to the VMA allocation.
        Do not free() it manually.
    */

    memset(buffer, 0, sizeof(*buffer));
}
static uint32_t rt_compute_mip_count(uint32_t w, uint32_t h) {
    uint32_t max_dim = w > h ? w : h;
    uint32_t mips    = 1;
    while (max_dim > 1) {
        max_dim >>= 1;
        mips++;
    }
    return mips;
}

static void rt_update_bindless_descriptors(Renderer *r, const RenderTarget *rt);
static void rt_destroy_internal(Renderer *r, RenderTarget *rt, bool release_id);

static bool rt_create_internal(Renderer *r, RenderTarget *rt, const RenderTargetSpec *spec, uint32_t bindless_index) {

    if (!r || !rt || !spec || spec->width == 0 || spec->height == 0)
        return false;

    memset(rt, 0, sizeof(*rt));

    rt->format = spec->format;
    rt->width  = spec->width;
    rt->height = spec->height;
    rt->usage  = spec->usage;
    rt->aspect = spec->aspect ? spec->aspect : get_image_aspect(spec->format);
    rt->layers = spec->layers;
    // Mip count
    uint32_t mips = spec->mip_count;
    if (mips == 0)
        mips = rt_compute_mip_count(spec->width, spec->height);
    if (mips > RT_MAX_MIPS)
        mips = RT_MAX_MIPS;
    rt->mip_count = mips;

    // // Bindless slots unused until registered
     rt->bindless_index = r->dummy_texture;
    //
    // Create image
    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = rt->format,
        .extent        = {rt->width, rt->height, 1},
        .mipLevels     = rt->mip_count,
        .arrayLayers   = spec->layers,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = rt->usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VmaAllocationCreateInfo alloc_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
    };

    VkResult res = vmaCreateImage(r->devc.vmaallocator, &image_info, &alloc_info, &rt->image, &rt->allocation, NULL);
    if (res != VK_SUCCESS) {
        log_error("[rt_create] vmaCreateImage failed: %d", res);
        return false;
    }

    // Full mip chain view (for sampling)
    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = rt->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = rt->format,
        .subresourceRange =
            {
                .aspectMask     = rt->aspect,
                .baseMipLevel   = 0,
                .levelCount     = rt->mip_count,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };

    VK_CHECK(vkCreateImageView(r->devc.device, &view_info, NULL, &rt->view));

    // Per-mip views (for attachment use)
    for (uint32_t mip = 0; mip < rt->mip_count; mip++) {
        VkImageViewCreateInfo mip_view_info = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = rt->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = rt->format,
            .subresourceRange =
                {
                    .aspectMask     = rt->aspect,
                    .baseMipLevel   = mip,
                    .levelCount     = 1,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
        };
        VK_CHECK(vkCreateImageView(r->devc.device, &mip_view_info, NULL, &rt->mip_views[mip]));
    }

    // Init per-mip states to undefined
    for (uint32_t mip = 0; mip < rt->mip_count; mip++) {
        rt->mip_states[mip] = (ImageState){
            .stage        = VK_PIPELINE_STAGE_2_NONE,
            .access       = VK_ACCESS_2_NONE,
            .layout       = VK_IMAGE_LAYOUT_UNDEFINED,
            .queue_family = VK_QUEUE_FAMILY_IGNORED,
            .validity     = IMAGE_STATE_UNDEFINED,
            .dirty_mips   = 0,
        };
    }

    if (spec->debug_name) {
        (void)spec->debug_name; // for future VK_EXT_debug_utils
    }

    uint32_t id = bindless_index;
    if (id == UINT32_MAX && !mu_id_pool_create_id(&r->texture_system.id_pool, &id)) {
        fprintf(stderr, "Texture pool exhausted\n");
        return false;
    }
    rt->bindless_index = id;

    rt_update_bindless_descriptors(r, rt);

    log_info("[rt_create] %ux%u fmt=%d mips=%u ", rt->width, rt->height, rt->format, rt->mip_count);
    return true;
}

bool rt_create(Renderer *r, RenderTarget *rt, const RenderTargetSpec *spec) {
    return rt_create_internal(r, rt, spec, UINT32_MAX);
}

static void rt_update_bindless_descriptors(Renderer *r, const RenderTarget *rt) {
    if (!r || !rt || rt->bindless_index == UINT32_MAX)
        return;

    VkWriteDescriptorSet writes[2];
    VkDescriptorImageInfo images[2];
    uint32_t write_count = 0;

    if (rt->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        images[write_count] = (VkDescriptorImageInfo){
            .imageView = rt->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        writes[write_count] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->bindless_system.set,
            .dstBinding = BINDLESS_TEXTURE_BINDING,
            .dstArrayElement = rt->bindless_index,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo = &images[write_count],
        };
        write_count++;
    }

    if (rt->usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        images[write_count] = (VkDescriptorImageInfo){
            .imageView = rt->view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        writes[write_count] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->bindless_system.set,
            .dstBinding = BINDLESS_STORAGE_IMAGE_BINDING,
            .dstArrayElement = rt->bindless_index,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &images[write_count],
        };
        write_count++;
    }

    if (write_count != 0)
        vkUpdateDescriptorSets(r->devc.device, write_count, writes, 0, NULL);
}

static void rt_destroy_internal(Renderer *r, RenderTarget *rt, bool release_id) {
    if (!r || !rt || !rt->image)
        return;
    vkDeviceWaitIdle(r->devc.device);
    uint32_t id = rt->bindless_index;

    if (id != UINT32_MAX) {
        // Clear sampled descriptor if used
        if (rt->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
            VkDescriptorImageInfo img = {.imageView   = r->texture_system.textures[r->dummy_texture].view,
                                         .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

            VkWriteDescriptorSet write = {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet          = r->bindless_system.set,
                                          .dstBinding      = BINDLESS_TEXTURE_BINDING,
                                          .dstArrayElement = id,
                                          .descriptorCount = 1,
                                          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          .pImageInfo      = &img};

            vkUpdateDescriptorSets(r->devc.device, 1, &write, 0, NULL);
        }

        // Clear storage descriptor if used
        if (rt->usage & VK_IMAGE_USAGE_STORAGE_BIT) {
            VkDescriptorImageInfo img = {.imageView   = r->texture_system.textures[r->dummy_texture].view,
                                         .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

            VkWriteDescriptorSet write = {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet          = r->bindless_system.set,
                                          .dstBinding      = BINDLESS_STORAGE_IMAGE_BINDING,
                                          .dstArrayElement = id,
                                          .descriptorCount = 1,
                                          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                          .pImageInfo      = &img};

            vkUpdateDescriptorSets(r->devc.device, 1, &write, 0, NULL);
        }

        if (release_id)
            mu_id_pool_destroy_id(&r->texture_system.id_pool, id);
    }

    if (rt->view)
        vkDestroyImageView(r->devc.device, rt->view, NULL);

    for (uint32_t i = 0; i < rt->mip_count; i++) {
        if (rt->mip_views[i])
            vkDestroyImageView(r->devc.device, rt->mip_views[i], NULL);
    }

    if (rt->image)
        vmaDestroyImage(r->devc.vmaallocator, rt->image, rt->allocation);

    memset(rt, 0, sizeof(*rt));
}

void rt_destroy(Renderer *r, RenderTarget *rt) {
    rt_destroy_internal(r, rt, true);
}

bool rt_resize(Renderer *r, RenderTarget *rt, uint32_t width, uint32_t height)

{
    if (!r || !rt)
        return false;

    if (width == rt->width && height == rt->height)
        return true;

    uint32_t bindless_index = rt->bindless_index;
    RenderTargetSpec spec = {.width      = width,
                             .height     = height,
                             .layers     = rt->layers,
                             .format     = rt->format,
                             .usage      = rt->usage,
                             .aspect     = rt->aspect,
                             .mip_count  = rt->mip_count,
                             .debug_name = rt->debug_name};

    rt_destroy_internal(r, rt, false);
    return rt_create_internal(r, rt, &spec, bindless_index);
}
bool sampler_create(Renderer *r, const VkSamplerCreateInfo *ci, uint32_t *out_sampler_id) {
    if (!r || !ci || !out_sampler_id)
        return false;

    VkSampler sampler = VK_NULL_HANDLE;

    VkResult res = vkCreateSampler(r->devc.device, ci, NULL, &sampler);
    if (res != VK_SUCCESS)
        return false;

    uint32_t id;
    if (!mu_id_pool_create_id(&r->sampler_pool, &id)) {
        vkDestroySampler(r->devc.device, sampler, NULL);
        return false;
    }

    r->samplers[id] = sampler;

    *out_sampler_id = id;

    VkDescriptorImageInfo sampler_info = {.sampler = r->samplers[id]};

    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                  .dstSet          = r->bindless_system.set,
                                  .dstBinding      = BINDLESS_SAMPLER_BINDING,
                                  .dstArrayElement = id,

                                  .descriptorCount = 1,
                                  .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
                                  .pImageInfo      = &sampler_info};

    vkUpdateDescriptorSets(r->devc.device, 1, &write, 0, NULL);
    return true;
}

static ColorAttachmentBlend blend_alpha(void) {
    return (ColorAttachmentBlend){
        .blend_enable = true,

        .src_color = VK_BLEND_FACTOR_SRC_ALPHA,
        .dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .color_op  = VK_BLEND_OP_ADD,

        .src_alpha = VK_BLEND_FACTOR_ONE,
        .dst_alpha = VK_BLEND_FACTOR_ZERO,
        .alpha_op  = VK_BLEND_OP_ADD,

        .write_mask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
}

static ColorAttachmentBlend blend_additive(void) {
    return (ColorAttachmentBlend){
        .blend_enable = true,

        .src_color = VK_BLEND_FACTOR_ONE,
        .dst_color = VK_BLEND_FACTOR_ONE,
        .color_op  = VK_BLEND_OP_ADD,

        .src_alpha = VK_BLEND_FACTOR_ONE,
        .dst_alpha = VK_BLEND_FACTOR_ONE,
        .alpha_op  = VK_BLEND_OP_ADD,

        .write_mask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
}

static ColorAttachmentBlend blend_disabled(void) {
    return (ColorAttachmentBlend){
        .blend_enable = false,

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
    GraphicsPipelineConfig cfg = {0};

    cfg.cull_mode    = VK_CULL_MODE_NONE;
    cfg.front_face   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    cfg.polygon_mode = VK_POLYGON_MODE_FILL;

    cfg.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    cfg.depth_test_enable  = true;
    cfg.depth_write_enable = true;
    cfg.depth_compare_op   = VK_COMPARE_OP_GREATER;

    cfg.color_attachment_count = 0;
    cfg.color_formats          = NULL;
    cfg.depth_format           = VK_FORMAT_UNDEFINED;

    for (uint32_t i = 0; i < MAX_COLOR_ATTACHMENTS; i++)
        cfg.blends[i] = blend_disabled();

    return cfg;
}

static VkShaderModule create_shader_module(VkDevice device, const void *code, size_t size) {
    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = (const uint32_t *)code,
    };

    VkShaderModule mod = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &ci, NULL, &mod));
    return mod;
}

VkPipeline create_graphics_pipeline(Renderer *renderer, const GraphicsPipelineConfig *cfg) {

    void  *vs_code = NULL;
    size_t vs_size = 0;

    void  *fs_code = NULL;
    size_t fs_size = 0;

    if (!read_file(cfg->vert_path, &vs_code, &vs_size))
        abort();

    if (!read_file(cfg->frag_path, &fs_code, &fs_size))
        abort();

    VkShaderModule vs = create_shader_module(renderer->devc.device, vs_code, vs_size);

    VkShaderModule fs = create_shader_module(renderer->devc.device, fs_code, fs_size);

    VkPipelineShaderStageCreateInfo      stages[2] = {{
                                                          .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                          .stage  = VK_SHADER_STAGE_VERTEX_BIT,
                                                          .module = vs,
                                                          .pName  = "main",
                                                      },
                                                      {
                                                          .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                          .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                          .module = fs,
                                                          .pName  = "main",
                                                      }};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,

        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions    = NULL,

        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions    = NULL,
    };
    VkPipelineInputAssemblyStateCreateInfo input_asm = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology               = cfg->topology,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };

    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,

        .polygonMode = cfg->polygon_mode,
        .cullMode    = cfg->cull_mode,
        .frontFace   = cfg->front_face,

        .lineWidth = 1.0f,

        .depthClampEnable        = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .depthBiasEnable         = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo msaa = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,

        .depthTestEnable  = cfg->depth_test_enable,
        .depthWriteEnable = cfg->depth_write_enable,
        .depthCompareOp   = cfg->depth_compare_op,

        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable     = VK_FALSE,
    };

    VkPipelineColorBlendAttachmentState blends[MAX_COLOR_ATTACHMENTS];

    forEach(i, cfg->color_attachment_count) {
        const ColorAttachmentBlend          *src = &cfg->blends[i];
        VkPipelineColorBlendAttachmentState *dst = &blends[i];

        dst->blendEnable = src->blend_enable;

        dst->srcColorBlendFactor = src->src_color;
        dst->dstColorBlendFactor = src->dst_color;
        dst->colorBlendOp        = src->color_op;

        dst->srcAlphaBlendFactor = src->src_alpha;
        dst->dstAlphaBlendFactor = src->dst_alpha;
        dst->alphaBlendOp        = src->alpha_op;

        dst->colorWriteMask = src->write_mask;
    }

    VkPipelineColorBlendStateCreateInfo blend_state = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = cfg->color_attachment_count,
        .pAttachments    = blends,
    };

    // ----------------------------
    // Dynamic states
    // ----------------------------

    VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dyn = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dyn_states,
    };

    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,

        .colorAttachmentCount    = cfg->color_attachment_count,
        .pColorAttachmentFormats = cfg->color_formats,

        .depthAttachmentFormat   = cfg->depth_format,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };

    VkGraphicsPipelineCreateInfo pipe = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,

        .pNext = &rendering,

        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vertex_input,
        .pInputAssemblyState = &input_asm,
        .pViewportState      = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState   = &msaa,
        .pDepthStencilState  = &depth,
        .pColorBlendState    = &blend_state,
        .pDynamicState       = &dyn,
        //
        // .layout = pipeline_layout_cache_build(renderer->device, &renderer->descriptor_layout_cache,
        // &renderer->pipeline_layout_cache,
        //                                       set_bindings, refl.binding_counts, refl.set_create_flags, set_flags,
        //                                       refl.set_count, refl.push_ranges, refl.push_count),

        .layout     = renderer->bindless_system.pipeline_layout,
        .renderPass = VK_NULL_HANDLE,
        .subpass    = 0,
    };

    VkPipeline pipeline;

    VkResult res =
        vkCreateGraphicsPipelines(renderer->devc.device, renderer->devc.pipeline_cache, 1, &pipe, NULL, &pipeline);

    if (res != VK_SUCCESS) {
        fprintf(stderr, "Failed to create graphics pipeline\n");
        abort();
    }
    free(vs_code);
    free(fs_code);

    return pipeline;
}

VkPipeline create_compute_pipeline(Renderer *renderer, const char *compute_path) {

    void  *code = NULL;
    size_t size = 0;

    if (!read_file(compute_path, &code, &size))
        abort();

    VkShaderModule                  module = create_shader_module(renderer->devc.device, code, size);
    VkPipelineLayout                layout = renderer->bindless_system.pipeline_layout;
    VkPipelineShaderStageCreateInfo stage  = {
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = module,
        .pName  = "main",
    };
    VkComputePipelineCreateInfo ci = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = stage,
        .layout = layout,
    };

    VkPipeline pipeline;

    VK_CHECK(vkCreateComputePipelines(renderer->devc.device, renderer->devc.pipeline_cache, 1, &ci, NULL, &pipeline));

    vkDestroyShaderModule(renderer->devc.device, module, NULL);
    free(code);

    return pipeline;
}

void vk_cmd_set_viewport_scissor(VkCommandBuffer cmd, VkExtent2D extent) {
    VkViewport vp = {
        .x        = 0.0f,
        .y        =       (float)extent.height ,
        .width    = (float)extent.width,
        .height   =- (float)extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D sc = {
        .offset = {0, 0},
        .extent = extent,
    };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

static void spv_to_slang(const char *spv, char *out) {
    const char *name = strrchr(spv, '/');
    name             = name ? name + 1 : spv;

    char tmp[256];
    strcpy(tmp, name);

    char *stage = strstr(tmp, ".vert");
    if (!stage)
        stage = strstr(tmp, ".frag");
    if (!stage)
        stage = strstr(tmp, ".comp");

    if (stage)
        *stage = '\0';

    sprintf(out, "shaders/%s.slang", tmp);
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool shader_change_matches_spv(const char *changed, const char *spv) {
    if (!changed || !spv)
        return false;

    char slang[256];
    spv_to_slang(spv, slang);

    const char *changed_name = path_basename(changed);
    const char *spv_name     = path_basename(spv);
    if (strcmp(changed_name, spv_name) == 0)
        return true;

    if (strstr(changed, slang))
        return true;

    const char *slang_name = path_basename(slang);

    return strcmp(changed_name, slang_name) == 0;
}
PipelineID pipeline_create_compute(Renderer *r, const char *path) {

    uint32_t id;
    mu_id_pool_create_id(&r->render_pipelines.pipeline_id_pool, &id);

    if (id >= MAX_PIPELINES) {
        printf("Pipeline overmu\n");
        debug_break();
    }

    PipelineEntry *e = &r->render_pipelines.entries[id];

    e->type         = PIPELINE_TYPE_COMPUTE;
    e->compute.path = path;
    e->dirty        = false;

    VkPipeline p = create_compute_pipeline(r, path);

    r->render_pipelines.pipelines[id] = p;

    r->render_pipelines.count++;
    return id;
}
PipelineID pipeline_create_graphics(Renderer *r, GraphicsPipelineConfig *cfg) {
    uint32_t id;
    mu_id_pool_create_id(&r->render_pipelines.pipeline_id_pool, &id);

    PipelineEntry *e = &r->render_pipelines.entries[id];

    e->type     = PIPELINE_TYPE_GRAPHICS;
    e->graphics = *cfg;
    e->dirty    = false;

    r->render_pipelines.pipelines[id] = create_graphics_pipeline(r, cfg);

    r->render_pipelines.count++;

    return id;
}
void pipeline_rebuild(Renderer *r) {
    bool any_dirty = false;

    for (int i = 0; i < r->render_pipelines.count; i++)
        if (r->render_pipelines.entries[i].dirty)
            any_dirty = true;

    if (!any_dirty)
        return;

    vkDeviceWaitIdle(r->devc.device);

    for (int i = 0; i < r->render_pipelines.count; i++) {
        PipelineEntry *e = &r->render_pipelines.entries[i];

        if (!e->dirty)
            continue;

        e->dirty = false;

        vkDestroyPipeline(r->devc.device, r->render_pipelines.pipelines[i], NULL);

        if (e->type == PIPELINE_TYPE_GRAPHICS)
            r->render_pipelines.pipelines[i] = create_graphics_pipeline(r, &e->graphics);
        else
            r->render_pipelines.pipelines[i] = create_compute_pipeline(r, e->compute.path);

        printf("Pipeline %d hot reloaded\n", i);
    }
}

void pipeline_mark_dirty(Renderer *r, const char *changed) {
    for (uint32_t i = 0; i < r->render_pipelines.count; i++) {
        PipelineEntry *e = &r->render_pipelines.entries[i];

        if (e->type != PIPELINE_TYPE_GRAPHICS && e->type != PIPELINE_TYPE_COMPUTE)
            continue;

        bool matches = false;

        if (e->type == PIPELINE_TYPE_GRAPHICS) {
            matches = shader_change_matches_spv(changed, e->graphics.vert_path) ||
                      shader_change_matches_spv(changed, e->graphics.frag_path);
        } else {
            matches = shader_change_matches_spv(changed, e->compute.path);
        }

        if (matches)
            e->dirty = true;
    }
}
static dmon_watch_id g_source_watch_id;

static void watch_callback(dmon_watch_id watch_id, dmon_action action, const char *rootdir, const char *filepath,
                           const char *oldfilepath, void *user) {
    Renderer *r = (Renderer *)user;

    if (action != DMON_ACTION_CREATE && action != DMON_ACTION_MODIFY && action != DMON_ACTION_MOVE) {
        return;
    }

    // --- Handle Source Shader Changes (e.g., in "shaders/") ---
    if (watch_id.id == g_source_watch_id.id) {
        // Check if it's a source file (e.g., .slang, .vert, .frag)
        const char *ext = strrchr(filepath, '.');
        if (ext && (strcmp(ext, ".slang") == 0 || strcmp(ext, ".vert") == 0 || strcmp(ext, ".frag") == 0)) {
            // Trigger the bash script
            if (trigger_shader_compilation()) {
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s", rootdir, filepath);
                pipeline_mark_dirty(r, full_path);
            }
        }
    }
    // --- Handle Compiled Shader Changes (e.g., in "compiledshaders/") ---
    else {
        // Check if it's a .spv file
        const char *ext = strrchr(filepath, '.');
        if (!ext || strcmp(ext, ".spv") != 0)
            return;

        // Construct path and mark dirty
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", rootdir, filepath);
        pipeline_mark_dirty(r, full_path);
    }
}

void image_transition_swapchain(Renderer *r, VkCommandBuffer cmd, FlowSwapchain *sc, VkImageLayout new_layout,
                                VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    uint32_t index = sc->current_image;

    ImageState *state = &sc->states[index];

    VkPipelineStageFlags2 src_stage;
    VkAccessFlags2        src_access;
    if (state->validity == IMAGE_STATE_UNDEFINED) {
        src_stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        src_access = 0;
    } else {
        src_stage  = state->stage;
        src_access = state->access;
    }
    VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,

                                     .srcStageMask  = src_stage,
                                     .srcAccessMask = src_access,

                                     .dstStageMask  = dst_stage,
                                     .dstAccessMask = dst_access,

                                     .oldLayout = state->layout,
                                     .newLayout = new_layout,

                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,

                                     .image = sc->images[index],

                                     .subresourceRange = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel   = 0,
                                                          .levelCount     = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount     = 1}};

    r->barrierbatch.image_barriers[r->barrierbatch.image_count++] = barrier;
    state->layout                                                 = new_layout;
    state->stage                                                  = dst_stage;
    state->access                                                 = dst_access;
    state->validity                                               = IMAGE_STATE_VALID;
}

static inline VkImageSubresourceRange image_subresource_range(VkImageAspectFlags aspect, uint32_t baseMip,
                                                              uint32_t mipCount) {
    VkImageSubresourceRange range = {.aspectMask     = aspect,
                                     .baseMipLevel   = baseMip,
                                     .levelCount     = mipCount,
                                     .baseArrayLayer = 0,
                                     .layerCount     = VK_REMAINING_ARRAY_LAYERS};

    return range;
}

inline void cmd_transition_all_mips(Renderer *r, VkCommandBuffer cmd, VkImage image, ImageState *state,
                                    VkImageAspectFlags aspect, uint32_t mipCount, VkPipelineStageFlags2 newStage,
                                    VkAccessFlags2 newAccess, VkImageLayout newLayout, uint32_t newQueueFamily) {
    if (state->validity == IMAGE_STATE_VALID) {
        if (state->stage == newStage && state->access == newAccess && state->layout == newLayout &&
            state->queue_family == newQueueFamily) {
            return;
        }
    }

    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,

        .srcStageMask = state->validity == IMAGE_STATE_VALID ? state->stage : VK_PIPELINE_STAGE_2_NONE,

        .srcAccessMask = state->validity == IMAGE_STATE_VALID ? state->access : VK_ACCESS_2_NONE,

        .dstStageMask  = newStage,
        .dstAccessMask = newAccess,

        .oldLayout = state->validity == IMAGE_STATE_VALID ? state->layout : VK_IMAGE_LAYOUT_UNDEFINED,

        .newLayout = newLayout,

        .srcQueueFamilyIndex = state->validity == IMAGE_STATE_EXTERNAL ? state->queue_family : VK_QUEUE_FAMILY_IGNORED,

        .dstQueueFamilyIndex = newQueueFamily,

        .image = image,

        .subresourceRange = image_subresource_range(aspect, 0, mipCount)};

    r->barrierbatch.image_barriers[r->barrierbatch.image_count++] = barrier;
    state->stage                                                  = newStage;
    state->access                                                 = newAccess;
    state->layout                                                 = newLayout;
    state->queue_family                                           = newQueueFamily;
    state->validity                                               = IMAGE_STATE_VALID;
    state->dirty_mips                                             = 0;
}

void cmd_transition_mip(Renderer *r, VkCommandBuffer cmd, VkImage image, ImageState *state, VkImageAspectFlags aspect,
                        uint32_t mip, VkPipelineStageFlags2 newStage, VkAccessFlags2 newAccess, VkImageLayout newLayout,
                        uint32_t newQueueFamily) {
    uint32_t bit = 1u << mip;

    if (state->validity == IMAGE_STATE_VALID) {
        if ((state->dirty_mips & bit) == 0 && state->stage == newStage && state->access == newAccess &&
            state->layout == newLayout && state->queue_family == newQueueFamily) {
            return;
        }
    }

    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,

        .srcStageMask = state->validity == IMAGE_STATE_VALID ? state->stage : VK_PIPELINE_STAGE_2_NONE,

        .srcAccessMask = state->validity == IMAGE_STATE_VALID ? state->access : VK_ACCESS_2_NONE,

        .dstStageMask  = newStage,
        .dstAccessMask = newAccess,

        .oldLayout = state->validity == IMAGE_STATE_VALID ? state->layout : VK_IMAGE_LAYOUT_UNDEFINED,

        .newLayout = newLayout,

        .srcQueueFamilyIndex = state->validity == IMAGE_STATE_EXTERNAL ? state->queue_family : VK_QUEUE_FAMILY_IGNORED,

        .dstQueueFamilyIndex = newQueueFamily,

        .image = image,

        .subresourceRange = image_subresource_range(aspect, mip, 1)};

    r->barrierbatch.image_barriers[r->barrierbatch.image_count++] = barrier;

    state->stage        = newStage;
    state->access       = newAccess;
    state->layout       = newLayout;
    state->queue_family = newQueueFamily;
    state->validity     = IMAGE_STATE_VALID;

    state->dirty_mips &= ~bit;
}
void flush_barriers(Renderer *r, VkCommandBuffer cmd) {

    if (r->barrierbatch.image_count == 0)
        return;

    VkDependencyInfo dep = {.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .imageMemoryBarrierCount = r->barrierbatch.image_count,
                            .pImageMemoryBarriers    = r->barrierbatch.image_barriers};

    vkCmdPipelineBarrier2(cmd, &dep);

    r->barrierbatch.image_count = 0;
}

MU_INLINE void rt_transition_mip(Renderer *r, VkCommandBuffer cmd, RenderTarget *rt, uint32_t mip,
                                 VkImageLayout new_layout, VkPipelineStageFlags2 new_stage, VkAccessFlags2 new_access

) {
    assert(mip < rt->mip_count);
    cmd_transition_mip(r, cmd, rt->image, &rt->mip_states[mip], rt->aspect, mip, new_stage, new_access, new_layout,
                       VK_QUEUE_FAMILY_IGNORED);
}

MU_INLINE void rt_transition_all(Renderer *r, VkCommandBuffer cmd, RenderTarget *rt, VkImageLayout new_layout,
                                 VkPipelineStageFlags2 new_stage, VkAccessFlags2 new_access) {
    for (uint32_t mip = 0; mip < rt->mip_count; mip++) {
        ImageState *s = &rt->mip_states[mip];
        // Skip if already in target state
        if (s->validity == IMAGE_STATE_VALID && s->stage == new_stage && s->access == new_access &&
            s->layout == new_layout) {
            continue;
        }
        cmd_transition_mip(r, cmd, rt->image, s, rt->aspect, mip, new_stage, new_access, new_layout,
                           VK_QUEUE_FAMILY_IGNORED);
    }
}

void renderer_create(Renderer *r, RendererDesc *desc) {
    TracyCZoneN(ctx, "renderer_create", 1);
    // Instance
    // Debug messenger
    // Physical device
    // Device info
    // Logical device
    // Queues
    // Frame contexts

    {

        VkApplicationInfo app = {.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                 .pApplicationName   = desc->app_name,
                                 .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                 .pEngineName        = "MU",
                                 .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
                                 .apiVersion         = VK_API_VERSION_1_3

        };

        const char *extensions[64];
        uint32_t    ext_count = 0;

        forEach(i, desc->instance_extension_count) { extensions[ext_count++] = desc->instance_extensions[i]; }

        extensions[ext_count++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;

        if (desc->enable_validation) {
            if (!is_instance_extension_supported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                log_warn("[instance] %s not supported by loader", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }
            extensions[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        }

        const char *layers[8];
        uint32_t    layer_count = desc->instance_layer_count;

        if (layer_count) {
            memcpy(layers, desc->instance_layers, sizeof(char *) * layer_count);
        }
        if (desc->enable_validation) {
            if (!is_instance_layer_supported("VK_LAYER_KHRONOS_validation")) {
                log_warn("[instance] VK_LAYER_KHRONOS_validation not present");
            }
            layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
        }

        VkValidationFeaturesEXT validation_features;
        memset(&validation_features, 0, sizeof(validation_features));
        validation_features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        // todo get this from rendererdesc may be
        static const VkValidationFeatureEnableEXT enabled_features[] = {
            VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
            VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT,
            VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT, VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT,
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};

        log_info("[instance] app=%s api=1.3 validation=%u gpu_validation=%u",
                 desc->app_name ? desc->app_name : "(null)", desc->enable_validation,
                 desc->enable_gpu_based_validation);
        forEach(i, ext_count) log_info("[instance]  ext[%u]=%s", i, extensions[i]);

        if (layer_count > 0) {
            log_info("[instance] layers: %u", layer_count);
            forEach(i, layer_count) log_info("[instance]  layer[%u]=%s", i, layers[i]);
        }
        VkInstanceCreateInfo ci = {.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                   .pApplicationInfo        = &app,
                                   .enabledExtensionCount   = ext_count,
                                   .ppEnabledExtensionNames = extensions,
                                   .enabledLayerCount       = layer_count,
                                   .ppEnabledLayerNames     = layers};
        if (desc->enable_validation && desc->enable_gpu_based_validation) {
            validation_features.enabledValidationFeatureCount =
                (uint32_t)(sizeof(enabled_features) / sizeof(enabled_features[0]));
            validation_features.pEnabledValidationFeatures = enabled_features;

            validation_features.pNext = ci.pNext;
            ci.pNext                  = &validation_features;
        }
        VK_CHECK(vkCreateInstance(&ci, r->vk_allocator_callbacks, &r->instance.instance));
        volkLoadInstance(r->instance.instance);
        log_info("[renderer] instance created");
    }

    {

        //
        // 2. Debug Messenger
        //
        if (desc->enable_validation) {
            VkDebugUtilsMessengerCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,

                                                     .messageSeverity = desc->validation_severity,

                                                     .messageType = desc->validation_types,

                                                     .pfnUserCallback = debug_callback};

            {
                vkCreateDebugUtilsMessengerEXT(r->instance.instance, &ci, r->vk_allocator_callbacks,
                                               &r->instance.debug_messenger);

                log_info("[renderer] debug messenger created");

                {
                    VkDebugUtilsMessengerCallbackDataEXT data = {
                        .sType    = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,
                        .pMessage = "Validation is enabled and debug messenger is active",
                    };
                    vkSubmitDebugUtilsMessageEXT(r->instance.instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                                                 VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &data);
                }
            }
        }
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    r->window = glfwCreateWindow(desc->width, desc->height, "Vulkan", NULL, NULL);

    VK_CHECK(glfwCreateWindowSurface(r->instance.instance, r->window, NULL, &r->surface));
    //
    // 3. Pick Physical Device
    //
    {
        r->devc.physical_device = pick_physical_device(r->instance.instance, r->surface, desc);

        if (r->devc.physical_device == VK_NULL_HANDLE) {
            log_error("No GPU found");
            return;
        }

        vkGetPhysicalDeviceProperties(r->devc.physical_device, &r->info.properties);

        vkGetPhysicalDeviceMemoryProperties(r->devc.physical_device, &r->info.memory);

        log_info("[renderer] GPU: %s", r->info.properties.deviceName);
    }

    // 4. Query Features
    //
    if (desc->use_custom_features) {
        r->info.feature_chain = desc->custom_features;
    } else {
        query_device_features(r->devc.physical_device, &r->info.feature_chain);

        RendererCaps caps = default_caps();

        // Enable debug printf if requested
        if (desc->enable_debug_printf) {
            caps.debug_printf = true;
        }

        if (desc->enable_pipeline_stats) {
            caps.pipeline_statistics_query = true;
        }

        apply_caps(&r->info.feature_chain, &caps);
    }

    if (!desc->use_custom_features) {
        r->info.feature_chain.core.pNext = &r->info.feature_chain.v11;
        r->info.feature_chain.v11.pNext  = &r->info.feature_chain.v12;
        r->info.feature_chain.v12.pNext  = &r->info.feature_chain.v13;
        r->info.feature_chain.v13.pNext  = NULL;

        VkBaseOutStructure *tail = (VkBaseOutStructure *)&r->info.feature_chain.v13;

        if (device_has_extension(r->devc.physical_device, VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) {
            r->info.feature_chain.maintenance5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;
            r->info.feature_chain.maintenance5.pNext = NULL;
            tail->pNext                              = (VkBaseOutStructure *)&r->info.feature_chain.maintenance5;
            tail                                     = (VkBaseOutStructure *)&r->info.feature_chain.maintenance5;
        }

        if (desc->enable_debug_printf &&
            device_has_extension(r->devc.physical_device, VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME)) {
        }

        tail->pNext = NULL;
    }

    //
    // 5. Find Queue Families
    //
    queue_families q;
    find_queue_families(r->devc.physical_device, r->surface, &q);

    r->devc.graphics_queue_index = q.graphics_family;

    r->devc.present_queue_index = q.present_family;

    r->devc.compute_queue_index = q.has_compute ? q.compute_family : q.graphics_family;

    r->devc.transfer_queue_index = q.has_transfer ? q.transfer_family : q.graphics_family;

    //
    // 6. Create Logical Device
    //
    {
        float priority = 1.0f;

        uint32_t unique[4];
        uint32_t count = 0;

        unique[count++] = r->devc.graphics_queue_index;

        if (r->devc.present_queue_index != r->devc.graphics_queue_index)
            unique[count++] = r->devc.present_queue_index;

        if (r->devc.compute_queue_index != r->devc.graphics_queue_index)
            unique[count++] = r->devc.compute_queue_index;

        if (r->devc.transfer_queue_index != r->devc.graphics_queue_index)
            unique[count++] = r->devc.transfer_queue_index;

        VkDeviceQueueCreateInfo queues[4];

        forEach(i, count) {
            queues[i] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,

                                                  .queueFamilyIndex = unique[i],

                                                  .queueCount = 1,

                                                  .pQueuePriorities = &priority};
        }

        const char *extensions[32];
        uint32_t    ext_count = 0;

        forEach(i, desc->device_extension_count) { extensions[ext_count++] = desc->device_extensions[i]; }

        if (device_has_extension(r->devc.physical_device, VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) {
            extensions[ext_count++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
        }

        if (desc->enable_debug_printf &&
            device_has_extension(r->devc.physical_device, VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME)) {
            extensions[ext_count++] = VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME;
            log_info("[renderer] VK_KHR_shader_non_semantic_info enabled for Debug Printf");
        } else if (desc->enable_debug_printf) {
            log_info("[renderer] VK_KHR_shader_non_semantic_info not available - Debug Printf disabled");
        }

        VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,

                                 .pNext = &r->info.feature_chain.core,

                                 .queueCreateInfoCount = count,

                                 .pQueueCreateInfos = queues,

                                 .enabledExtensionCount = ext_count,

                                 .ppEnabledExtensionNames = extensions};

        VK_CHECK(vkCreateDevice(r->devc.physical_device, &ci, r->vk_allocator_callbacks, &r->devc.device));

        volkLoadDevice(r->devc.device);
        log_info("[renderer] logical device created");
    }

    //
    // 7. Get Queues
    //
    vkGetDeviceQueue(r->devc.device, r->devc.graphics_queue_index, 0, &r->devc.graphics_queue);
    vkGetDeviceQueue(r->devc.device, r->devc.present_queue_index, 0, &r->devc.present_queue);

    vkGetDeviceQueue(r->devc.device, r->devc.compute_queue_index, 0, &r->devc.compute_queue);

    vkGetDeviceQueue(r->devc.device, r->devc.transfer_queue_index, 0, &r->devc.transfer_queue);

    log_info("[renderer] queues acquired");

    //
    // 8. Create Frame Contexts
    //
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        FrameContext *f = &r->frames[i];
        vk_cmd_create_pool(r->devc.device, r->devc.graphics_queue_index, true, false, &f->cmdbufpool);
        vk_cmd_alloc(r->devc.device, f->cmdbufpool, true, &f->cmdbuf);
        vk_create_semaphore(r->devc.device, &f->image_available_semaphore);
        vk_create_fence(r->devc.device, true, &f->in_flight_fence);
    }

    log_info("[renderer] frame contexts created");

    r->current_frame     = 0;
    r->cpu_prev_frame    = mu_time_now();
    r->cpu_frame_ns      = 0.0;
    r->cpu_active_ns     = 0.0;
    r->cpu_wait_ns       = 0.0;
    r->cpu_wait_accum_ns = 0.0;

    log_info("[renderer] initialization complete");

    mu_id_pool_init(&r->texture_system.id_pool, MAX_BINDLESS_TEXTURES);

    mu_id_pool_init(&r->sampler_pool, MAX_BINDLESS_SAMPLERS);

    mu_id_pool_init(&r->render_pipelines.pipeline_id_pool, MAX_PIPELINES);
    vk_cmd_create_pool(r->devc.device, r->devc.graphics_queue_index, true, false, &r->one_time_gfx_pool);

    int fb_w, fb_h;
    glfwGetFramebufferSize(r->window, &fb_w, &fb_h);
    //  descriptor_layout_cache_init(&r->descriptor_layout_cache);
    // pipeline_layout_cache_init(&r->pipeline_layout_cache);
    r->devc.pipeline_cache =
        pipeline_cache_load_or_create(r->devc.device, r->devc.physical_device, "pipeline_cache.bin");
    VkDescriptorSetLayoutBinding bindings[] = {// textures
                                               {
                                                   .binding         = BINDLESS_TEXTURE_BINDING,
                                                   .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                   .descriptorCount = desc->bindless_sampled_image_count,
                                                   .stageFlags      = VK_SHADER_STAGE_ALL,
                                               },

                                               // samplers
                                               {
                                                   .binding         = BINDLESS_SAMPLER_BINDING,
                                                   .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
                                                   .descriptorCount = desc->bindless_sampler_count,
                                                   .stageFlags      = VK_SHADER_STAGE_ALL,
                                               },

                                               // storage images
                                               {
                                                   .binding         = BINDLESS_STORAGE_IMAGE_BINDING,
                                                   .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                   .descriptorCount = desc->bindless_storage_image_count,
                                                   .stageFlags      = VK_SHADER_STAGE_ALL,
                                               },
                                               {
                                                   .binding         = GLOBAL_DATA_BINDING,
                                                   .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                   .descriptorCount = 1,
                                                   .stageFlags      = VK_SHADER_STAGE_ALL,
                                               }};
    VkDescriptorBindingFlags     flags[ARRAY_COUNT(bindings)];

    forEach(i, ARRAY_COUNT(flags)) {
        flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                   VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo ext = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount  = ARRAY_COUNT(flags),
        .pBindingFlags = flags,
    };

    VkDescriptorSetLayoutCreateInfo ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext        = &ext,
        .bindingCount = ARRAY_COUNT(bindings),
        .pBindings    = bindings,
        .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
    };

    vkCreateDescriptorSetLayout(r->devc.device, &ci, NULL, &r->bindless_system.set_layout);

    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, desc->bindless_sampled_image_count},
        {VK_DESCRIPTOR_TYPE_SAMPLER, desc->bindless_sampler_count},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, desc->bindless_storage_image_count},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo cib = {
        .sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags   = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = ARRAY_COUNT(sizes),
        .pPoolSizes    = sizes,
    };

    vkCreateDescriptorPool(r->devc.device, &cib, NULL, &r->bindless_system.pool);

    VkDescriptorSetAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = r->bindless_system.pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &r->bindless_system.set_layout,
    };

    vkAllocateDescriptorSets(r->devc.device, &ai, &r->bindless_system.set);

    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_ALL,
        .offset     = 0,
        .size       = 256 // your device limit
    };

    VkPipelineLayoutCreateInfo playoutci = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &r->bindless_system.set_layout,

        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push_range,
    };

    VK_CHECK(vkCreatePipelineLayout(r->devc.device, &playoutci, NULL, &r->bindless_system.pipeline_layout));

    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice         = r->devc.physical_device;
    allocatorInfo.device                 = r->devc.device;
    allocatorInfo.instance               = r->instance.instance;

    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;

    //  use VMA_DYNAMIC_VULKAN_FUNCTIONS
    VmaVulkanFunctions vulkanFunctions = {
        .vkGetInstanceProcAddr                   = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr                     = vkGetDeviceProcAddr,
        .vkGetPhysicalDeviceProperties           = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties     = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory                        = vkAllocateMemory,
        .vkFreeMemory                            = vkFreeMemory,
        .vkMapMemory                             = vkMapMemory,
        .vkUnmapMemory                           = vkUnmapMemory,
        .vkFlushMappedMemoryRanges               = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges          = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory                      = vkBindBufferMemory,
        .vkBindImageMemory                       = vkBindImageMemory,
        .vkGetBufferMemoryRequirements           = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements            = vkGetImageMemoryRequirements,
        .vkCreateBuffer                          = vkCreateBuffer,
        .vkDestroyBuffer                         = vkDestroyBuffer,
        .vkCreateImage                           = vkCreateImage,
        .vkDestroyImage                          = vkDestroyImage,
        .vkCmdCopyBuffer                         = vkCmdCopyBuffer,
        .vkGetBufferMemoryRequirements2KHR       = vkGetBufferMemoryRequirements2,
        .vkGetImageMemoryRequirements2KHR        = vkGetImageMemoryRequirements2,
        .vkBindBufferMemory2KHR                  = vkBindBufferMemory2,
        .vkBindImageMemory2KHR                   = vkBindImageMemory2,
        .vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2,
        .vkGetDeviceBufferMemoryRequirements     = vkGetDeviceBufferMemoryRequirements,
        .vkGetDeviceImageMemoryRequirements      = vkGetDeviceImageMemoryRequirements,
    };
    allocatorInfo.pVulkanFunctions = &vulkanFunctions;

    vmaCreateAllocator(&allocatorInfo, &r->devc.vmaallocator);
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_COMBINED_IMAGE_COUNT},
        {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_SAMPLER_COUNT},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, IMGUI_UBO_COUNT},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, IMGUI_SSBO_COUNT},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_SAMPLED_IMAGE_COUNT},
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = 1000,
        .poolSizeCount = ARRAY_COUNT(pool_sizes),
        .pPoolSizes    = pool_sizes,
    };

    vkCreateDescriptorPool(r->devc.device, &pool_info, NULL, &r->imgui_descriptor_pool);
    VkFormat                depth_format = pick_depth_format(r->devc.physical_device);
    VkFormat                hdr_format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    FlowSwapchainCreateInfo sci          = {.surface         = r->surface,
                                            .width           = fb_w,
                                            .height          = fb_h,
                                            .min_image_count = 3,

                                            //.preferred_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR,
                                            .preferred_format      = desc->swapchain_preferred_format,
                                            .preferred_color_space = desc->swapchain_preferred_color_space,
                                            .extra_usage           = desc->swapchain_extra_usage_flags,
                                            .old_swapchain         = VK_NULL_HANDLE};

    if (desc->swapchain_preferred_present_mode) {

        sci.preferred_present_mode = desc->swapchain_preferred_present_mode;
    } else {
        sci.preferred_present_mode = vk_swapchain_select_present_mode(r->devc.physical_device, r->surface, desc->vsync);
    }

    vk_create_swapchain(r->devc.device, r->devc.physical_device, &r->swapchain, &sci, r->devc.graphics_queue,
                        r->one_time_gfx_pool, r);

    RenderTargetSpec depth_spec = {.width  = r->swapchain.extent.width,
                                   .height = r->swapchain.extent.height,
                                   .layers = 1,

                                   .format = depth_format,

                                   .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,

                                   .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,

                                   .mip_count  = 1,
                                   .debug_name = "depth_buffer"};
    RenderTargetSpec hdr_spec   = {.width      = r->swapchain.extent.width,
                                   .height     = r->swapchain.extent.height,
                                   .layers     = 1,
                                   .format     = hdr_format,
                                   .usage      = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                                   .mip_count  = 1,
                                   .debug_name = "hdr_color"};
    RenderTargetSpec ldr_spec   = {.width  = r->swapchain.extent.width,
                                   .height = r->swapchain.extent.height,
                                   .layers = 1,

                                   .format = VK_FORMAT_R8G8B8A8_UNORM,

                                   .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |

                                            VK_IMAGE_USAGE_STORAGE_BIT |      // compute writes tonemap result
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | // copy → swapchain
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | // optional clears
                                            VK_IMAGE_USAGE_SAMPLED_BIT,       // future post effects

                                   .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                   .mip_count  = 1,
                                   .debug_name = "ldr_color"};
    RenderTargetSpec smaa_final_spec = ldr_spec;
    smaa_final_spec.debug_name       = "smaa_final";

    RenderTargetSpec smaa_edge_spec = {.width  = r->swapchain.extent.width,
                                       .height = r->swapchain.extent.height,
                                       .layers = 1,

                                       .format = VK_FORMAT_R8G8_UNORM,

                                       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,

                                       .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                       .mip_count  = 1,
                                       .debug_name = "smaa_edges"};

    RenderTargetSpec smaa_weight_spec = {.width  = r->swapchain.extent.width,
                                         .height = r->swapchain.extent.height,
                                         .layers = 1,

                                         .format = VK_FORMAT_R8G8B8A8_UNORM,

                                         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,

                                         .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                         .mip_count  = 1,
                                         .debug_name = "smaa_weights"};
    imgui_init(r->window, r->instance.instance, r->devc.physical_device, r->devc.device, r->devc.graphics_queue_index,
               r->devc.graphics_queue, r->imgui_descriptor_pool, r->swapchain.image_count, r->swapchain.image_count,
               VK_FORMAT_B8G8R8A8_SRGB, depth_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

#include "external/smaa/Textures/AreaTex.h"

#include "external/smaa/Textures/SearchTex.h"

    {

        TextureCreateDesc desc = {.width     = AREATEX_WIDTH,
                                  .height    = AREATEX_HEIGHT,
                                  .mip_count = 1,
                                  .format    = VK_FORMAT_R8G8_UNORM,
                                  .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

        TextureID id  = create_texture(r, &desc);
        Texture  *tex = &r->texture_system.textures[id];

        VkDeviceSize size = AREATEX_SIZE;

        Buffer staging;
        create_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

        memcpy(staging.mapping, areaTexBytes, size);

        VkCommandBuffer cmd = vk_begin_one_time_cmd(r->devc.device, r->one_time_gfx_pool);

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

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier);

        VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .imageSubresource.layerCount = 1,
                                    .imageExtent                 = {AREATEX_WIDTH, AREATEX_HEIGHT, 1}};

        vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier);

        vk_end_one_time_cmd(r->devc.device, r->devc.graphics_queue, r->one_time_gfx_pool, cmd);

        destroy_buffer(r, &staging);
    }
    {

        TextureID smaa_area;
        TextureID smaa_search;

        {
            TextureCreateDesc desc = {.width     = AREATEX_WIDTH,
                                      .height    = AREATEX_HEIGHT,
                                      .mip_count = 1,
                                      .format    = VK_FORMAT_R8G8_UNORM,
                                      .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

            smaa_area    = create_texture(r, &desc);
            Texture *tex = &r->texture_system.textures[smaa_area];

            VkDeviceSize size = AREATEX_SIZE;

            Buffer staging;
            create_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

            memcpy(staging.mapping, areaTexBytes, size);

            VkCommandBuffer cmd = vk_begin_one_time_cmd(r->devc.device, r->one_time_gfx_pool);

            VkImageMemoryBarrier barrier = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                            .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            .srcAccessMask               = 0,
                                            .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .image                       = tex->image,
                                            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                            .subresourceRange.levelCount = 1,
                                            .subresourceRange.layerCount = 1};

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &barrier);

            VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .imageSubresource.layerCount = 1,
                                        .imageExtent                 = {AREATEX_WIDTH, AREATEX_HEIGHT, 1}};

            vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            vk_end_one_time_cmd(r->devc.device, r->devc.graphics_queue, r->one_time_gfx_pool, cmd);

            destroy_buffer(r, &staging);
        }

        {
            TextureCreateDesc desc = {.width     = SEARCHTEX_WIDTH,
                                      .height    = SEARCHTEX_HEIGHT,
                                      .mip_count = 1,
                                      .format    = VK_FORMAT_R8_UNORM,
                                      .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

            smaa_search  = create_texture(r, &desc);
            Texture *tex = &r->texture_system.textures[smaa_search];

            VkDeviceSize size = SEARCHTEX_SIZE;

            Buffer staging;
            create_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

            memcpy(staging.mapping, searchTexBytes, size);

            VkCommandBuffer cmd = vk_begin_one_time_cmd(r->devc.device, r->one_time_gfx_pool);

            VkImageMemoryBarrier barrier = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                            .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            .srcAccessMask               = 0,
                                            .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .image                       = tex->image,
                                            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                            .subresourceRange.levelCount = 1,
                                            .subresourceRange.layerCount = 1};

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &barrier);

            VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .imageSubresource.layerCount = 1,
                                        .imageExtent                 = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1}};

            vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            vk_end_one_time_cmd(r->devc.device, r->devc.graphics_queue, r->one_time_gfx_pool, cmd);

            destroy_buffer(r, &staging);
        }

        r->smaa_area_tex   = smaa_area;
        r->smaa_search_tex = smaa_search;
    }
    forEach(i, r->swapchain.image_count) {
        rt_create(r, &r->depth[i], &depth_spec);
        rt_create(r, &r->hdr_color[i], &hdr_spec);
        rt_create(r, &r->ldr_color[i], &ldr_spec);
        rt_create(r, &r->smaa_final[i], &smaa_final_spec);

        rt_create(r, &r->smaa_edges[i], &smaa_edge_spec);
        rt_create(r, &r->smaa_weights[i], &smaa_weight_spec);
    }
    {
        const char *path = "data/dummy_texture.png";

        uint32_t       w, h, c;
        unsigned char *pixels = stbi_load(path, &w, &h, &c, 4);
        if (!pixels) {
            fprintf(stderr, "Failed to load %s\n", path);
        }
        TextureCreateDesc desc = {.width     = w,
                                  .height    = h,
                                  .mip_count = 1,
                                  .format    = VK_FORMAT_R8G8B8A8_UNORM,
                                  .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                               VK_IMAGE_USAGE_STORAGE_BIT};
        TextureID         id   = create_texture(r, &desc);
        Texture          *tex  = &r->texture_system.textures[id];

        TextureInfo *texinfo    = &r->texture_system.info[id];
        VkDeviceSize image_size = w * h * 4;
        Buffer       staging;
        create_buffer(r, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, &staging);

        memcpy(staging.mapping, pixels, image_size);
        stbi_image_free(pixels);
        VkCommandBuffer      cmd      = vk_begin_one_time_cmd(r->devc.device, r->one_time_gfx_pool);
        VkImageMemoryBarrier barrier1 = {
            .sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcAccessMask               = 0,
            .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
            .image                       = tex->image,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = texinfo->mip_count,
            .subresourceRange.layerCount = 1,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier1);
        VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .imageSubresource.layerCount = 1,
                                    .imageExtent                 = {w, h, 1}};
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier barrier2 = barrier1;
        barrier2.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier2.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier2.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier2.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier2);
        vk_end_one_time_cmd(r->devc.device, r->devc.graphics_queue, r->one_time_gfx_pool, cmd);
        destroy_buffer(r, &staging);

        r->dummy_texture = id;
    };
    {
        forEach(i, MAX_FRAMES_IN_FLIGHT) {
            gpu_profiler_init(&r->gpuprofiler[i], r->devc.device, r->info.properties.limits.timestampPeriod,
                              desc->enable_pipeline_stats &&
                                  r->info.feature_chain.core.features.pipelineStatisticsQuery);
        }
    }

    memset(&r->default_samplers, 0, sizeof(r->default_samplers));

    {
        DefaultSamplerTable *table = &r->default_samplers;

        VkSamplerCreateInfo ci = {
            .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter    = VK_FILTER_LINEAR,
            .minFilter    = VK_FILTER_LINEAR,
            .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .minLod       = 0.0f,
            .maxLod       = VK_LOD_CLAMP_NONE,
        };

        // Linear wrap
        sampler_create(r, &ci, &table->samplers[SAMPLER_LINEAR_WRAP]);

        // Linear clamp
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_create(r, &ci, &table->samplers[SAMPLER_LINEAR_CLAMP]);

        // Nearest wrap
        ci.magFilter    = VK_FILTER_NEAREST;
        ci.minFilter    = VK_FILTER_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_create(r, &ci, &table->samplers[SAMPLER_NEAREST_WRAP]);

        // Nearest clamp
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_create(r, &ci, &table->samplers[SAMPLER_NEAREST_CLAMP]);

        // Anisotropic wrap
        ci.magFilter        = VK_FILTER_LINEAR;
        ci.minFilter        = VK_FILTER_LINEAR;
        ci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy    = 16.0f;
        sampler_create(r, &ci, &table->samplers[SAMPLER_LINEAR_WRAP_ANISO]);

        // Shadow sampler
        ci.anisotropyEnable = VK_FALSE;
        ci.compareEnable    = VK_TRUE;
        ci.compareOp        = VK_COMPARE_OP_LESS_OR_EQUAL;
        ci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler_create(r, &ci, &table->samplers[SAMPLER_SHADOW]);
    }
    {
        VkDeviceSize size = r->swapchain.extent.width * r->swapchain.extent.height * 4; // RGBA8

        create_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                      &r->readback_buffer);
    }
    {
        forEach(i, MAX_FRAMES_IN_FLIGHT) {
            create_buffer(r, sizeof(GlobalData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
                          &r->global_ubo[i]);
        }
    }

    {
        buffer_pool_init(r, BUFFER_POOL_LINEAR, &r->cpu_pool, desc->size_of_cpu_pool,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                         2048);
        buffer_pool_init(r, BUFFER_POOL_TLSF, &r->gpu_pool, desc->size_of_gpu_pool,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                         VMA_MEMORY_USAGE_GPU_ONLY, 0, 2048);
        buffer_pool_init(r, BUFFER_POOL_RING, &r->staging_pool, desc->size_of_staging_pool,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                         2048);

        VkBufferDeviceAddressInfo addrInfo = {.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                              .buffer = r->gpu_pool.buffer};
        r->gpu_base_addr                   = vkGetBufferDeviceAddress(r->devc.device, &addrInfo);
    }

    {

        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/fire.vert.spv";
            cfg.frag_path              = "compiledshaders/fire.frag.spv";
            cfg.depth_test_enable      = false;
            cfg.depth_write_enable     = false;
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &r->hdr_color[0].format;

            r->EnginePipelines.fire = pipeline_create_graphics(r, &cfg);
        }

        {
            r->EnginePipelines.postprocess = pipeline_create_compute(r, "compiledshaders/postprocess.comp.spv");
        }

        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/smaa_edge.vert.spv";
            cfg.frag_path              = "compiledshaders/smaa_edge.frag.spv";
            cfg.depth_test_enable      = false;
            cfg.depth_write_enable     = false;
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &r->smaa_edges[0].format;

            r->smaa_pipelines.smaa_edge = pipeline_create_graphics(r, &cfg);
        }

        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/smaa_weight.vert.spv";
            cfg.frag_path              = "compiledshaders/smaa_weight.frag.spv";
            cfg.depth_test_enable      = false;
            cfg.depth_write_enable     = false;
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &r->smaa_weights[0].format;

            r->smaa_pipelines.smaa_weight = pipeline_create_graphics(r, &cfg);
        }

        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/smaa_blend.vert.spv";
            cfg.frag_path              = "compiledshaders/smaa_blend.frag.spv";
            cfg.depth_test_enable      = false;
            cfg.depth_write_enable     = false;
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &r->smaa_final[0].format;

            r->smaa_pipelines.smaa_blend = pipeline_create_graphics(r, &cfg);
        }
    }
}

static Renderer *g_renderer = NULL;

void graphics_init(void) {
    VK_CHECK(volkInitialize());
    if (!is_instance_extension_supported("VK_KHR_wayland_surface"))
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    else
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    glfwInit();
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};

    uint32_t     glfw_ext_count = 0;
    const char **glfw_exts      = glfwGetRequiredInstanceExtensions(&glfw_ext_count);

    RendererDesc desc = {
        .app_name            = "My Renderer",
        .instance_layers     = NULL,
        .instance_extensions = glfw_exts,
        .device_extensions   = dev_exts,

        .instance_layer_count        = 0,
        .instance_extension_count    = glfw_ext_count,
        .device_extension_count      = 2,
        .enable_gpu_based_validation = true,
        .enable_validation           =  true,

        .validation_severity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .validation_types = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .width            = 1362,
        .height           = 749,

        .swapchain_preferred_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
        .swapchain_preferred_format      = VK_FORMAT_B8G8R8A8_SRGB,
        .swapchain_extra_usage_flags =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .vsync               = false,
        .enable_debug_printf = false,

        .bindless_sampled_image_count     = MAX_BINDLESS_TEXTURES,
        .bindless_sampler_count           = MAX_BINDLESS_SAMPLERS,
        .bindless_storage_image_count     = 16384,
        .enable_pipeline_stats            = true,
        .swapchain_preferred_present_mode = VK_PRESENT_MODE_MAILBOX_KHR,

        .size_of_cpu_pool     = MB(32),
        .size_of_gpu_pool     = MB(512),
        .size_of_staging_pool = MB(128),

    };

    g_renderer = malloc(sizeof(*g_renderer));
    MU_SCOPE_TIMER("Renderer Creation") { renderer_create(g_renderer, &desc); }

    // gfx_pipelines();
}
FORCE_INLINE bool vk_swapchain_acquire(VkDevice device, FlowSwapchain *sc, VkSemaphore image_available, VkFence fence,
                                       uint64_t timeout) {
    ///  PFN_vkAcquireNextImage2KHR
    VkResult r = vkAcquireNextImageKHR(device, sc->swapchain, timeout, image_available, fence, &sc->current_image);

    if (r == VK_SUCCESS)
        return true;

    if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
        sc->needs_recreate = true;
        return false;
    }

    VK_CHECK(r);
    return false;
}

FORCE_INLINE bool vk_swapchain_present(VkQueue present_queue, FlowSwapchain *sc, const VkSemaphore *waits,
                                       uint32_t wait_count) {
    VkPresentInfoKHR info = {.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                             .waitSemaphoreCount = wait_count,
                             .pWaitSemaphores    = waits,
                             .swapchainCount     = 1,
                             .pSwapchains        = &sc->swapchain,
                             .pImageIndices      = &sc->current_image};

    VkResult r = vkQueuePresentKHR(present_queue, &info);

    if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
        sc->needs_recreate = true;
        return false;
    }

    VK_CHECK(r);
    return true;
}

#define GPU_PROF_HISTORY_SIZE 128
#define MAX_RECORDED_PASSES   32

typedef struct GpuPassStats {
    char     name[64];
    double   time_ms;
    double   avg_ms;
    double   min_ms;
    double   max_ms;
    uint64_t vs_invocations;
    uint64_t fs_invocations;
    uint64_t primitives;
    float    history[GPU_PROF_HISTORY_SIZE];
    uint32_t history_idx;
} GpuPassStats;

typedef struct GpuProfilerUIState {
    bool         open;
    bool         paused;
    bool         show_pipeline_stats;
    uint32_t     pass_count;
    double       total_gpu_time_ms;
    double       avg_total_gpu_time_ms;
    float        total_history[GPU_PROF_HISTORY_SIZE];
    uint32_t     total_history_idx;
    GpuPassStats pass_stats[MAX_RECORDED_PASSES];
    uint32_t     frame_counter;
} GpuProfilerUIState;

static double ns_to_ms(double ns) { return ns / 1000000.0; }

static GpuProfilerUIState g_gpu_profiler_ui = {
    .open                = false,
    .paused              = false,
    .show_pipeline_stats = true,
};

static void gpu_profiler_ui_update(GpuProfiler *p) {
    if (g_gpu_profiler_ui.paused || !p || p->pass_count == 0)
        return;

    g_gpu_profiler_ui.pass_count = MIN(p->pass_count, MAX_RECORDED_PASSES);
    double total_ms              = 0.0;

    for (uint32_t i = 0; i < p->pass_count && i < MAX_RECORDED_PASSES; i++) {
        GpuPass      *pass = &p->passes[i];
        GpuPassStats *ps   = &g_gpu_profiler_ui.pass_stats[i];

        if (pass->name) {
            strncpy(ps->name, pass->name, sizeof(ps->name) - 1);
            ps->name[sizeof(ps->name) - 1] = '\0';
        } else {
            snprintf(ps->name, sizeof(ps->name), "Pass %u", i);
        }

        double ms = pass->time_ms;
        if (ms < 0.0)
            ms = 0.0;
        ps->time_ms = ms;
        total_ms += ms;

        if (ps->avg_ms == 0.0) {
            ps->avg_ms = ms;
            ps->min_ms = ms;
            ps->max_ms = ms;
        } else {
            ps->avg_ms = ps->avg_ms * 0.95 + ms * 0.05;
            if (ms < ps->min_ms)
                ps->min_ms = ms;
            if (ms > ps->max_ms)
                ps->max_ms = ms;
        }

        ps->vs_invocations = pass->vs_invocations;
        ps->fs_invocations = pass->fs_invocations;
        ps->primitives     = pass->primitives;

        ps->history[ps->history_idx] = (float)ms;
        ps->history_idx              = (ps->history_idx + 1) % GPU_PROF_HISTORY_SIZE;
    }

    g_gpu_profiler_ui.total_gpu_time_ms = total_ms;
    if (g_gpu_profiler_ui.avg_total_gpu_time_ms == 0.0) {
        g_gpu_profiler_ui.avg_total_gpu_time_ms = total_ms;
    } else {
        g_gpu_profiler_ui.avg_total_gpu_time_ms = g_gpu_profiler_ui.avg_total_gpu_time_ms * 0.95 + total_ms * 0.05;
    }

    g_gpu_profiler_ui.total_history[g_gpu_profiler_ui.total_history_idx] = (float)total_ms;
    g_gpu_profiler_ui.total_history_idx = (g_gpu_profiler_ui.total_history_idx + 1) % GPU_PROF_HISTORY_SIZE;
    g_gpu_profiler_ui.frame_counter++;
}

static void profiler_format_count(char *buffer, size_t buffer_size, uint64_t value) {
    if (value >= 1000000000ull) {
        snprintf(buffer, buffer_size, "%.2f B", (double)value / 1000000000.0);
    } else if (value >= 1000000ull) {
        snprintf(buffer, buffer_size, "%.2f M", (double)value / 1000000.0);
    } else if (value >= 1000ull) {
        snprintf(buffer, buffer_size, "%.1f k", (double)value / 1000.0);
    } else {
        snprintf(buffer, buffer_size, "%llu", (unsigned long long)value);
    }
}

static MU_INLINE void frame_start(Renderer *r) {
    TracyCZoneNC(ctx, "frame_start", 0x00FF00, 1);
    uint64_t frame_now = mu_time_now();
    r->cpu_frame_ns    = (double)(frame_now - r->cpu_prev_frame);
    r->cpu_prev_frame  = frame_now;
    r->current_frame   = (r->current_frame + 1) % MAX_FRAMES_IN_FLIGHT;

    int fb_w, fb_h;
    glfwGetFramebufferSize(r->window, &fb_w, &fb_h);

    if (fb_w == 0 || fb_h == 0) {
        uint64_t wait_start = mu_time_now();
        glfwWaitEvents();
        r->cpu_wait_accum_ns += (double)(mu_time_now() - wait_start);
        return;
    }
    r->swapchain.needs_recreate |= fb_w != (int)r->swapchain.extent.width || fb_h != (int)r->swapchain.extent.height;

    if (r->swapchain.needs_recreate) {
        vkDeviceWaitIdle(r->devc.device);

        vk_swapchain_recreate(r->devc.device, r->devc.physical_device, &r->swapchain, fb_w, fb_h,
                              r->devc.graphics_queue, r->one_time_gfx_pool, r);

        forEach(i, r->swapchain.image_count) {
            rt_resize(r, &r->depth[i], fb_w, fb_h);
            rt_resize(r, &r->hdr_color[i], fb_w, fb_h);

            rt_resize(r, &r->ldr_color[i], fb_w, fb_h);
           
            rt_resize(r, &r->smaa_final[i], fb_w, fb_h);
            rt_resize(r, &r->smaa_edges[i], fb_w, fb_h);
            rt_resize(r, &r->smaa_weights[i], fb_w, fb_h);
        }

        r->swapchain.needs_recreate = false;
    }

    FrameContext *f = &r->frames[r->current_frame];

    uint64_t wait_start = mu_time_now();
    VK_CHECK(vkWaitForFences(r->devc.device, 1, &f->in_flight_fence, VK_TRUE, UINT64_MAX));
    r->cpu_wait_ns       = (double)(mu_time_now() - wait_start);
    r->cpu_wait_accum_ns = r->cpu_wait_accum_ns * 0.95 + r->cpu_wait_ns * 0.05;
    r->cpu_active_ns     = MAX(r->cpu_frame_ns - r->cpu_wait_ns, 0.0);

    VK_CHECK(vkResetFences(r->devc.device, 1, &f->in_flight_fence));

    buffer_pool_linear_reset(&r->cpu_pool);
    buffer_pool_ring_free_to(&r->staging_pool, f->staging_tail);

    GpuProfiler *frame_prof = &r->gpuprofiler[r->current_frame];

    gpu_profiler_collect(frame_prof, r->devc.device);
    gpu_profiler_ui_update(frame_prof);

    vkResetCommandPool(r->devc.device, f->cmdbufpool, 0);

    vk_swapchain_acquire(r->devc.device, &r->swapchain, r->frames[r->current_frame].image_available_semaphore,
                         VK_NULL_HANDLE, UINT64_MAX);
    TracyCZoneEnd(ctx);
}

static void update_global_data(Renderer *r) {
    GlobalData data = {0};
    glm_mat4_identity(data.view);
    glm_mat4_identity(data.projection);
    glm_mat4_identity(data.viewproj);
    glm_mat4_identity(data.inv_view);
    glm_mat4_identity(data.inv_projection);
    glm_mat4_identity(data.inv_viewproj);

    data.time = (float)glfwGetTime();
    data.delta_time = (float)((double)r->cpu_frame_ns / 1000000000.0);
    data.frame_count = r->frame_count++;
    data.screen_params[0] = (float)r->swapchain.extent.width;
    data.screen_params[1] = (float)r->swapchain.extent.height;
    data.screen_params[2] = 1.0f / data.screen_params[0];
    data.screen_params[3] = 1.0f / data.screen_params[1];

    Buffer *global_buffer = &r->global_ubo[r->current_frame];
    memcpy(global_buffer->mapping, &data, sizeof(data));
    vmaFlushAllocation(r->devc.vmaallocator, global_buffer->allocation, 0, sizeof(data));

    VkDescriptorBufferInfo info = {
        .buffer = global_buffer->buffer,
        .offset = 0,
        .range = sizeof(data),
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = r->bindless_system.set,
        .dstBinding = GLOBAL_DATA_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &info,
    };
    vkUpdateDescriptorSets(r->devc.device, 1, &write, 0, NULL);
}

static MU_INLINE void submit_frame(Renderer *r) {
    TracyCZoneNC(ctx, "submit_frame", 0xFF0000, 1);
    FrameContext *f   = &r->frames[r->current_frame];
    uint32_t      img = r->swapchain.current_image;

    if (r->staging_pool.type == BUFFER_POOL_RING)
        f->staging_tail = r->staging_pool.ring.head;

    VkCommandBufferSubmitInfo cmd = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = f->cmdbuf};

    VkSemaphoreSubmitInfo wait = {.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = f->image_available_semaphore,
                                  .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};

    VkSemaphoreSubmitInfo signal = {.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                    .semaphore = r->swapchain.render_finished[img],
                                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};

    VkSubmitInfo2 submit = {.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                            .waitSemaphoreInfoCount   = 1,
                            .pWaitSemaphoreInfos      = &wait,
                            .commandBufferInfoCount   = 1,
                            .pCommandBufferInfos      = &cmd,
                            .signalSemaphoreInfoCount = 1,
                            .pSignalSemaphoreInfos    = &signal};

    VK_CHECK(vkQueueSubmit2(r->devc.graphics_queue, 1, &submit, f->in_flight_fence));

    vk_swapchain_present(r->devc.present_queue, &r->swapchain,
                         &r->swapchain.render_finished[r->swapchain.current_image], 1);

    TracyCZoneEnd(ctx);
}

PUSH_CONSTANT(PostPush, uint32_t src_texture_id; uint32_t output_image_id; uint32_t sampler_id; uint32_t width;
              uint32_t height; uint frame; float exposure;

);
PUSH_CONSTANT(EdgePush, uint32_t texture_id; uint32_t sampler_id;);

PUSH_CONSTANT(BlendPush, uint32_t color_tex; uint32_t weight_tex; uint32_t sampler_id; uint32_t pad;);

PUSH_CONSTANT(WeightPush, uint32_t edge_tex; uint32_t area_tex; uint32_t search_tex; uint32_t sampler_id;);

static void render_gpu_profiler_ui(Renderer *r) {

    ImGuiIO *io = igGetIO_Nil();
    if (!g_gpu_profiler_ui.open) {
        ImGuiIO *io = igGetIO_Nil();
        if (igBegin("GPU Profiler (Hidden)", NULL,
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
            igTextColored((ImVec4_c){0.3f, 0.8f, 1.0f, 1.0f}, "Profiler");
            igSameLine(0.0f, 10.0f);
            if (igButton("Show", (ImVec2_c){40, 20})) {
                g_gpu_profiler_ui.open = true;
            }
        }
        igEnd();
        return;
    }

    if (!g_gpu_profiler_ui.open)
        return;
    igSetNextWindowSize((ImVec2_c){720.0f, 520.0f}, ImGuiCond_FirstUseEver);
    if (!igBegin("GPU Profiler", &g_gpu_profiler_ui.open, ImGuiWindowFlags_None)) {
        igEnd();
        return;
    }

    igTextColored((ImVec4_c){0.3f, 0.9f, 0.5f, 1.0f}, "GPU Timing Profiler");
    igSameLine(0.0f, 20.0f);
    igCheckbox("Pause", &g_gpu_profiler_ui.paused);
    if (igButton("CLOSE", (ImVec2_c){40, 20})) {
        g_gpu_profiler_ui.open = false;
    }

    igSameLine(0.0f, 15.0f);
    igCheckbox("Pipeline Stats", &g_gpu_profiler_ui.show_pipeline_stats);
    igSameLine(0.0f, 15.0f);
    if (igButton("Reset Min/Max", (ImVec2_c){0, 0})) {
        for (uint32_t i = 0; i < MAX_RECORDED_PASSES; i++) {
            g_gpu_profiler_ui.pass_stats[i].min_ms = g_gpu_profiler_ui.pass_stats[i].time_ms;
            g_gpu_profiler_ui.pass_stats[i].max_ms = g_gpu_profiler_ui.pass_stats[i].time_ms;
        }
    }

    igSeparator();

    double frame_ms         = ns_to_ms(r->cpu_frame_ns);
    double active_ms        = ns_to_ms(r->cpu_active_ns);
    double wait_ms          = ns_to_ms(r->cpu_wait_ns);
    double wait_avg_ms      = ns_to_ms(r->cpu_wait_accum_ns);
    double gpu_ms           = g_gpu_profiler_ui.total_gpu_time_ms;
    double frame_budget_pct = frame_ms > 0.0 ? gpu_ms / frame_ms * 100.0 : 0.0;
    if (frame_budget_pct > 100.0)
        frame_budget_pct = 100.0;

    igTextColored((ImVec4_c){0.3f, 0.8f, 1.0f, 1.0f}, "Frame Metrics");
    if (igBeginTable("FrameMetrics", 3, ImGuiTableFlags_SizingStretchProp, (ImVec2_c){0.0f, 0.0f}, 0.0f)) {
        igTableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        igTableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 105.0f, 0);
        igTableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        igTableHeadersRow();

        igTableNextRow(0, 0.0f);
        igTableSetColumnIndex(0);
        igText("Frame time");
        igTableSetColumnIndex(1);
        igText("%.3f ms", frame_ms);
        igTableSetColumnIndex(2);
        igText("%.1f FPS", io ? io->Framerate : 0.0f);

        igTableNextRow(0, 0.0f);
        igTableSetColumnIndex(0);
        igText("CPU active time");
        igTableSetColumnIndex(1);
        igText("%.3f ms", active_ms);
        igTableSetColumnIndex(2);
        igText("%.1f%% of frame", frame_ms > 0.0 ? active_ms / frame_ms * 100.0 : 0.0);

        igTableNextRow(0, 0.0f);
        igTableSetColumnIndex(0);
        igText("CPU waiting time");
        igTableSetColumnIndex(1);
        igText("%.3f ms", wait_ms);
        igTableSetColumnIndex(2);
        igText("EMA %.3f ms", wait_avg_ms);

        igTableNextRow(0, 0.0f);
        igTableSetColumnIndex(0);
        igText("GPU frame time");
        igTableSetColumnIndex(1);
        igText("%.3f ms", gpu_ms);
        igTableSetColumnIndex(2);
        igText("%.1f%% of CPU frame", frame_budget_pct);

        igTableNextRow(0, 0.0f);
        igTableSetColumnIndex(0);
        igText("ImGui workload");
        igTableSetColumnIndex(1);
        igText("%d windows", io ? io->MetricsActiveWindows : 0);
        igTableSetColumnIndex(2);
        igText("%d vertices | %d indices", io ? io->MetricsRenderVertices : 0, io ? io->MetricsRenderIndices : 0);
        igEndTable();
    }

    igSpacing();

    if (g_gpu_profiler_ui.show_pipeline_stats) {
        uint64_t total_vs         = 0;
        uint64_t total_fs         = 0;
        uint64_t total_primitives = 0;
        for (uint32_t i = 0; i < g_gpu_profiler_ui.pass_count; i++) {
            total_vs += g_gpu_profiler_ui.pass_stats[i].vs_invocations;
            total_fs += g_gpu_profiler_ui.pass_stats[i].fs_invocations;
            total_primitives += g_gpu_profiler_ui.pass_stats[i].primitives;
        }

        char vs_buf[32];
        char fs_buf[32];
        char primitives_buf[32];
        profiler_format_count(vs_buf, sizeof(vs_buf), total_vs);
        profiler_format_count(fs_buf, sizeof(fs_buf), total_fs);
        profiler_format_count(primitives_buf, sizeof(primitives_buf), total_primitives);
        igText("Pipeline statistics");
        igSameLine(0.0f, 15.0f);
        igText("Vertices: %s | Fragments: %s | Clipping primitives: %s", vs_buf, fs_buf, primitives_buf);
        igSpacing();
    }

    igText("Total GPU Time: ");
    igSameLine(0.0f, 0.0f);
    igTextColored((ImVec4_c){1.0f, 0.85f, 0.3f, 1.0f}, "%.3f ms", g_gpu_profiler_ui.total_gpu_time_ms);
    igSameLine(0.0f, 15.0f);
    igText("(Avg: %.3f ms)", g_gpu_profiler_ui.avg_total_gpu_time_ms);
    igSameLine(0.0f, 25.0f);
    igText("FPS: ");
    igSameLine(0.0f, 0.0f);
    igTextColored((ImVec4_c){0.4f, 0.8f, 1.0f, 1.0f}, "%.1f", io ? io->Framerate : 0.0f);

    char overlay_buf[64];
    snprintf(overlay_buf, sizeof(overlay_buf), "Total: %.3f ms", g_gpu_profiler_ui.total_gpu_time_ms);
    float max_graph_val = (float)g_gpu_profiler_ui.avg_total_gpu_time_ms * 1.5f;
    if (max_graph_val < 1.0f)
        max_graph_val = 1.0f;

    igPlotLines_FloatPtr("##GpuTotalTimeGraph", g_gpu_profiler_ui.total_history, GPU_PROF_HISTORY_SIZE,
                         (int)g_gpu_profiler_ui.total_history_idx, overlay_buf, 0.0f, max_graph_val,
                         (ImVec2_c){-1.0f, 55.0f}, sizeof(float));

    igSpacing();

    int             table_cols = g_gpu_profiler_ui.show_pipeline_stats ? 8 : 5;
    ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

    if (igBeginTable("GpuPassTable", table_cols, table_flags, (ImVec2_c){0.0f, 0.0f}, 0.0f)) {
        igTableSetupColumn("Pass Name", ImGuiTableColumnFlags_WidthStretch, 2.0f, 0);
        igTableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f, 0);
        igTableSetupColumn("Avg (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f, 0);
        igTableSetupColumn("Min / Max (ms)", ImGuiTableColumnFlags_WidthFixed, 115.0f, 0);
        igTableSetupColumn("% Total", ImGuiTableColumnFlags_WidthStretch, 2.5f, 0);
        if (g_gpu_profiler_ui.show_pipeline_stats) {
            igTableSetupColumn("Vert Shaders", ImGuiTableColumnFlags_WidthFixed, 95.0f, 0);
            igTableSetupColumn("Frag Shaders", ImGuiTableColumnFlags_WidthFixed, 95.0f, 0);
            igTableSetupColumn("Clip Primitives", ImGuiTableColumnFlags_WidthFixed, 105.0f, 0);
        }
        igTableHeadersRow();

        for (uint32_t i = 0; i < g_gpu_profiler_ui.pass_count; i++) {
            GpuPassStats *ps = &g_gpu_profiler_ui.pass_stats[i];
            igTableNextRow(0, 0.0f);

            igTableSetColumnIndex(0);
            igText("%s", ps->name);

            igTableSetColumnIndex(1);
            if (ps->time_ms < 0.1) {
                igText("%.1f us", ps->time_ms * 1000.0);
            } else {
                igText("%.3f ms", ps->time_ms);
            }

            igTableSetColumnIndex(2);
            igText("%.3f", ps->avg_ms);

            igTableSetColumnIndex(3);
            igText("%.3f / %.3f", ps->min_ms, ps->max_ms);

            igTableSetColumnIndex(4);
            float pct = (g_gpu_profiler_ui.total_gpu_time_ms > 0.0)
                            ? (float)(ps->time_ms / g_gpu_profiler_ui.total_gpu_time_ms)
                            : 0.0f;
            if (pct > 1.0f)
                pct = 1.0f;
            char pct_buf[32];
            snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", pct * 100.0f);
            igProgressBar(pct, (ImVec2_c){-1.0f, 0.0f}, pct_buf);

            if (g_gpu_profiler_ui.show_pipeline_stats) {
                igTableSetColumnIndex(5);
                if (ps->vs_invocations >= 1000000) {
                    igText("%.2f M", (double)ps->vs_invocations / 1000000.0);
                } else if (ps->vs_invocations >= 1000) {
                    igText("%.1f k", (double)ps->vs_invocations / 1000.0);
                } else {
                    igText("%llu", (unsigned long long)ps->vs_invocations);
                }

                igTableSetColumnIndex(6);
                char fs_buf[32];
                profiler_format_count(fs_buf, sizeof(fs_buf), ps->fs_invocations);
                igText("%s", fs_buf);

                igTableSetColumnIndex(7);
                char primitives_buf[32];
                profiler_format_count(primitives_buf, sizeof(primitives_buf), ps->primitives);
                igText("%s", primitives_buf);
            }
        }
        igEndTable();
    }

    igSeparator();
    igTextDisabled("Timestamp Period: %.2f ns | Query Pool Size: %d passes",
                   (double)r->info.properties.limits.timestampPeriod, MAX_GPU_PASSES);

    igEnd();
}

static void post_pass(Renderer *r, VkCommandBuffer cmd) {
    uint32_t image = r->swapchain.current_image;

    GpuProfiler *frame_prof = &r->gpuprofiler[r->current_frame];
    GPU_SCOPE(frame_prof, cmd, "Post Processing", VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
        rt_transition_all(r, cmd, &r->hdr_color[image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        rt_transition_all(r, cmd, &r->ldr_color[image], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);



        flush_barriers(r, cmd);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          r->render_pipelines.pipelines[r->EnginePipelines.postprocess]);

        PostPush push = {
            .src_texture_id  = r->hdr_color[image].bindless_index,
            .output_image_id = r->ldr_color[image].bindless_index,
            .sampler_id      = r->default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            .width           = r->swapchain.extent.width,
            .height          = r->swapchain.extent.height,
            .frame           = 0,
            .exposure        = 1.2f,
        };

        vkCmdPushConstants(cmd, r->bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);

        uint32_t gx = (push.width + 15) / 16;
        uint32_t gy = (push.height + 15) / 16;

        vkCmdDispatch(cmd, gx, gy, 1);
    }
}

typedef struct FirePush {
    uint32_t width;
    uint32_t height;
    float    time;
    float    pad;
} FirePush;

static void pass_fire(Renderer *r, VkCommandBuffer cmd) {
    GpuProfiler *frame_prof = &r->gpuprofiler[r->current_frame];
    GPU_SCOPE(frame_prof, cmd, "Fire Pass", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
        uint32_t image = r->swapchain.current_image;

        rt_transition_all(r, cmd, &r->hdr_color[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        flush_barriers(r, cmd);

        VkRenderingAttachmentInfo color = {
            .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView   = r->hdr_color[image].view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue  = {.color = {{0.02f, 0.025f, 0.03f, 1.0f}}},
        };

        VkRenderingInfo rendering = {
            .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea.extent    = r->swapchain.extent,
            .layerCount           = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &color,
        };

        FirePush push = {
            .width  = r->swapchain.extent.width,
            .height = r->swapchain.extent.height,
            .time   = (float)glfwGetTime(),
            .pad    = 0.0f,
        };

        vkCmdBeginRendering(cmd, &rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->render_pipelines.pipelines[r->EnginePipelines.fire]);
        vk_cmd_set_viewport_scissor(cmd, r->swapchain.extent);
        vkCmdPushConstants(cmd, r->bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }
}

static void pass_smaa(Renderer *r, VkCommandBuffer cmd) {
    uint32_t     image      = r->swapchain.current_image;
    GpuProfiler *frame_prof = &r->gpuprofiler[r->current_frame];

    {
        /* 1. Edge detection */
        GPU_SCOPE(frame_prof, cmd, "SMAA Edge", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
            rt_transition_all(r, cmd, &r->smaa_edges[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            rt_transition_all(r, cmd, &r->ldr_color[image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            flush_barriers(r, cmd);

            VkRenderingAttachmentInfo edge_color = {
                .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView   = r->smaa_edges[image].view,
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue  = {.color = {{0, 0, 0, 0}}},
            };

            VkRenderingInfo edge_rendering = {
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea.extent    = r->swapchain.extent,
                .layerCount           = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments    = &edge_color,
            };

            vkCmdBeginRendering(cmd, &edge_rendering);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              r->render_pipelines.pipelines[r->smaa_pipelines.smaa_edge]);
            vk_cmd_set_viewport_scissor(cmd, r->swapchain.extent);

            EdgePush edge_push = {
                .texture_id = r->ldr_color[image].bindless_index,
                .sampler_id = r->default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            };

            vkCmdPushConstants(cmd, r->bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(edge_push),
                               &edge_push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
        }
    }
    {
        /* 2. Weight calculation */
        GPU_SCOPE(frame_prof, cmd, "SMAA Weight", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
            rt_transition_all(r, cmd, &r->smaa_weights[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            rt_transition_all(r, cmd, &r->smaa_edges[image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            flush_barriers(r, cmd);

            VkRenderingAttachmentInfo weight_color = {
                .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView   = r->smaa_weights[image].view,
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue  = {.color = {{0, 0, 0, 0}}},
            };

            VkRenderingInfo weight_rendering = {
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea.extent    = r->swapchain.extent,
                .layerCount           = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments    = &weight_color,
            };

            vkCmdBeginRendering(cmd, &weight_rendering);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              r->render_pipelines.pipelines[r->smaa_pipelines.smaa_weight]);
            vk_cmd_set_viewport_scissor(cmd, r->swapchain.extent);

            WeightPush weight_push = {
                .edge_tex   = r->smaa_edges[image].bindless_index,
                .area_tex   = r->smaa_area_tex,
                .search_tex = r->smaa_search_tex,
                .sampler_id = r->default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            };

            vkCmdPushConstants(cmd, r->bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(weight_push),
                               &weight_push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
        }
    }
    {
        /* 3. Blend LDR + SMAA into FINAL */
        GPU_SCOPE(frame_prof, cmd, "SMAA Blend", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
            rt_transition_all(r, cmd, &r->smaa_final[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            rt_transition_all(r, cmd, &r->ldr_color[image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            rt_transition_all(r, cmd, &r->smaa_weights[image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            flush_barriers(r, cmd);

            VkRenderingAttachmentInfo final_color = {
                .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView   = r->smaa_final[image].view,
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
            };

            VkRenderingInfo final_rendering = {
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea.extent    = r->swapchain.extent,
                .layerCount           = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments    = &final_color,
            };

            vkCmdBeginRendering(cmd, &final_rendering);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              r->render_pipelines.pipelines[r->smaa_pipelines.smaa_blend]);
            vk_cmd_set_viewport_scissor(cmd, r->swapchain.extent);

            BlendPush blend_push = {
                .color_tex  = r->ldr_color[image].bindless_index,
                .weight_tex = r->smaa_weights[image].bindless_index,
                .sampler_id = r->default_samplers.samplers[SAMPLER_LINEAR_CLAMP],
            };

            vkCmdPushConstants(cmd, r->bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(blend_push),
                               &blend_push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
        }
    }
}

static void pass_ldr_to_swapchain(Renderer *r, VkCommandBuffer cmd) {
    GpuProfiler *frame_prof = &r->gpuprofiler[r->current_frame];
    GPU_SCOPE(frame_prof, cmd, "Blit Swapchain", VK_PIPELINE_STAGE_2_TRANSFER_BIT) {
        uint32_t image = r->swapchain.current_image;

        rt_transition_all(r, cmd, &r->smaa_final[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

        image_transition_swapchain(r, cmd, &r->swapchain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        flush_barriers(r, cmd);

        VkImageBlit blit = {
            .srcSubresource =
                {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = 0,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },

            .srcOffsets =
                {
                    {0, 0, 0},
                    {(int32_t)r->swapchain.extent.width, (int32_t)r->swapchain.extent.height, 1},
                },

            .dstSubresource =
                {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = 0,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },

            .dstOffsets =
                {
                    {0, 0, 0},
                    {(int32_t)r->swapchain.extent.width, (int32_t)r->swapchain.extent.height, 1},
                },
        };

        vkCmdBlitImage(cmd, r->smaa_final[image].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       r->swapchain.images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
}

static void pass_imgui(Renderer *r, VkCommandBuffer cmd) {
    GpuProfiler *frame_prof = &r->gpuprofiler[r->current_frame];
    GPU_SCOPE(frame_prof, cmd, "ImGui Render", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
        uint32_t image = r->swapchain.current_image;

        image_transition_swapchain(r, cmd, &r->swapchain, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        flush_barriers(r, cmd);

        VkRenderingAttachmentInfo color = {
            .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView   = r->swapchain.image_views[image],
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        };

        VkRenderingInfo rendering = {
            .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea.extent    = r->swapchain.extent,
            .layerCount           = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &color,
        };

        vkCmdBeginRendering(cmd, &rendering);

        ImDrawData *draw_data = igGetDrawData();

        ImGui_ImplVulkan_RenderDrawData(draw_data, cmd, VK_NULL_HANDLE);

        vkCmdEndRendering(cmd);
    }
}

FORCE_INLINE void imgui_shutdown(void) {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    igDestroyContext(NULL);
}

FORCE_INLINE void imgui_begin_frame(void) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    igNewFrame();
}

int main() {

    graphics_init();
    dmon_init();

    g_source_watch_id = dmon_watch("shaders", watch_callback, DMON_WATCHFLAGS_RECURSIVE, g_renderer);

    dmon_watch("compiledshaders", watch_callback, DMON_WATCHFLAGS_RECURSIVE, g_renderer);
    // Specify your shader directory here
    while (!glfwWindowShouldClose(g_renderer->window)) {

        TracyCFrameMark;
        glfwPollEvents();

        pipeline_rebuild(g_renderer);
        frame_start(g_renderer);
        update_global_data(g_renderer);

        imgui_begin_frame();
        Renderer *renderer = g_renderer;

        Renderer       *r          = g_renderer;
        VkCommandBuffer cmd        = renderer->frames[renderer->current_frame].cmdbuf;
        GpuProfiler    *frame_prof = &renderer->gpuprofiler[renderer->current_frame];

        vk_cmd_begin(cmd, false);
        gpu_profiler_begin_frame(frame_prof, cmd);

        {
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->bindless_system.pipeline_layout,
                                        0, 1, &renderer->bindless_system.set, 0, NULL);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer->bindless_system.pipeline_layout,
                                        0, 1, &renderer->bindless_system.set, 0, NULL);

                rt_transition_all(
                    renderer, cmd, &renderer->depth[renderer->swapchain.current_image],
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

                flush_barriers(renderer, cmd);
            }
        }

        pass_fire(r, cmd);
        post_pass(r, cmd);
        pass_smaa(r, cmd);
        pass_ldr_to_swapchain(r, cmd);
        render_gpu_profiler_ui(r);
        igRender();
        pass_imgui(r, cmd);
        image_transition_swapchain(r, cmd, &r->swapchain, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);

        flush_barriers(r, cmd);

        vk_cmd_end(cmd);

        submit_frame(renderer);
    }

    dmon_deinit();
    pipeline_cache_save(g_renderer->devc.device, g_renderer->devc.physical_device, g_renderer->devc.pipeline_cache,
                        "pipeline_cache.bin");
    return 0;
}
