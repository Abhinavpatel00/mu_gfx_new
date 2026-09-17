#define CGLM_ALL_UNALIGNED
#include "external/cglm/include/cglm/cglm.h"
#include "external/cglm/include/cglm/types.h"
#include "external/cglm/include/cglm/vec3.h"

#include "src/platform.h"
#include "src/input_rgfw.h"
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "external/VulkanMemoryAllocator/include/vk_mem_alloc.h"
#include "external/logger-c/logger/logger.h"
#include "external/volk/volk.h"

#include "src/nuklear_ui.h"

#include "external/debugbreak/debugbreak.h"
#include "src/tracy.h"
#include "external/mu/mu.h"
#include "src/constant.h"
#include "stdbool.h"
#include <stdio.h>

#include "external/logger-c/logger/logger.h"
// macros
#define ALIGNAS(x) __attribute__((aligned(x)))

#define ARRAY_COUNT(array) (sizeof(array)) / (sizeof(array[1]))

#define forEach(i, count) for (uint32_t i = 0; i < (count); i++)


#define RUN_ONCE for(static int _once = 1; _once; _once = 0)

#define PUSH_CONSTANT(name, BODY)                                                                                      \
    typedef struct ALIGNAS(16) name##_init                                                                             \
    {                                                                                                                  \
        BODY                                                                                                           \
    } name##_init;                                                                                                     \
    enum                                                                                                               \
    {                                                                                                                  \
        name##_pad_size = 256 - sizeof(name##_init)                                                                    \
    };                                                                                                                 \
                                                                                                                       \
    typedef struct ALIGNAS(16) name                                                                                    \
    {                                                                                                                  \
        BODY uint8_t _pad[name##_pad_size];                                                                            \
    } name;                                                                                                            \
                                                                                                                       \
    _Static_assert(sizeof(name) == 256, "Push constant != 256");








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
    #define FORCE_INLINE static inline __attribute__((always_inline))
#else
    #define FORCE_INLINE static inline
#endif

