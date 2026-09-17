#include "vk.h"
#include "external/mu/mu/mu_perf.h"

static bool is_instance_extension_supported(const char *extension_name) {
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

static bool is_instance_layer_supported(const char *layer_name) {
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

    return VK_FALSE;
}

static bool device_supports_extensions(VkPhysicalDevice gpu, const char **req, uint32_t req_count) {
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

static VkPhysicalDevice pick_physical_device(VkInstance instance, VkSurfaceKHR surface, VkBackendDesc *rcd) {
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

static void query_device_features(VkPhysicalDevice gpu, VkFeatureChain *out) {
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

typedef struct VkBackendCaps {
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
} VkBackendCaps;

static VkBackendCaps default_caps(void) {
    return (VkBackendCaps){
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
static void apply_caps(VkFeatureChain *f, const VkBackendCaps *caps) {
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
static void find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface, queue_families *out);
// Call AFTER vkCreateDevice.
// Uses the family indices already stored in queue_families.
static void init_device_queues(VkDevice device, queue_families *q);

static void find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface, queue_families *out) {
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

static void init_device_queues(VkDevice device, queue_families *q) {
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

static VkPipelineCache pipeline_cache_load_or_create(VkDevice device, VkPhysicalDevice phys, const char *path) {
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

static VkSurfaceCapabilities2KHR query_surface_capabilities(VkPhysicalDevice gpu, VkSurfaceKHR surface) {
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

static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR *caps, uint32_t desired_w, uint32_t desired_h) {
    if (caps->currentExtent.width != 0xFFFFFFFF)
        return caps->currentExtent;
    VkExtent2D extent = {.width = desired_w, .height = desired_h};
    extent.width      = CLAMP(extent.width, caps->minImageExtent.width, caps->maxImageExtent.width);
    extent.height     = CLAMP(extent.height, caps->minImageExtent.height, caps->maxImageExtent.height);
    return extent;
}

static VkSurfaceFormatKHR select_surface_format(VkPhysicalDevice gpu, VkSurfaceKHR surface, VkFormat preferred,
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
                         VkBackend *r) {
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
                           VkQueue graphics_queue, VkCommandPool one_time_pool, VkBackend *r){
    if (new_w == 0 || new_h == 0)
        return;

    // Device-wide wait is required here, not just a timeline wait: the old
    // swapchain's render_finished semaphores are consumed by the PRESENT queue,
    // which the submission timeline does not track. Only a device idle covers
    // every submitted batch that references them. rt_resize below also needs
    // quiescence (it recreates targets and rewrites bindless descriptors).
    vkDeviceWaitIdle(device);

    // Release the old swapchain's views AND its bindless texture slots by
    // reusing vk_swapchain_destroy. It destroys images views, semaphores,
    // frees id-pool entries and memsets its argument, so operate on a copy and
    // keep the old VkSwapchainKHR handle alive for oldSwapchain reuse.
    VkSwapchainKHR   old       = sc->swapchain;
    FlowSwapchain    old_state = *sc;
    // Keep the old VkSwapchainKHR alive for oldSwapchain reuse below; only
    // destroy its views/semaphores/bindless slots here.
    old_state.swapchain = VK_NULL_HANDLE;
    vk_swapchain_destroy(device, &old_state, r ? &r->texture_system.id_pool : NULL);

    FlowSwapchainCreateInfo info = {0};
    info.surface                 = sc->surface;
    info.width                   = new_w;
    info.height                  = new_h;
    info.min_image_count         = MAX(3u, sc->image_count);
    info.preferred_format        = sc->format;
    info.preferred_color_space   = sc->color_space;
    info.preferred_present_mode  = sc->present_mode;
    info.extra_usage             = sc->image_usage & ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.old_swapchain           = old;

    vk_create_swapchain(device, gpu, sc, &info, graphics_queue, one_time_pool, r);

    if (old)
        vkDestroySwapchainKHR(device, old, NULL);
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
TextureID create_texture(VkBackend *r, const TextureCreateDesc *desc) {
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

bool create_buffer(VkBackend *r, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memory_usage, Buffer *out);

void destroy_buffer(VkBackend *r, Buffer *b);

bool buffer_pool_init(VkBackend *r,

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



void buffer_pool_destroy(VkBackend *r, BufferPool *pool) {
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

// ---- Byte-span views ----
//
// Non-owning pointer+size passed by value; the pair ships in registers. The
// macro form is a compound literal, so the referenced value only needs to live
// through the call. (Promote to external/mu when a second span type is needed.)


// Staging-slot alignment is backend policy (256 covers noncoherent atom size and
// keeps ring slots cache-line friendly); callers never pass it.
#define STAGING_ALIGNMENT_DEFAULT 256

bool renderer_upload_buffer_to_slice(VkBackend *r, VkCommandBuffer cmd, BufferSlice dst_slice, ByteSpan data) {
    if (!r || !cmd || !data.data || !dst_slice.buffer || data.size == 0)
        return false;

    if (data.size > dst_slice.size)
        return false;

    BufferSlice staging_slice = buffer_pool_alloc(&r->staging_pool, data.size, STAGING_ALIGNMENT_DEFAULT);
    if (!staging_slice.buffer || !staging_slice.mapped)
        return false;

    memcpy(staging_slice.mapped, data.data, (size_t)data.size);

    VkBufferCopy copy = {
        .srcOffset = staging_slice.offset,
        .dstOffset = dst_slice.offset,
        .size      = data.size,
    };
    vkCmdCopyBuffer(cmd, staging_slice.buffer, dst_slice.buffer, 1, &copy);

    return true;
}

// One-call upload: allocates from the gpu pool at dst_alignment, stages, records
// the copy. dst_alignment is the only knob left to the caller (vertex/SSBO data
// often wants specific alignment for device-address fetch).
BufferSlice renderer_upload_buffer(VkBackend *r, VkCommandBuffer cmd, ByteSpan data, VkDeviceSize dst_alignment) {
    BufferSlice dst_slice = {0};
    if (!r || !cmd || !data.data || data.size == 0)
        return dst_slice;

    dst_slice = buffer_pool_alloc(&r->gpu_pool, data.size, dst_alignment);
    if (!dst_slice.buffer)
        return dst_slice;

    if (!renderer_upload_buffer_to_slice(r, cmd, dst_slice, data)) {
        buffer_pool_free(dst_slice);
        memset(&dst_slice, 0, sizeof(dst_slice));
    }

    return dst_slice;
}
bool create_buffer(VkBackend *r, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memory_usage, Buffer *out) {
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
void destroy_buffer(VkBackend *r, Buffer *buffer) {
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

static void rt_update_bindless_descriptors(VkBackend *r, const RenderTarget *rt);
static void rt_destroy_internal(VkBackend *r, RenderTarget *rt, bool release_id);

static bool rt_create_internal(VkBackend *r, RenderTarget *rt, const RenderTargetSpec *spec, uint32_t bindless_index) {

    if (!r || !rt || !spec || spec->width == 0 || spec->height == 0 || spec->usage == 0)
        return false;

    memset(rt, 0, sizeof(*rt));

    rt->format = spec->format;
    rt->width  = spec->width;
    rt->height = spec->height;
    rt->usage  = spec->usage;
    rt->aspect = spec->aspect ? spec->aspect : get_image_aspect(spec->format);
    rt->layers = spec->layers ? spec->layers : 1;

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
    rt->bindless_index = UINT32_MAX;
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

bool rt_create(VkBackend *r, RenderTarget *rt, const RenderTargetSpec *spec) {
    return rt_create_internal(r, rt, spec, UINT32_MAX);
}

static void rt_update_bindless_descriptors(VkBackend *r, const RenderTarget *rt) {
    if (!r || !rt || rt->bindless_index == UINT32_MAX)
        return;

    VkWriteDescriptorSet  writes[2];
    VkDescriptorImageInfo images[2];
    uint32_t              write_count = 0;

    if (rt->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        images[write_count] = (VkDescriptorImageInfo){
            .imageView   = rt->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        writes[write_count] = (VkWriteDescriptorSet){
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = r->bindless_system.set,
            .dstBinding      = BINDLESS_TEXTURE_BINDING,
            .dstArrayElement = rt->bindless_index,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo      = &images[write_count],
        };
        write_count++;
    }

    if (rt->usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        images[write_count] = (VkDescriptorImageInfo){
            .imageView   = rt->view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        writes[write_count] = (VkWriteDescriptorSet){
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = r->bindless_system.set,
            .dstBinding      = BINDLESS_STORAGE_IMAGE_BINDING,
            .dstArrayElement = rt->bindless_index,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo      = &images[write_count],
        };
        write_count++;
    }

    if (write_count != 0)
        vkUpdateDescriptorSets(r->devc.device, write_count, writes, 0, NULL);
}

static void rt_destroy_internal(VkBackend *r, RenderTarget *rt, bool release_id) {
    if (!r || !rt || !rt->image)
        return;
    if (release_id && rt->bindless_index != UINT32_MAX)
        mu_id_pool_destroy_id(&r->texture_system.id_pool, rt->bindless_index);

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

void rt_destroy(VkBackend *r, RenderTarget *rt) { rt_destroy_internal(r, rt, true); }

bool rt_resize(VkBackend *r, RenderTarget *rt, uint32_t width, uint32_t height)

{
    if (!r || !rt)
        return false;

    if (width == rt->width && height == rt->height)
        return true;

    uint32_t         bindless_index = rt->bindless_index;
    RenderTargetSpec spec           = {.width      = width,
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
// Zero-value defaults: linear min/mag/mip filtering, repeat addressing, lod clamp
// [0, VK_LOD_CLAMP_NONE]. Name only deltas. Backend-only Vk fields (border color,
// unnormalized coordinates, etc.) still go through the raw path.


static inline VkSamplerCreateInfo sampler_info_from_desc(const SamplerDesc *d) {
    VkSamplerCreateInfo ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = d->mag_filter ? d->mag_filter : VK_FILTER_LINEAR,
        .minFilter    = d->min_filter ? d->min_filter : VK_FILTER_LINEAR,
        .mipmapMode   = d->nearest_mips ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = d->clamp_mode_override ? d->clamp_mode_override
                                               : (d->address_u ? d->address_u : VK_SAMPLER_ADDRESS_MODE_REPEAT),
        .addressModeV = d->clamp_mode_override ? d->clamp_mode_override
                                               : (d->address_v ? d->address_v : VK_SAMPLER_ADDRESS_MODE_REPEAT),
        .addressModeW = d->clamp_mode_override ? d->clamp_mode_override
                                               : (d->address_w ? d->address_w : VK_SAMPLER_ADDRESS_MODE_REPEAT),
        .anisotropyEnable = d->anisotropic ? VK_TRUE : VK_FALSE,
        .maxAnisotropy    = d->anisotropic ? 16.0f : 1.0f,
        .compareEnable    = d->compare_enabled ? VK_TRUE : VK_FALSE,
        .compareOp        = d->compare_enabled ? (d->compare ? d->compare : VK_COMPARE_OP_LESS_OR_EQUAL)
                                               : VK_COMPARE_OP_NEVER,
        .minLod           = 0.0f,
        .maxLod           = VK_LOD_CLAMP_NONE,
        .borderColor      = d->border_color ? d->border_color : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    return ci;
}

bool sampler_create(VkBackend *r, const SamplerDesc *desc, uint32_t *out_sampler_id) {
    if (!r || !desc || !out_sampler_id)
        return false;

    VkSampler sampler = VK_NULL_HANDLE;

    VkSamplerCreateInfo ci = sampler_info_from_desc(desc);
    VkResult       res = vkCreateSampler(r->devc.device, &ci, NULL, &sampler);
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



// The zero-value config is the default state: unblended, no cull, counter-
// clockwise-wound triangles, triangle list, depth-tested. Call sites name only what differs.




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



static inline void pipeline_fill_blend_defaults(GraphicsPipelineConfig *cfg) {
    forEach(i, cfg->color_attachment_count) {
        if (cfg->blends[i].write_mask == 0) // unset: designated init leaves the rest zeroed
            cfg->blends[i] = blend_disabled();
    }
}

VkPipeline create_graphics_pipeline(VkBackend *renderer, const GraphicsPipelineConfig *cfg) {

    assert(cfg->vert_path && cfg->frag_path && "pipeline needs both shader paths");

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

    vkDestroyShaderModule(renderer->devc.device, vs, NULL);
    vkDestroyShaderModule(renderer->devc.device, fs, NULL);

    free(vs_code);
    free(fs_code);

    return pipeline;
}

VkPipeline create_compute_pipeline(VkBackend *renderer, const char *compute_path) {

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
        .y        = (float)extent.height,
        .width    = (float)extent.width,
        .height   = -(float)extent.height,
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

static void spv_to_slang(const char *spv, char *out, size_t out_size) {
    const char *name = strrchr(spv, '/');
    name             = name ? name + 1 : spv;

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", name);

    char *stage = strstr(tmp, ".vert");
    if (!stage)
        stage = strstr(tmp, ".frag");
    if (!stage)
        stage = strstr(tmp, ".comp");

    if (stage)
        *stage = '\0';

    snprintf(out, out_size, "shaders/%s.slang", tmp);
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool shader_change_matches_spv(const char *changed, const char *spv) {
    if (!changed || !spv)
        return false;

    char slang[256];
    spv_to_slang(spv, slang, sizeof(slang));

    const char *changed_name = path_basename(changed);
    const char *spv_name     = path_basename(spv);
    if (strcmp(changed_name, spv_name) == 0)
        return true;

    if (strstr(changed, slang))
        return true;

    const char *slang_name = path_basename(slang);

    return strcmp(changed_name, slang_name) == 0;
}
PipelineID pipeline_create_compute(VkBackend *r, const char *path) {

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
    return id + 1; // public IDs are 1-based; 0 means "no pipeline"
}
PipelineID pipeline_create_graphics(VkBackend *r, GraphicsPipelineConfig *cfg) {
    uint32_t id;
    mu_id_pool_create_id(&r->render_pipelines.pipeline_id_pool, &id);

    PipelineEntry *e = &r->render_pipelines.entries[id];

    e->type     = PIPELINE_TYPE_GRAPHICS;
    pipeline_fill_blend_defaults(cfg);
    e->graphics = *cfg;
    e->dirty    = false;

    r->render_pipelines.pipelines[id] = create_graphics_pipeline(r, cfg);

    r->render_pipelines.count++;

    return id + 1; // public IDs are 1-based; 0 means "no pipeline"
}
static void deferred_destroy_pipeline(VkBackend *r, void *user) {
    vkDestroyPipeline(r->devc.device, (VkPipeline)(uintptr_t)user, NULL);
}

void pipeline_rebuild(VkBackend *r) {
    bool any_dirty = false;

    for (int i = 0; i < r->render_pipelines.count; i++)
        if (r->render_pipelines.entries[i].dirty)
            any_dirty = true;

    if (!any_dirty)
        return;

    // In-flight frames keep using the old pipelines; retire them on the timeline
    // instead of stalling the device on every shader hot reload.
    uint64_t retire = r->timeline_last_submitted;

    for (int i = 0; i < r->render_pipelines.count; i++) {
        PipelineEntry *e = &r->render_pipelines.entries[i];

        if (!e->dirty)
            continue;

        e->dirty = false;

        delete_queue_defer(r, retire, deferred_destroy_pipeline, (void *)(uintptr_t)r->render_pipelines.pipelines[i]);

        if (e->type == PIPELINE_TYPE_GRAPHICS)
            r->render_pipelines.pipelines[i] = create_graphics_pipeline(r, &e->graphics);
        else
            r->render_pipelines.pipelines[i] = create_compute_pipeline(r, e->compute.path);

        printf("Pipeline %d hot reloaded\n", i);
    }
}

void pipeline_mark_dirty(VkBackend *r, const char *changed) {
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
static MU_INLINE void barrier_batch_push(VkBackend *r, const VkImageMemoryBarrier2 *barrier) {
    if (r->barrierbatch.image_count < (uint32_t)ARRAY_COUNT(r->barrierbatch.image_barriers)) {
        r->barrierbatch.image_barriers[r->barrierbatch.image_count++] = *barrier;
    } else {
        // Dropped: state tracking is still updated by the caller, so if this
        // ever fires, images may reach a draw in the wrong layout. Call
        // flush_barriers() more often in the offending pass.
        r->barrierbatch.overflow_count++;
    }
}

void image_transition_swapchain(VkBackend *r, VkCommandBuffer cmd, FlowSwapchain *sc, VkImageLayout new_layout,
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

    barrier_batch_push(r, &barrier);
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

void cmd_transition_all_mips(VkBackend *r, VkCommandBuffer cmd, VkImage image, ImageState *state,
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

    barrier_batch_push(r, &barrier);
    state->stage                                                  = newStage;
    state->access                                                 = newAccess;
    state->layout                                                 = newLayout;
    state->queue_family                                           = newQueueFamily;
    state->validity                                               = IMAGE_STATE_VALID;
    state->dirty_mips                                             = 0;
}

void cmd_transition_mip(VkBackend *r, VkCommandBuffer cmd, VkImage image, ImageState *state, VkImageAspectFlags aspect,
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

    barrier_batch_push(r, &barrier);

    state->stage        = newStage;
    state->access       = newAccess;
    state->layout       = newLayout;
    state->queue_family = newQueueFamily;
    state->validity     = IMAGE_STATE_VALID;

    state->dirty_mips &= ~bit;
}
void flush_barriers(VkBackend *r, VkCommandBuffer cmd) {

    if (r->barrierbatch.image_count == 0)
        return;

    VkDependencyInfo dep = {.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .imageMemoryBarrierCount = r->barrierbatch.image_count,
                            .pImageMemoryBarriers    = r->barrierbatch.image_barriers};

    vkCmdPipelineBarrier2(cmd, &dep);

    if (r->barrierbatch.overflow_count != 0) {
        log_error("[barriers] batch overflow: %u image barrier(s) dropped this flush "
                  "(batch capacity: %u)",
                  r->barrierbatch.overflow_count, (uint32_t)ARRAY_COUNT(r->barrierbatch.image_barriers));
        r->barrierbatch.overflow_count = 0;
    }

    r->barrierbatch.image_count = 0;
}

void rt_transition_mip(VkBackend *r, VkCommandBuffer cmd, RenderTarget *rt, uint32_t mip,
                                 VkImageLayout new_layout, VkPipelineStageFlags2 new_stage, VkAccessFlags2 new_access

) {
    assert(mip < rt->mip_count);
    cmd_transition_mip(r, cmd, rt->image, &rt->mip_states[mip], rt->aspect, mip, new_stage, new_access, new_layout,
                       VK_QUEUE_FAMILY_IGNORED);
}

void rt_transition_all(VkBackend *r, VkCommandBuffer cmd, RenderTarget *rt, VkImageLayout new_layout,
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
// ============================================================
// Pass API implementation
// ============================================================

void push_constants(VkBackend *r, VkCommandBuffer cmd, ByteSpan data) {
    assert(data.size > 0 && data.size <= 256 && data.size % 4 == 0);
    vkCmdPushConstants(cmd, r->bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, data.size, data.data);
}

// ---- Root-argument payload helpers ----
//
// The per-draw/per-dispatch argument block is a parameter of the work, in the
// style of NoGraphicsAPI's root: a ByteSpan copied with the draw/dispatch call.
// Payload structs are declared once via PUSH_CONSTANT and shared with Slang, so
// the CPU and shader layouts cannot drift. Push constants do not reference the
// caller's memory after the call, so a stack-local payload is fine.

static void emit_root_data(VkBackend *r, VkCommandBuffer cmd, ByteSpan root) {
    assert(root.size % 4 == 0 && "root payload must be a multiple of 4 bytes");
    assert(root.size <= 256 && "root payload exceeds the 256-byte push-constant range");
    assert(root.size > 0 && "empty root payload; pass a real struct or drop the argument");
    push_constants(r, cmd, root);
}

void cmd_draw(VkBackend *r, VkCommandBuffer cmd, ByteSpan root, uint32_t vertex_count,
                           uint32_t instance_count) {
    emit_root_data(r, cmd, root);
    vkCmdDraw(cmd, vertex_count, instance_count, 0, 0);
}

void dispatch_push(VkBackend *r, VkCommandBuffer cmd, ByteSpan root, uint32_t group_count_x,
                                uint32_t group_count_y, uint32_t group_count_z) {
    emit_root_data(r, cmd, root);
    vkCmdDispatch(cmd, group_count_x, group_count_y, group_count_z);
}

void end_pass(VkCommandBuffer cmd) { vkCmdEndRendering(cmd); }

static void pass_transition_attachment(VkBackend *r, VkCommandBuffer cmd, const PassAttachment *a) {
    if (!a->target) {
        // Swapchain attachment: the backend owns swapchain image state.
        image_transition_swapchain(r, cmd, &r->swapchain, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        return;
    }

    if (a->target->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
        rt_transition_all(r, cmd, a->target, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    } else {
        rt_transition_all(r, cmd, a->target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
}

void begin_pass(VkBackend *r, VkCommandBuffer cmd, const PassDesc *desc) {
    assert(r && cmd && desc);
    assert(desc->color_count <= MAX_COLOR_ATTACHMENTS);
    assert((desc->colors && desc->color_count) || desc->color_count == 0);

    const RenderTarget *area = desc->color_count && desc->colors[0].target ? desc->colors[0].target
                               : (desc->depth ? desc->depth->target : NULL);
    if (!area && desc->shader_read_count)
        area = desc->shader_reads[0];
    if (!area && desc->shader_write_count)
        area = desc->shader_writes[0];
    VkExtent2D area_extent;
    if (area) {
        area_extent = (VkExtent2D){.width = area->width, .height = area->height};
    } else {
        // Swapchain-only pass: render area comes from the swapchain extent.
        assert(desc->color_count && desc->colors[0].swapchain_view &&
               "begin_pass needs a target or a swapchain view to derive the render area");
        area_extent = r->swapchain.extent;
    }

    forEach(i, desc->color_count) pass_transition_attachment(r, cmd, &desc->colors[i]);
    if (desc->depth)
        pass_transition_attachment(r, cmd, desc->depth);

    forEach(i, desc->shader_read_count) {
        rt_transition_all(r, cmd, desc->shader_reads[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    forEach(i, desc->shader_write_count) {
        rt_transition_all(r, cmd, desc->shader_writes[i], VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    flush_barriers(r, cmd);

    if (desc->color_count == 0) {
        // Compute pass: no rendering scope, just the pipeline bind.
        if (desc->pipeline)
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->render_pipelines.pipelines[desc->pipeline - 1]);
        return;
    }

    VkRenderingAttachmentInfo color_attachments[MAX_COLOR_ATTACHMENTS];
    forEach(i, desc->color_count) {
        const PassAttachment *a = &desc->colors[i];
        color_attachments[i] = (VkRenderingAttachmentInfo){
            .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView   = a->target ? a->target->view : a->swapchain_view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp      = (VkAttachmentLoadOp)a->load,
            .storeOp     = (VkAttachmentStoreOp)a->store,
            .clearValue  = {.color = {{a->clear[0], a->clear[1], a->clear[2], a->clear[3]}}},
        };
    }

    VkRenderingAttachmentInfo depth_attachment = {0};
    if (desc->depth) {
        depth_attachment = (VkRenderingAttachmentInfo){
            .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView   = desc->depth->target->view,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .loadOp      = (VkAttachmentLoadOp)desc->depth->load,
            .storeOp     = (VkAttachmentStoreOp)desc->depth->store,
            .clearValue  = {.depthStencil = {.depth = desc->depth->clear[0]}},
        };
    }

    const VkRenderingInfo rendering = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {.extent = area_extent},
        .layerCount           = 1,
        .colorAttachmentCount = desc->color_count,
        .pColorAttachments    = color_attachments,
        .pDepthAttachment     = desc->depth ? &depth_attachment : NULL,
    };

    vkCmdBeginRendering(cmd, &rendering);
    vk_cmd_set_viewport_scissor(cmd, area_extent);
    if (desc->pipeline)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->render_pipelines.pipelines[desc->pipeline - 1]);
}

// ============================================================
// Delete queue
// ============================================================

void delete_queue_defer(VkBackend *r, uint64_t retire_value, DeferredDestroyFn fn, void *user) {
    DeleteQueue *q = &r->delete_queue;
    assert(fn && "deferred destroy needs a callback");
    assert(q->count < DELETE_QUEUE_CAPACITY && "delete queue exhausted: raise DELETE_QUEUE_CAPACITY");
    assert((q->count == 0 ||
            q->entries[(q->tail + DELETE_QUEUE_CAPACITY - 1) % DELETE_QUEUE_CAPACITY].retire_value <= retire_value) &&
           "retire values must be nondecreasing");

    q->entries[q->tail] = (DeleteQueueEntry){.retire_value = retire_value, .fn = fn, .user = user};
    q->tail             = (q->tail + 1) % DELETE_QUEUE_CAPACITY;
    q->count++;
}

void delete_queue_tick(VkBackend *r) {
    DeleteQueue *q = &r->delete_queue;
    if (q->count == 0)
        return;

    uint64_t completed = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(r->devc.device, r->timeline, &completed));

    while (q->count && q->entries[q->head].retire_value <= completed) {
        DeleteQueueEntry *e = &q->entries[q->head];
        e->fn(r, e->user);
        q->head = (q->head + 1) % DELETE_QUEUE_CAPACITY;
        q->count--;
    }
}

void delete_queue_drain(VkBackend *r) {
    DeleteQueue *q = &r->delete_queue;
    while (q->count) {
        DeleteQueueEntry *e = &q->entries[q->head];
        e->fn(r, e->user);
        q->head = (q->head + 1) % DELETE_QUEUE_CAPACITY;
        q->count--;
    }
}


bool vk_swapchain_acquire(VkDevice device, FlowSwapchain *sc, VkSemaphore image_available, VkFence fence,
                                       uint64_t timeout) {
    ///  PFN_vkAcquireNextImage2KHR
    VkResult r = vkAcquireNextImageKHR(device, sc->swapchain, timeout, image_available, fence, &sc->current_image);

    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        sc->needs_recreate |= r == VK_SUBOPTIMAL_KHR;
        return true;
    }

    if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
        sc->needs_recreate = true;
        return false;
    }

    VK_CHECK(r);
    return false;
}

bool vk_swapchain_present(VkQueue present_queue, FlowSwapchain *sc, const VkSemaphore *waits,
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


void vk_instance_create(VkBackend *r, VkBackendDesc *desc) {
    TracyCZoneN(ctx, "renderer_create", 1);
    // Instance
    // Debug messenger
    // Physical device
    // Device info
    // Logical device
    // Queues
    // Frame contexts

    bool enable_validation = desc->enable_validation;
    if (enable_validation && !is_instance_layer_supported("VK_LAYER_KHRONOS_validation")) {
        log_warn("[instance] VK_LAYER_KHRONOS_validation not present, disabling validation");
        enable_validation = false;
    }

    bool enable_debug_utils = enable_validation && is_instance_extension_supported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (enable_validation && !enable_debug_utils) {
        log_warn("[instance] %s not supported by loader", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

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

        if (enable_debug_utils) {
            extensions[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        }

        const char *layers[8];
        uint32_t    layer_count = desc->instance_layer_count;

        if (layer_count) {
            memcpy(layers, desc->instance_layers, sizeof(char *) * layer_count);
        }
        if (enable_validation) {
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
                 desc->app_name ? desc->app_name : "(null)", enable_validation,
                 enable_validation && desc->enable_gpu_based_validation);
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
        if (enable_validation && desc->enable_gpu_based_validation) {
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
        if (enable_validation && enable_debug_utils) {
            VkDebugUtilsMessengerCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,

                                                     .messageSeverity = desc->validation_severity,

                                                     .messageType = desc->validation_types,

                                                     .pfnUserCallback = debug_callback};

            {
                VK_CHECK(vkCreateDebugUtilsMessengerEXT(r->instance.instance, &ci, r->vk_allocator_callbacks,
                                               &r->instance.debug_messenger));

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

    TracyCZoneEnd(ctx);
}

void vk_backend_create(VkBackend *r, VkBackendDesc *desc) {
    //
    // 3. Pick Physical Device
    //
    {
        r->devc.physical_device = pick_physical_device(r->instance.instance, r->surface, desc);

        if (r->devc.physical_device == VK_NULL_HANDLE) {
            log_error("No GPU found");
            exit(EXIT_FAILURE);
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

        VkBackendCaps caps = default_caps();

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
    }

    // Submission timeline: monotonically increasing, signalled by every submit.
    // Requires the timelineSemaphore feature, which the renderer already enables.
    assert(r->info.feature_chain.v12.timelineSemaphore);
    vk_create_timeline_semaphore(r->devc.device, &r->timeline);
    r->timeline_last_submitted = 0;

    log_info("[renderer] frame contexts created");

    r->current_frame     = 0;

    log_info("[renderer] initialization complete");

    mu_id_pool_init(&r->texture_system.id_pool, MAX_BINDLESS_TEXTURES);

    mu_id_pool_init(&r->sampler_pool, MAX_BINDLESS_SAMPLERS);

    mu_id_pool_init(&r->render_pipelines.pipeline_id_pool, MAX_PIPELINES);
    vk_cmd_create_pool(r->devc.device, r->devc.graphics_queue_index, true, false, &r->one_time_gfx_pool);

    uint32_t fb_w = desc->width, fb_h = desc->height;
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

    VK_CHECK(vkCreateDescriptorSetLayout(r->devc.device, &ci, NULL, &r->bindless_system.set_layout));

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

    VK_CHECK(vkCreateDescriptorPool(r->devc.device, &cib, NULL, &r->bindless_system.pool));

    VkDescriptorSetAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = r->bindless_system.pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &r->bindless_system.set_layout,
    };

    VK_CHECK(vkAllocateDescriptorSets(r->devc.device, &ai, &r->bindless_system.set));

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

    VK_CHECK(vmaCreateAllocator(&allocatorInfo, &r->devc.vmaallocator));
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

    {
        r->enable_graphics_profiler = desc->enable_graphics_profiler;
        forEach(i, MAX_FRAMES_IN_FLIGHT) {
            gpu_profiler_init(&r->gpuprofiler[i], r->devc.device, r->info.properties.limits.timestampPeriod,
                              desc->enable_graphics_profiler,
                              desc->enable_pipeline_stats &&
                                  r->info.feature_chain.core.features.pipelineStatisticsQuery);
        }
    }

    memset(&r->default_samplers, 0, sizeof(r->default_samplers));

    {
        DefaultSamplerTable *table = &r->default_samplers;

        // Zero-value SamplerDesc = linear filtering, repeat addressing. Name only deltas.
        const SamplerDesc linear_wrap  = {0};
        const SamplerDesc linear_clamp = {.address_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .address_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        const SamplerDesc nearest_wrap = {.min_filter  = VK_FILTER_NEAREST,
                                          .mag_filter  = VK_FILTER_NEAREST,
                                          .nearest_mips = true};
        const SamplerDesc nearest_clamp = {.min_filter = VK_FILTER_NEAREST,
                                           .mag_filter  = VK_FILTER_NEAREST,
                                           .nearest_mips = true,
                                           .address_u    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                           .address_v    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                           .address_w    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        const SamplerDesc aniso_wrap   = {.anisotropic = true};
        const SamplerDesc shadow       = {.compare_enabled       = true,
                                    .clamp_mode_override = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};

        sampler_create(r, &linear_wrap, &table->samplers[SAMPLER_LINEAR_WRAP]);
        sampler_create(r, &linear_clamp, &table->samplers[SAMPLER_LINEAR_CLAMP]);
        sampler_create(r, &nearest_wrap, &table->samplers[SAMPLER_NEAREST_WRAP]);
        sampler_create(r, &nearest_clamp, &table->samplers[SAMPLER_NEAREST_CLAMP]);
        sampler_create(r, &aniso_wrap, &table->samplers[SAMPLER_LINEAR_WRAP_ANISO]);
        sampler_create(r, &shadow, &table->samplers[SAMPLER_SHADOW]);
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

}

bool vk_frame_acquire(VkBackend *r) {
    FrameContext *f = &r->frames[r->current_frame];

    // Slot reuse waits for the submission that last used its commands and uploads.
    if (f->timeline_value != 0) {
        VkSemaphoreWaitInfo wait = {.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                    .semaphoreCount = 1,
                                    .pSemaphores    = &r->timeline,
                                    .pValues        = &f->timeline_value};
        VK_CHECK(vkWaitSemaphores(r->devc.device, &wait, UINT64_MAX));
    }


    buffer_pool_linear_reset(&r->cpu_pool);
    buffer_pool_ring_free_to(&r->staging_pool, f->staging_tail);

    GpuProfiler *frame_prof = &r->gpuprofiler[r->current_frame];

    uint64_t completed = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(r->devc.device, r->timeline, &completed));
    gpu_profiler_collect(frame_prof, r->devc.device, completed);

    vkResetCommandPool(r->devc.device, f->cmdbufpool, 0);

    // OUT_OF_DATE has no acquired image; SUBOPTIMAL is submitted before recreation.
    if (!vk_swapchain_acquire(r->devc.device, &r->swapchain, r->frames[r->current_frame].image_available_semaphore,
                              VK_NULL_HANDLE, UINT64_MAX)) {
        return false;
    }
    return true;
}

void vk_frame_submit(VkBackend *r) {
    TracyCZoneNC(ctx, "submit_frame", 0xFF0000, 1);

    FrameContext *f   = &r->frames[r->current_frame];
    uint32_t      img = r->swapchain.current_image;

    if (r->staging_pool.type == BUFFER_POOL_RING)
        f->staging_tail = r->staging_pool.ring.head;

    VkCommandBufferSubmitInfo cmd = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = f->cmdbuf};

    VkSemaphoreSubmitInfo wait = {.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = f->image_available_semaphore,
                                  .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};    VkSemaphoreSubmitInfo signal = {.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = r->swapchain.render_finished[img],
                                  .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};

    // The device-wide timeline advances on every submission; frame-slot reuse,
    // deferred destruction, and readback waits all key on this value.
    uint64_t retire_value              = r->timeline_last_submitted + 1;
    r->timeline_last_submitted         = retire_value;
    f->timeline_value                  = retire_value;

    VkSemaphoreSubmitInfo timeline_signal = {.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                             .semaphore = r->timeline,
                                             .value     = retire_value,
                                             .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};

    VkSemaphoreSubmitInfo signals[] = {signal, timeline_signal};

    VkSubmitInfo2 submit = {.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                            .waitSemaphoreInfoCount   = 1,
                            .pWaitSemaphoreInfos      = &wait,
                            .commandBufferInfoCount   = 1,
                            .pCommandBufferInfos      = &cmd,
                            .signalSemaphoreInfoCount = 2,
                            .pSignalSemaphoreInfos    = signals};

    VK_CHECK(vkQueueSubmit2(r->devc.graphics_queue, 1, &submit, VK_NULL_HANDLE));

    vk_swapchain_present(r->devc.present_queue, &r->swapchain,
                         &r->swapchain.render_finished[r->swapchain.current_image], 1);

    TracyCZoneEnd(ctx);
}

void wait_idle(VkBackend *r) {
    VK_CHECK(vkDeviceWaitIdle(r->devc.device));
}

void vk_backend_destroy(VkBackend *r) {
    assert(r->delete_queue.count == 0);
    forEach(i, MAX_FRAMES_IN_FLIGHT) {
        gpu_profiler_destroy(&r->gpuprofiler[i], r->devc.device);
        vkDestroySemaphore(r->devc.device, r->frames[i].image_available_semaphore, NULL);
        vkDestroyCommandPool(r->devc.device, r->frames[i].cmdbufpool, NULL);
    }
    forEach(i, MAX_PIPELINES) {
        if (r->render_pipelines.pipelines[i]) {
            vkDestroyPipeline(r->devc.device, r->render_pipelines.pipelines[i], NULL);
            mu_id_pool_destroy_id(&r->render_pipelines.pipeline_id_pool, i);
        }
    }
    forEach(i, MAX_BINDLESS_TEXTURES) {
        if (r->texture_system.textures[i].image)
            destroy_texture(r, i);
    }
    forEach(i, MAX_BINDLESS_SAMPLERS) {
        if (r->samplers[i])
            sampler_destroy(r, i);
    }
    buffer_pool_destroy(r, &r->staging_pool);
    buffer_pool_destroy(r, &r->gpu_pool);
    buffer_pool_destroy(r, &r->cpu_pool);
    vk_swapchain_destroy(r->devc.device, &r->swapchain, &r->texture_system.id_pool);
    vkDestroyPipelineLayout(r->devc.device, r->bindless_system.pipeline_layout, NULL);
    vkDestroyDescriptorPool(r->devc.device, r->bindless_system.pool, NULL);
    vkDestroyDescriptorSetLayout(r->devc.device, r->bindless_system.set_layout, NULL);
    vkDestroySemaphore(r->devc.device, r->timeline, NULL);
    vkDestroyCommandPool(r->devc.device, r->one_time_gfx_pool, NULL);
    if (r->transfer_pool)
        vkDestroyCommandPool(r->devc.device, r->transfer_pool, NULL);
    vkDestroyPipelineCache(r->devc.device, r->devc.pipeline_cache, NULL);
    mu_id_pool_deinit(&r->render_pipelines.pipeline_id_pool);
    mu_id_pool_deinit(&r->sampler_pool);
    mu_id_pool_deinit(&r->texture_system.id_pool);
    vmaDestroyAllocator(r->devc.vmaallocator);
    vkDestroyDevice(r->devc.device, r->vk_allocator_callbacks);
    r->devc.device = VK_NULL_HANDLE;
}

void vk_instance_destroy(VkBackend *r) {
    assert(r->devc.device == VK_NULL_HANDLE);
    if (r->instance.debug_messenger)
        vkDestroyDebugUtilsMessengerEXT(r->instance.instance, r->instance.debug_messenger, r->vk_allocator_callbacks);
    vkDestroyInstance(r->instance.instance, r->vk_allocator_callbacks);
    r->instance = (InstanceContext){0};
}

void destroy_texture(VkBackend *r, TextureID id) {
    assert(id < MAX_BINDLESS_TEXTURES && r->texture_system.textures[id].image);
    Texture *texture = &r->texture_system.textures[id];
    vkDestroyImageView(r->devc.device, texture->view, r->vk_allocator_callbacks);
    vmaDestroyImage(r->devc.vmaallocator, texture->image, texture->allocation);
    *texture = (Texture){0};
    r->texture_system.info[id] = (TextureInfo){0};
    mu_id_pool_destroy_id(&r->texture_system.id_pool, id);
}

void sampler_destroy(VkBackend *r, SamplerID id) {
    assert(id < MAX_BINDLESS_SAMPLERS && r->samplers[id]);
    vkDestroySampler(r->devc.device, r->samplers[id], NULL);
    r->samplers[id] = VK_NULL_HANDLE;
    mu_id_pool_destroy_id(&r->sampler_pool, id);
}
