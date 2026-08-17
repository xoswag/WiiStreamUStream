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
extern int32_t gEncoderCore;

// Learned by patching GX2SetTVBuffer / GX2SetDRCBuffer. The colour buffer handed
// to GX2CopyColorBufferToScanBuffer does not carry the scan buffer's format, so
// the only way to know whether the output is sRGB is to watch it being set.
extern volatile GX2SurfaceFormat gTVSurfaceFormat;
extern volatile GX2SurfaceFormat gDRCSurfaceFormat;
