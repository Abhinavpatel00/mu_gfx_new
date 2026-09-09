#ifndef MU_TRACY_H
#define MU_TRACY_H

#if defined(TRACY_ENABLE)
    #if defined(__has_include)
        #if __has_include("../external/tracy/public/tracy/TracyC.h")
            #include "../external/tracy/public/tracy/TracyC.h"
        #elif __has_include("external/tracy/public/tracy/TracyC.h")
            #include "external/tracy/public/tracy/TracyC.h"
        #else
            #define MU_TRACY_DISABLED
        #endif
    #else
        #include "../external/tracy/public/tracy/TracyC.h"
    #endif
#else
    #define MU_TRACY_DISABLED
#endif

#ifdef MU_TRACY_DISABLED
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ___tracy_c_zone_context {
    uint32_t id;
    int32_t active;
} TracyCZoneCtx;

typedef const void* TracyCLockCtx;
typedef const void* TracyCSharedLockCtx;

#define TracyCZone(c,x)
#define TracyCZoneN(c,x,y)
#define TracyCZoneC(c,x,y)
#define TracyCZoneNC(c,x,y,z)
#define TracyCZoneEnd(c)
#define TracyCZoneText(c,x,y)
#define TracyCZoneTextF(c,x,...)
#define TracyCZoneName(c,x,y)
#define TracyCZoneNameF(c,x,...)
#define TracyCZoneColor(c,x)
#define TracyCZoneValue(c,x)

#define TracyCAlloc(x,y)
#define TracyCFree(x)
#define TracyCMemoryDiscard(x)

#define TracyCAllocN(x,y,z)
#define TracyCFreeN(x,y)

#define TracyCFrameMark
#define TracyCFrameMarkNamed(x)
#define TracyCFrameMarkStart(x)
#define TracyCFrameMarkEnd(x)
#define TracyCFrameImage(x,y,z,w,a)

#define TracyCPlot(x,y)
#define TracyCPlotF(x,y)
#define TracyCPlotI(x,y)
#define TracyCPlotConfig(x,y,z,w,a)

#define TracyCMessage(x,y)
#define TracyCMessageL(x)
#define TracyCMessageC(x,y,z)
#define TracyCMessageLC(x,y)
#define TracyCAppInfo(x,y)

#define TracyCZoneS(x,y,z)
#define TracyCZoneNS(x,y,z,w)
#define TracyCZoneCS(x,y,z,w)
#define TracyCZoneNCS(x,y,z,w,a)

#define TracyCAllocS(x,y,z)
#define TracyCFreeS(x,y)
#define TracyCMemoryDiscardS(x,y)

#define TracyCAllocNS(x,y,z,w)
#define TracyCFreeNS(x,y,z)

#define TracyCMessageS(x,y,z)
#define TracyCMessageLS(x,y)
#define TracyCMessageCS(x,y,z,w)
#define TracyCMessageLCS(x,y,z)

#define TracyCLockCtx(l)
#define TracyCLockAnnounce(l)
#define TracyCLockTerminate(l)
#define TracyCLockBeforeLock(l)
#define TracyCLockAfterLock(l)
#define TracyCLockAfterUnlock(l)
#define TracyCLockAfterTryLock(l,x)
#define TracyCLockMark(l)
#define TracyCLockCustomName(l,x,y)

#define TracyCSharedLockCtx(l)
#define TracyCSharedLockAnnonce(l)
#define TracyCSharedLockTerminate(l)
#define TracyCSharedLockBeforeLock(l)
#define TracyCSharedLockAfterLock(l)
#define TracyCSharedLockAfterUnlock(l)
#define TracyCSharedLockAfterTryLock(l,x)
#define TracyCSharedLockBeforeSharedLock(l)
#define TracyCSharedLockAfterSharedLock(l)
#define TracyCSharedLockAfterSharedUnlock(l)
#define TracyCSharedLockAfterTrySharedLock(l,x)
#define TracyCSharedLockMark(l)
#define TracyCSharedLockCustomName(l,x,y)

#define TracyCIsConnected 0
#define TracyCIsStarted 0

#define TracyCBeginSamplingProfiling() 0
#define TracyCEndSamplingProfiling()

#define TracyCSetThreadName(name)

#ifdef __cplusplus
}
#endif
#endif // MU_TRACY_DISABLED

#endif // MU_TRACY_H
