#include "../common.h"
#include "../external/xxHash/xxhash.h"
#include <errno.h>
/*

whatevet the fuck u write no one cares here 
*/

// ededdeded ao
//
FORCE_INLINE void vk_create_fence(VkDevice device, bool signaled, VkFence* out_fence)
{
    VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0U};

    VK_CHECK(vkCreateFence(device, &info, NULL, out_fence));
}


FORCE_INLINE void vk_create_fences(VkDevice device, uint32_t count, bool signaled, VkFence* out_fences)
{
    for(uint32_t i = 0; i < count; i++)
        vk_create_fence(device, signaled, &out_fences[i]);
}


FORCE_INLINE void vk_wait_fence(VkDevice device, VkFence fence, uint64_t timeout_ns)
{
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, timeout_ns));
}


FORCE_INLINE void vk_wait_fences(VkDevice device, uint32_t count, const VkFence* fences, bool wait_all, uint64_t timeout_ns)
{
    VK_CHECK(vkWaitForFences(device, count, fences, wait_all ? VK_TRUE : VK_FALSE, timeout_ns));
}


FORCE_INLINE void vk_reset_fence(VkDevice device, VkFence fence)
{
    VK_CHECK(vkResetFences(device, 1, &fence));
}


FORCE_INLINE void vk_reset_fences(VkDevice device, uint32_t count, const VkFence* fences)
{
    VK_CHECK(vkResetFences(device, count, fences));
}


FORCE_INLINE bool vk_fence_is_signaled(VkDevice device, VkFence fence)
{
    return vkGetFenceStatus(device, fence) == VK_SUCCESS;
}


FORCE_INLINE void vk_destroy_fences(VkDevice device, uint32_t count, VkFence* fences)
{
    for(uint32_t i = 0; i < count; i++)
    {
        if(fences[i] != VK_NULL_HANDLE)
        {
            vkDestroyFence(device, fences[i], NULL);
            fences[i] = VK_NULL_HANDLE;
        }
    }
}


/*
===============================================================================
Semaphore Helpers
===============================================================================
*/

FORCE_INLINE void vk_create_semaphore(VkDevice device, VkSemaphore* out_semaphore)
{
    VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    VK_CHECK(vkCreateSemaphore(device, &info, NULL, out_semaphore));
}


FORCE_INLINE void vk_create_semaphores(VkDevice device, uint32_t count, VkSemaphore* out_semaphores)
{
    for(uint32_t i = 0; i < count; i++)
        vk_create_semaphore(device, &out_semaphores[i]);
}


FORCE_INLINE void vk_destroy_semaphores(VkDevice device, uint32_t count, VkSemaphore* semaphores)
{
    for(uint32_t i = 0; i < count; i++)
    {
        if(semaphores[i] != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device, semaphores[i], NULL);
            semaphores[i] = VK_NULL_HANDLE;
        }
    }
}


/*
===============================================================================
Command Pool Helpers
===============================================================================
*/

FORCE_INLINE VkCommandBufferLevel vk_cmd_level(bool primary)
{
    return primary ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY;
}


FORCE_INLINE void vk_cmd_create_pool(VkDevice device, uint32_t queue_family_index, bool transient, bool resettable, VkCommandPool* out_pool)
{
    uint32_t flags = 0;

    if(transient)
        flags |= VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    if(resettable)
        flags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = flags, .queueFamilyIndex = queue_family_index};

    VK_CHECK(vkCreateCommandPool(device, &ci, NULL, out_pool));
}


FORCE_INLINE void vk_cmd_destroy_pool(VkDevice device, VkCommandPool pool)
{
    if(pool)
        vkDestroyCommandPool(device, pool, NULL);
}


FORCE_INLINE void vk_cmd_create_many_pools(VkDevice device, uint32_t queue_family_index, bool transient, bool resettable, uint32_t count, VkCommandPool* out_pools)
{
    for(uint32_t i = 0; i < count; i++)
        vk_cmd_create_pool(device, queue_family_index, transient, resettable, &out_pools[i]);
}


FORCE_INLINE void vk_cmd_destroy_many_pools(VkDevice device, uint32_t count, VkCommandPool* pools)
{
    for(uint32_t i = 0; i < count; i++)
        vk_cmd_destroy_pool(device, pools[i]);
}


/*
===============================================================================
Command Buffer Helpers
===============================================================================
*/



FORCE_INLINE void vk_cmd_alloc(VkDevice device, VkCommandPool pool, bool primary, VkCommandBuffer* out_cmd)
{
    VkCommandBufferAllocateInfo ci = {.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool        = pool,
                                      .level              = vk_cmd_level(primary),
                                      .commandBufferCount = 1};

    VK_CHECK(vkAllocateCommandBuffers(device, &ci, out_cmd));
}


FORCE_INLINE void vk_cmd_begin(VkCommandBuffer cmd, bool one_time)
{
    VkCommandBufferBeginInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = NULL,
        .flags = one_time ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0U,
        .pInheritanceInfo = NULL
    };

    VK_CHECK(vkBeginCommandBuffer(cmd, &ci));
}
FORCE_INLINE void vk_cmd_end(VkCommandBuffer cmd)
{
    VK_CHECK(vkEndCommandBuffer(cmd));
}


FORCE_INLINE void vk_cmd_reset(VkCommandBuffer cmd)
{
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
}


FORCE_INLINE void vk_cmd_reset_pool(VkDevice device, VkCommandPool pool)
{
    VK_CHECK(vkResetCommandPool(device, pool, 0));
}


/*
===============================================================================
One-time command helpers (NOT force inline)
===============================================================================
*/

static inline VkCommandBuffer vk_begin_one_time_cmd(VkDevice device, VkCommandPool pool)
{
    VkCommandBuffer cmd;

    VkCommandBufferAllocateInfo allocInfo = {.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool        = pool,
                                             .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};

    VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &cmd));

    VkCommandBufferBeginInfo beginInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

    VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

    return cmd;
}


static inline void vk_end_one_time_cmd(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd)
{
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};

    VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    vkFreeCommandBuffers(device, pool, 1, &cmd);
}






FORCE_INLINE uint32_t hash32_bytes(const void* data, size_t size)
{
    return (uint32_t)XXH32(data, size, 0);
}

FORCE_INLINE uint64_t hash64_bytes(const void* data, size_t size)
{
    return (uint64_t)XXH64(data, size, 0);
}

 FORCE_INLINE uint32_t round_up(uint32_t a, uint32_t b)
{
    return (a + b - 1) & ~(b - 1);
}
 FORCE_INLINE uint64_t round_up_64(uint64_t a, uint64_t b)
{
    return (a + b - 1) & ~(b - 1);
}

 FORCE_INLINE size_t c99_strnlen(const char* s, size_t maxlen)
{
    size_t i = 0;
    if(!s)
        return 0;
    for(; i < maxlen && s[i]; i++)
    {
    }
    return i;
}

















#define VK_IMAGE_VIEW_DEFAULT(img, fmt)                                                                          \
    (VkImageViewCreateInfo)                                                                                            \
    {                                                                                                                  \
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,                                                          \
        .pNext    = NULL,                                                                                              \
        .flags    = 0,                                                                                                 \
        .image    = (img),                                                                                             \
        .viewType = VK_IMAGE_VIEW_TYPE_2D,                                                                             \
        .format   = (fmt),                                                                                             \
        .components = {                                                                                                \
            VK_COMPONENT_SWIZZLE_IDENTITY,                                                                             \
            VK_COMPONENT_SWIZZLE_IDENTITY,                                                                             \
            VK_COMPONENT_SWIZZLE_IDENTITY,                                                                             \
            VK_COMPONENT_SWIZZLE_IDENTITY,                                                                             \
        },                                                                                                             \
        .subresourceRange = {                                                                                          \
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,                                                               \
            .baseMipLevel   = 0,                                                                                       \
            .levelCount     = VK_REMAINING_MIP_LEVELS,                                                                 \
            .baseArrayLayer = 0,                                                                                       \
            .layerCount     = VK_REMAINING_ARRAY_LAYERS,                                                               \
        },                                                                                                             \
    }



#define VK_IMAGE_DEFAULT_2D(w, h, fmt, usageFlags)                                                                     \
    (VkImageCreateInfo){                                                                                               \
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,                                                          \
        .pNext         = NULL,                                                                                         \
        .flags         = 0,                                                                                            \
        .imageType     = VK_IMAGE_TYPE_2D,                                                                             \
        .format        = (fmt),                                                                                        \
        .extent        = { (uint32_t)(w), (uint32_t)(h), 1 },                                                          \
        .mipLevels     = 1,                                                                                            \
        .arrayLayers   = 1,                                                                                            \
        .samples       = VK_SAMPLE_COUNT_1_BIT,                                                                        \
        .tiling        = VK_IMAGE_TILING_OPTIMAL,                                                                      \
        .usage         = (usageFlags),                                                                                 \
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,                                                                    \
        .queueFamilyIndexCount = 0,                                                                                    \
        .pQueueFamilyIndices   = NULL,                                                                                 \
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,                                                                    \
    }





static bool read_file(const char* path, void** out_data, size_t* out_size)
{
    *out_data = NULL;
    *out_size = 0;

    FILE* f = fopen(path, "rb");
    if(!f)
    {
        log_error("Failed to open '%s' (errno=%d)", path, errno);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);

    if(len <= 0)
    {
        log_error("Invalid size for '%s'", path);
        fclose(f);
        return false;
    }

    void* data = malloc((size_t)len);
    if(!data)
    {
        log_error("Out of memory reading '%s'", path);
        fclose(f);
        return false;
    }

    if(fread(data, 1, (size_t)len, f) != (size_t)len)
    {
        log_error("Short read for '%s'", path);
        free(data);
        fclose(f);
        return false;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)len;
    return true;
}





















#ifndef GPU_PROF_MAX_SCOPES
#define GPU_PROF_MAX_SCOPES 128
#endif

#ifndef GPU_PROF_NAME_MAX
#define GPU_PROF_NAME_MAX 32
#endif


typedef struct
{
    const char* name;

    uint32_t start_query;
    uint32_t end_query;

    uint32_t stats_query;

    double time_ms;

    uint64_t vs_invocations;
    uint64_t fs_invocations;
    uint64_t primitives;
} GpuPass;


typedef struct
{
    VkQueryPool timestamp_pool;
    VkQueryPool stats_pool;

    uint32_t query_count;
    uint32_t pass_count;

    float timestamp_period;

    bool enable_pipeline_stats;

    GpuPass passes[GPU_PROF_MAX_SCOPES];

} GpuProfiler;

#define MAX_GPU_PASSES GPU_PROF_MAX_SCOPES
FORCE_INLINE void gpu_profiler_init(GpuProfiler* p, VkDevice device, float timestamp_period, bool enable_pipeline_stats)
{
    p->enable_pipeline_stats = enable_pipeline_stats;
    p->timestamp_pool        = VK_NULL_HANDLE;
    p->stats_pool            = VK_NULL_HANDLE;
    p->query_count           = 0;
    p->pass_count            = 0;

    VkQueryPoolCreateInfo time_info = {.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType  = VK_QUERY_TYPE_TIMESTAMP,
                                       .queryCount = MAX_GPU_PASSES * 2};

    vkCreateQueryPool(device, &time_info, NULL, &p->timestamp_pool);

    if(enable_pipeline_stats)
    {
        VkQueryPoolCreateInfo stats_info = {.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                            .queryType  = VK_QUERY_TYPE_PIPELINE_STATISTICS,
                                            .queryCount = MAX_GPU_PASSES,
                                            .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT
                                                                  | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT
                                                                  | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT};

        VkResult stats_create_res = vkCreateQueryPool(device, &stats_info, NULL, &p->stats_pool);
        if(stats_create_res != VK_SUCCESS)
        {
            p->enable_pipeline_stats = false;
            p->stats_pool            = VK_NULL_HANDLE;
        }
    }

    p->timestamp_period = timestamp_period;
}

FORCE_INLINE void gpu_profiler_destroy(GpuProfiler* p, VkDevice device)
{
    if(p->timestamp_pool)
    {
        vkDestroyQueryPool(device, p->timestamp_pool, NULL);
        p->timestamp_pool = VK_NULL_HANDLE;
    }

    if(p->stats_pool)
    {
        vkDestroyQueryPool(device, p->stats_pool, NULL);
        p->stats_pool = VK_NULL_HANDLE;
    }

    p->query_count = 0;
    p->pass_count  = 0;
}

FORCE_INLINE void gpu_profiler_begin_frame(GpuProfiler* p, VkCommandBuffer cmd)
{
    if(!p || cmd == VK_NULL_HANDLE)
        return;

    p->query_count = 0;
    p->pass_count  = 0;

    vkCmdResetQueryPool(cmd, p->timestamp_pool, 0, MAX_GPU_PASSES * 2);

    if(p->enable_pipeline_stats && p->stats_pool != VK_NULL_HANDLE)
        vkCmdResetQueryPool(cmd, p->stats_pool, 0, MAX_GPU_PASSES);
}

FORCE_INLINE void gpu_profiler_begin_pass(GpuProfiler* p, VkCommandBuffer cmd, const char* name, VkPipelineStageFlagBits2 stage)
{


    uint32_t q = p->query_count++;

    GpuPass* pass = &p->passes[p->pass_count];

    pass->name        = name;
    pass->start_query = q;
    pass->stats_query = p->pass_count;

    vkCmdWriteTimestamp2(cmd, stage, p->timestamp_pool, q);

    if(p->enable_pipeline_stats && p->stats_pool != VK_NULL_HANDLE)
    {
        vkCmdBeginQuery(cmd, p->stats_pool, pass->stats_query, 0);
    }
}

FORCE_INLINE void gpu_profiler_end_pass(GpuProfiler* p, VkCommandBuffer cmd, VkPipelineStageFlagBits2 stage)
{


    uint32_t q = p->query_count++;

    GpuPass* pass = &p->passes[p->pass_count];

    pass->end_query = q;

    vkCmdWriteTimestamp2(cmd, stage, p->timestamp_pool, q);

    if(p->enable_pipeline_stats && p->stats_pool != VK_NULL_HANDLE)
    {
        vkCmdEndQuery(cmd, p->stats_pool, pass->stats_query);
    }

    p->pass_count++;
}

FORCE_INLINE void gpu_profiler_collect(GpuProfiler* p, VkDevice device)
{
    if (p->query_count == 0) return;

    uint64_t timestamps[MAX_GPU_PASSES * 2];

    VkResult time_res = vkGetQueryPoolResults(device, p->timestamp_pool, 0, p->query_count, p->query_count * sizeof(uint64_t), timestamps,
                                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    if(time_res != VK_SUCCESS)
        return;

    for(uint32_t i = 0; i < p->pass_count; i++)
    {
        GpuPass* pass = &p->passes[i];

        uint64_t t0 = timestamps[pass->start_query];
        uint64_t t1 = timestamps[pass->end_query];

        double ns = (t1 - t0) * p->timestamp_period;

        pass->time_ms = ns / 1000000.0;
    }

    if(!p->enable_pipeline_stats || p->stats_pool == VK_NULL_HANDLE || p->pass_count == 0)
        return;

    struct
    {
        uint64_t vs;
        uint64_t fs;
        uint64_t prim;
    } stats[MAX_GPU_PASSES];

    VkResult stats_res = vkGetQueryPoolResults(device, p->stats_pool, 0, p->pass_count, p->pass_count * sizeof(stats[0]), stats,
                                               sizeof(stats[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    if(stats_res != VK_SUCCESS)
        return;

    for(uint32_t i = 0; i < p->pass_count; i++)
    {
        p->passes[i].vs_invocations = stats[i].vs;
        p->passes[i].fs_invocations = stats[i].fs;
        p->passes[i].primitives     = stats[i].prim;
    }
}
#define GPU_SCOPE(prof,cmd,name,stage)                                       \
    for(int _gpu_scope_once =                                                \
            (gpu_profiler_begin_pass((prof),cmd,name,stage),0);              \
        _gpu_scope_once == 0;                                                \
        gpu_profiler_end_pass((prof),cmd,stage), _gpu_scope_once++)
























static void trigger_shader_compilation() {
    // Use system() to run the bash script
    // Note: system() blocks until the process finishes. 
    // For a quick compilation script, this is acceptable, 
    // but for better UX, consider using popen() or a thread.
    // Since dmon callback is already on a thread, blocking there is okay.
    // But if the script is slow, it might queue up. We'll use system() for simplicity.
    printf("[HotReload] Triggering shader compilation...\n");
    int result = system("bash compileslang.sh"); // Adjust path if needed
    if (result != 0) {
        fprintf(stderr, "[HotReload] compileslang.sh failed with code %d\n", result);
    }
}

