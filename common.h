#define IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE (1)
#define IMGUI_IMPL_VULKAN_USE_VOLK
#define CIMGUI_USE_GLFW
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_USE_VULKAN
#define CGLM_ALL_UNALIGNED
#include "external/cglm/include/cglm/cglm.h"
#include "external/cglm/include/cglm/types.h"
#include "external/cglm/include/cglm/vec3.h"

#define VK_NO_PROTOTYPES
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND


#include <GLFW/glfw3native.h>
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "external/VulkanMemoryAllocator/include/vk_mem_alloc.h"
#include "external/logger-c/logger/logger.h"
#include "external/volk/volk.h"

#include "external/cimgui/cimgui.h"
#include "external/cimgui/cimgui_impl.h"

#include "external/debugbreak/debugbreak.h"
#include "external/tracy/public/tracy/TracyC.h"
#include "external/mu/mu.h"
#include "src/constant.h"
#include "stdbool.h"
#include <stdio.h>

#include "external/logger-c/logger/logger.h"
// macros
#define ALIGNAS(x) __attribute__((aligned(x)))

#define ARRAY_COUNT(array) (sizeof(array)) / (sizeof(array[1]))

#define forEach(i, count) for (uint32_t i = 0; i < (count); i++)

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define CLAMP(v, mn, mx) MIN(MAX(v, mn), mx)
#define VK_CHECK(x)                                                                                                    \
    do {                                                                                                               \
        VkResult err = (x);                                                                                            \
        if (err != VK_SUCCESS) {                                                                                       \
            log_fatal("Vulkan error %d at %s:%d", err, __FILE__, __LINE__);                                            \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)

#define SWAP(type, a, b)                                                                                               \
    do {                                                                                                               \
        type _temp = (a);                                                                                              \
        (a)        = (b);                                                                                              \
        (b)        = _temp;                                                                                            \
    } while (0)



#if defined(_MSC_VER)
    #define FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define FORCE_INLINE inline __attribute__((always_inline))
#else
    #define FORCE_INLINE inline
#endif



