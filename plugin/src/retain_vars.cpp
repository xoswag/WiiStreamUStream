#include "retain_vars.hpp"

#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

volatile bool gClientConnected = false;
volatile bool gHasForeground   = false;

int32_t gCaptureInFlight = 0;

int32_t gScreen      = WUPS_STREAMING_SCREEN_TV;
int32_t gCaptureSize = WUPS_STREAMING_SIZE_NATIVE;
int32_t gColorMode   = WUPS_STREAMING_COLOR_AUTO;
int32_t gQuality      = 55;
int32_t gFrameSkip    = 0;
int32_t gEncoderCores = WUPS_STREAMING_CORES_0_2;
int32_t gEncodePath   = WUPS_STREAMING_PATH_FAST;
int32_t gGpuSync      = WUPS_STREAMING_GPUSYNC_ASYNC;

volatile bool gGpuSyncFallback = false;

volatile GX2SurfaceFormat gTVSurfaceFormat  = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
volatile GX2SurfaceFormat gDRCSurfaceFormat = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;

void StreamWaitForCapturesToFinish() {
    // A full fence (`sync`) between the caller's gate stores and the counter
    // loads below; the capture side has the mirror-image fence. Without both, a
    // capture could read the gate as still open while this reads the counter as
    // zero.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    OSMemoryBarrier();
    // One second is far longer than a capture can legitimately take; the bound
    // exists so a wedged GPU cannot hang application shutdown.
    for (int i = 0; i < 1000 && __atomic_load_n(&gCaptureInFlight, __ATOMIC_ACQUIRE) > 0; i++) {
        OSSleepTicks(OSMillisecondsToTicks(1));
    }
}
