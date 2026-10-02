#pragma once

#include <gx2/enum.h>
#include <stdint.h>

// --- Screen selection -------------------------------------------------------
#define WUPS_STREAMING_SCREEN_DRC 0
#define WUPS_STREAMING_SCREEN_TV  1

// --- Capture size -----------------------------------------------------------
// GX2CopySurface cannot resize (the Cafe SDK removed mismatched-dimension copies
// in 2.04), so capture is always 1:1 from the game's colour buffer. Anything
// smaller is produced afterwards by a CPU downscale on the encoder thread.
// "NATIVE" therefore costs the least and, because virtually every Wii U game
// renders its TV output at exactly 1280x720, is also how you get real 720p.
#define WUPS_STREAMING_SIZE_NATIVE 0
#define WUPS_STREAMING_SIZE_720P   1
#define WUPS_STREAMING_SIZE_480P   2
#define WUPS_STREAMING_SIZE_360P   3
#define WUPS_STREAMING_SIZE_240P   4
// Appended rather than slotted in, so settings saved by older builds keep meaning
// what they meant. 320x180 is an exact 4:1 of 720p, so it takes the fixed-ratio
// conversion path just like 360p (exact 2:1) does.
#define WUPS_STREAMING_SIZE_180P   5
#define WUPS_STREAMING_SIZE_LAST   WUPS_STREAMING_SIZE_180P

// --- Encoder cores ------------------------------------------------------------
// Which cores encode. The first listed core leads (takes frames off the capture
// queue, splices the bands, hands the result to the sender); the others each
// compress a band of the same frame in parallel. A core the running game keeps
// busy is detected and benched automatically, so listing one costs little.
#define WUPS_STREAMING_CORES_0_2 0
#define WUPS_STREAMING_CORES_0   1
#define WUPS_STREAMING_CORES_2   2
#define WUPS_STREAMING_CORES_1   3
#define WUPS_STREAMING_CORES_ALL 4
#define WUPS_STREAMING_CORES_LAST WUPS_STREAMING_CORES_ALL

// --- Encoder path ---------------------------------------------------------------
// FAST: multi-core, converts straight to YCbCr planes. SAFE: the single-core RGB
// path every earlier hardware measurement was taken on - kept as a known-good
// fallback that is one menu change away.
#define WUPS_STREAMING_PATH_FAST 0
#define WUPS_STREAMING_PATH_SAFE 1

// --- GPU synchronisation --------------------------------------------------------
// ASYNC: the game's render thread only flushes and records a GPU timestamp; the
// encoder waits for it. BLOCKING: GX2DrawDone on the render thread, which stalls
// the game every captured frame - the old behaviour.
#define WUPS_STREAMING_GPUSYNC_ASYNC    0
#define WUPS_STREAMING_GPUSYNC_BLOCKING 1

// --- Colour handling --------------------------------------------------------
// AUTO converts to sRGB when the scan buffer was configured with an sRGB format,
// which is what upstream was missing and why its README says "some games might
// be too dark, some might be too bright".
#define WUPS_STREAMING_COLOR_AUTO 0
#define WUPS_STREAMING_COLOR_RAW  1

#define STREAM_QUALITY_MIN 10
#define STREAM_QUALITY_MAX 95

#define STREAM_FRAMESKIP_MIN 0
#define STREAM_FRAMESKIP_MAX 9

// Capture only runs when a client is listening *and* the game owns the screen.
// Both are cleared before any teardown, and teardown then waits for
// gCaptureInFlight to drain, which is what keeps the GX2 hook from running
// against buffers that are being freed.
extern volatile bool gClientConnected;
extern volatile bool gHasForeground;

/** Number of GX2-hook captures in progress. Touched with __atomic builtins only. */
extern int32_t gCaptureInFlight;

static inline bool StreamingActive() {
    return gClientConnected && gHasForeground;
}

/** Blocks until no capture is running. Clear the gates above first. */
void StreamWaitForCapturesToFinish();

extern int32_t gScreen;
extern int32_t gCaptureSize;
extern int32_t gColorMode;
extern int32_t gQuality;
extern int32_t gFrameSkip;
extern int32_t gEncoderCores;
extern int32_t gEncodePath;
extern int32_t gGpuSync;

/**
 * Set by the encoder when asynchronous GPU sync keeps timing out, so the capture
 * hook falls back to blocking. Cleared at every title start, so each game gets a
 * fresh try.
 */
extern volatile bool gGpuSyncFallback;

static inline bool UseBlockingGpuSync() {
    return gGpuSync == WUPS_STREAMING_GPUSYNC_BLOCKING || gGpuSyncFallback;
}

// Learned by patching GX2SetTVBuffer / GX2SetDRCBuffer. The colour buffer handed
// to GX2CopyColorBufferToScanBuffer does not carry the scan buffer's format, so
// the only way to know whether the output is sRGB is to watch it being set.
extern volatile GX2SurfaceFormat gTVSurfaceFormat;
extern volatile GX2SurfaceFormat gDRCSurfaceFormat;
