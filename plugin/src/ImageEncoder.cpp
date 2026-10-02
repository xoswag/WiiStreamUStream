/****************************************************************************
 * Copyright (C) 2018 Maschell
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 ****************************************************************************/
#include "ImageEncoder.hpp"
#include "JpegStitch.hpp"
#include "ScreenCapture.hpp"
#include "StreamProtocol.h"
#include "StreamSender.hpp"
#include "YuvConvert.hpp"
#include "retain_vars.hpp"
#include "utils/logger.h"

#include <coreinit/cache.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

/*
 * How a frame is encoded (FAST path)
 * ----------------------------------
 * One *leader* thread and up to two *followers*, each pinned to its own core.
 *
 *   leader:    take a captured frame -> wait for its GPU copy to land
 *              -> hand each follower a band of the frame -> do the last band itself
 *              -> wait for the followers -> splice the bands into one JPEG
 *              -> give it to the sender thread
 *   follower:  convert its band straight to YCbCr 4:2:0 -> compress it
 *
 * So the per-frame CPU work divides across however many cores are actually
 * free. A follower whose core the game keeps busy is *benched* - noticed by it
 * falling well behind the leader, or by it missing a frame's deadline - and is
 * re-admitted only after it encodes a real band, on the side, about as fast as
 * the leader does. Listing a contended core therefore never makes things much
 * worse than leaving it out; it just stops helping.
 *
 * The SAFE path is the single-core RGB pipeline the earlier hardware
 * measurements were taken on, kept as a one-menu-option fallback.
 */

namespace ImageEncoder {
namespace {

constexpr int MAX_WORKERS = 3;

constexpr uint32_t LEADER_STACK_SIZE   = 0x40000;
constexpr uint32_t FOLLOWER_STACK_SIZE = 0x20000;

// Below a typical game thread (~16; lower is more important on Cafe OS), so the
// game always wins its own cores and the encoder only gets time it leaves idle.
constexpr int WORKER_PRIORITY = 25;

/**
 * 4:2:0, not upstream's 4:1:1. Both store a quarter of the chroma, but 4:2:0 is
 * the format every JPEG decoder is tuned for and it subsamples vertically as
 * well as horizontally, which suits real game footage better.
 */
constexpr int JPEG_SUBSAMPLING = TJSAMP_420;

/** How long to wait for a capture's GPU copy before dropping the frame. */
constexpr uint32_t GPU_WAIT_TIMEOUT_MS = 250;
/** Consecutive GPU-wait timeouts after which async sync is abandoned for this title. */
constexpr uint32_t GPU_FALLBACK_STREAK = 3;

/**
 * Once the leader has finished its own band it waits this much longer than its
 * band took (or the floor, whichever is more) for the followers, then gives the
 * frame up rather than let one stalled core hold the whole stream.
 */
constexpr uint32_t FOLLOWER_WAIT_MIN_US = 12000;
/** A follower slower per row than this multiple of the leader is benched... */
constexpr uint32_t SLOW_RATIO_X10 = 18;
/** ...after this many frames in a row of it. */
constexpr uint32_t SLOW_STREAK = 2;
/** A benched follower's trial band must come in under this multiple to rejoin. */
constexpr uint32_t FIT_RATIO_X10 = 15;
/** Trial bands for a benched follower are spaced this far apart, doubling (up to
 *  the max) each time it is benched again soon after rejoining. */
constexpr uint32_t PROBE_INTERVAL_MIN_MS = 1000;
constexpr uint32_t PROBE_INTERVAL_MAX_MS = 8000;
/** Rejoined for at least this long counts as settled: the interval resets. */
constexpr uint32_t SETTLED_MS = 10000;

enum : uint32_t {
    MSG_BAND = 1,
    MSG_STOP = 2,
    MSG_DONE = 3,
};

inline uint32_t ticksToUs(OSTime t) {
    return t > 0 ? (uint32_t) OSTicksToMicroseconds(t) : 0;
}

inline uint32_t ticksToMs(OSTime t) {
    return t > 0 ? (uint32_t) OSTicksToMilliseconds(t) : 0;
}

/** Nanoseconds per row, the unit cores are compared in. */
inline uint32_t nsPerRow(uint32_t us, uint32_t rows) {
    if (rows == 0) {
        return 0;
    }
    if (us > 4000000) {
        us = 4000000; // keep the product inside 32 bits; anything this slow is benched anyway
    }
    return us * 1000u / rows;
}

// =============================================================================
// Shared: target size
// =============================================================================

/** Longest edge allowed for the configured capture size, 0 meaning "no limit". */
void targetLimits(uint32_t &maxW, uint32_t &maxH) {
    switch (gCaptureSize) {
        case WUPS_STREAMING_SIZE_720P: maxW = 1280; maxH = 720; break;
        case WUPS_STREAMING_SIZE_480P: maxW = 854;  maxH = 480; break;
        case WUPS_STREAMING_SIZE_360P: maxW = 640;  maxH = 360; break;
        case WUPS_STREAMING_SIZE_240P: maxW = 426;  maxH = 240; break;
        case WUPS_STREAMING_SIZE_180P: maxW = 320;  maxH = 180; break;
        case WUPS_STREAMING_SIZE_NATIVE:
        default:                       maxW = 0;    maxH = 0;   break;
    }
}

/**
 * Downscale never upscales: sending more pixels than the console rendered costs
 * encode time and bandwidth and adds no detail. So a game that renders 854x480
 * stays 854x480 even on the 720p setting, and the client scales it for display.
 */
void computeTargetSize(uint32_t srcW, uint32_t srcH, uint32_t &dstW, uint32_t &dstH) {
    uint32_t maxW, maxH;
    targetLimits(maxW, maxH);

    if (maxW == 0 || (srcW <= maxW && srcH <= maxH)) {
        dstW = srcW;
        dstH = srcH;
        return;
    }

    const double scale = (double) maxW / srcW < (double) maxH / srcH
                                 ? (double) maxW / srcW
                                 : (double) maxH / srcH;

    dstW = (uint32_t) (srcW * scale + 0.5);
    dstH = (uint32_t) (srcH * scale + 0.5);

    // Even dimensions keep the 4:2:0 chroma planes exact.
    dstW &= ~1u;
    dstH &= ~1u;

    if (dstW < 16) dstW = 16;
    if (dstH < 16) dstH = 16;

    // The floor above is the one place this function could hand back a target
    // larger than the source (a very short scan buffer at the 240p setting), which
    // would turn the downscaler into an upscaler. Nothing good comes of that here -
    // the PC does the upscaling - so clamp back.
    if (dstW > srcW) dstW = srcW;
    if (dstH > srcH) dstH = srcH;
}

#ifdef DEBUG
/**
 * A fixed amount of pure-register integer work, timed once per report window.
 *
 * The absolute figure is meaningless - what matters is the RATIO between a quiet
 * scene and a busy one. Workers run at priority 25, so Cafe OS (strict priority,
 * no round-robin among equals) only schedules them when nothing more important
 * on the core is runnable, and every timing we take is wall clock. Timing known
 * work is the one way to see that dilation directly.
 */
volatile uint32_t sCalibrationSink = 0;

uint32_t measureDilationUs() {
    constexpr uint32_t ITERATIONS = 200000;
    const OSTime start = OSGetSystemTime();
    uint32_t x         = 1;
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        x = x * 3u + 1u;
    }
    sCalibrationSink = x; // keep the loop from being optimised away
    return ticksToUs(OSGetSystemTime() - start);
}
#endif

/** Splits encode cost into its parts for the diagnostics. */
struct FrameTiming {
    uint32_t convUs; // downscale + colour conversion (the leader's band, on FAST)
    uint32_t jpegUs; // JPEG compression (the leader's band, on FAST)
    uint32_t spliceUs;
    uint32_t bands;
    uint32_t frameTag; // FAST only: the tag the frame's bands were issued under
};

// =============================================================================
// SAFE path: single core, RGB, libjpeg does the colour conversion. This is the
// code from the last build the hardware measurements were taken on, unchanged
// apart from now handing its frame to the sender thread.
// =============================================================================

tjhandle sTjLegacy = nullptr;

uint8_t *sSafeJpeg         = nullptr;
unsigned long sSafeJpegCap = 0;

uint8_t *sScratchRGB      = nullptr;
uint32_t sScratchCapacity = 0;

// Per-column source bounds for the downscale, rebuilt only when the width changes.
uint32_t *sColStart   = nullptr;
uint32_t *sColEnd     = nullptr;
uint32_t sColCapacity = 0;

uint8_t sSrgbLut[256];

/**
 * (2^22)/n, so averaging a box becomes a multiply and a shift instead of a divide.
 *
 * Measured on hardware, the old resampler cost 166 cycles per output pixel and was
 * 47% of the whole frame time - more than the JPEG encode it feeds. Three integer
 * divides per pixel accounted for most of it: the PPC750's divider is ~19 cycles
 * and unpipelined, so it stalls everything behind it.
 *
 * 22 fractional bits, not 16: at 16 the reciprocal of a large box is quantised
 * badly enough to shift a channel by up to 14/255, which is a visible tint. 22
 * keeps the worst case within 1/255 of a true divide while the largest possible
 * product still sits comfortably inside 32 bits (~1.07e9 against a 4.29e9 limit).
 */
constexpr uint32_t RECIP_SHIFT      = 22;
constexpr uint32_t RECIP_TABLE_SIZE = 1024;
uint32_t sRecip[RECIP_TABLE_SIZE];

void buildSafeTables() {
    for (int i = 0; i < 256; i++) {
        const float v = (float) i / 255.0f;
        const float s = (v <= 0.0031308f) ? (v * 12.92f)
                                          : (1.055f * powf(v, 1.0f / 2.4f) - 0.055f);
        int out = (int) (s * 255.0f + 0.5f);
        if (out < 0) out = 0;
        if (out > 255) out = 255;
        sSrgbLut[i] = (uint8_t) out;
    }
    sRecip[0] = 0;
    for (uint32_t i = 1; i < RECIP_TABLE_SIZE; i++) {
        // Round up, so that a full-white box still averages to 255 rather than 254.
        sRecip[i] = ((1u << RECIP_SHIFT) + i - 1) / i;
    }
}

inline uint8_t averageChannel(uint32_t sum, uint32_t recip) {
    const uint32_t v = (sum * recip) >> RECIP_SHIFT;
    return (uint8_t) (v > 255 ? 255 : v);
}

bool ensureColumnTable(uint32_t dstW) {
    if (sColCapacity >= dstW && sColStart != nullptr && sColEnd != nullptr) {
        return true;
    }
    free(sColStart);
    free(sColEnd);
    sColStart = (uint32_t *) malloc(dstW * sizeof(uint32_t));
    sColEnd   = (uint32_t *) malloc(dstW * sizeof(uint32_t));
    if (sColStart == nullptr || sColEnd == nullptr) {
        free(sColStart);
        free(sColEnd);
        sColStart    = nullptr;
        sColEnd      = nullptr;
        sColCapacity = 0;
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate the resample column table");
        return false;
    }
    sColCapacity = dstW;
    return true;
}

bool ensureScratch(uint32_t bytes) {
    if (sScratchCapacity >= bytes) {
        return true;
    }
    free(sScratchRGB);
    sScratchRGB = (uint8_t *) malloc(bytes);
    if (sScratchRGB == nullptr) {
        sScratchCapacity = 0;
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate %u bytes of scratch", bytes);
        return false;
    }
    sScratchCapacity = bytes;
    return true;
}

bool ensureSafeJpeg(uint32_t w, uint32_t h) {
    const unsigned long needed = tjBufSize((int) w, (int) h, JPEG_SUBSAMPLING);
    if (sSafeJpegCap >= needed && sSafeJpeg != nullptr) {
        return true;
    }
    tjFree(sSafeJpeg);
    sSafeJpeg = (uint8_t *) tjAlloc((int) needed);
    if (sSafeJpeg == nullptr) {
        sSafeJpegCap = 0;
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate %lu bytes for the JPEG buffer", needed);
        return false;
    }
    sSafeJpegCap = needed;
    return true;
}

/**
 * Area-average downscale, optionally applying sRGB encoding, writing tightly
 * packed RGB. One pass over the source does both jobs, and dropping the alpha
 * channel means the JPEG encoder then reads 25% less memory.
 *
 * The surface is UNORM_R8_G8_B8_A8 and the console is big-endian, so a 32-bit
 * load yields 0xRRGGBBAA.
 */
bool resampleToRGB(const uint32_t *src, uint32_t srcPitchPx, uint32_t srcW, uint32_t srcH,
                   uint8_t *dst, uint32_t dstW, uint32_t dstH, bool applySrgb) {
    // 1:1 - no averaging, no division at all.
    if (dstW == srcW && dstH == srcH) {
        for (uint32_t y = 0; y < dstH; y++) {
            const uint32_t *row = src + (size_t) y * srcPitchPx;
            if (applySrgb) {
                for (uint32_t x = 0; x < dstW; x++) {
                    const uint32_t p = row[x];
                    *dst++ = sSrgbLut[(p >> 24) & 0xFF];
                    *dst++ = sSrgbLut[(p >> 16) & 0xFF];
                    *dst++ = sSrgbLut[(p >> 8) & 0xFF];
                }
            } else {
                for (uint32_t x = 0; x < dstW; x++) {
                    const uint32_t p = row[x];
                    *dst++ = (uint8_t) (p >> 24);
                    *dst++ = (uint8_t) (p >> 16);
                    *dst++ = (uint8_t) (p >> 8);
                }
            }
        }
        return true;
    }

    // Exact integer ratio - 1280x720 -> 640x360 is a clean 2:1 and every box is
    // then the same size, so the divisor is constant for the whole image.
    if (srcW % dstW == 0 && srcH % dstH == 0) {
        const uint32_t bx = srcW / dstW;
        const uint32_t by = srcH / dstH;
        const uint32_t n  = bx * by;

        if (n < RECIP_TABLE_SIZE) {
            const uint32_t recip = sRecip[n];
            for (uint32_t dy = 0; dy < dstH; dy++) {
                const uint32_t *bandBase = src + (size_t) (dy * by) * srcPitchPx;
                for (uint32_t dx = 0; dx < dstW; dx++) {
                    const uint32_t *rp = bandBase + dx * bx;
                    uint32_t r = 0, g = 0, b = 0;
                    for (uint32_t sy = 0; sy < by; sy++, rp += srcPitchPx) {
                        for (uint32_t sx = 0; sx < bx; sx++) {
                            const uint32_t p = rp[sx];
                            r += (p >> 24) & 0xFF;
                            g += (p >> 16) & 0xFF;
                            b += (p >> 8) & 0xFF;
                        }
                    }
                    uint8_t rr = averageChannel(r, recip);
                    uint8_t gg = averageChannel(g, recip);
                    uint8_t bb = averageChannel(b, recip);
                    if (applySrgb) {
                        rr = sSrgbLut[rr];
                        gg = sSrgbLut[gg];
                        bb = sSrgbLut[bb];
                    }
                    *dst++ = rr;
                    *dst++ = gg;
                    *dst++ = bb;
                }
            }
            return true;
        }
    }

    // General ratio. The column bounds depend only on dx, so compute them once for
    // the whole image instead of re-dividing on every row.
    if (!ensureColumnTable(dstW)) {
        return false;
    }
    for (uint32_t dx = 0; dx < dstW; dx++) {
        uint32_t sx0 = (uint32_t) ((uint64_t) dx * srcW / dstW);
        uint32_t sx1 = (uint32_t) ((uint64_t) (dx + 1) * srcW / dstW);
        if (sx1 <= sx0) sx1 = sx0 + 1;
        if (sx1 > srcW) sx1 = srcW;
        sColStart[dx] = sx0;
        sColEnd[dx]   = sx1;
    }

    for (uint32_t dy = 0; dy < dstH; dy++) {
        uint32_t sy0 = (uint32_t) ((uint64_t) dy * srcH / dstH);
        uint32_t sy1 = (uint32_t) ((uint64_t) (dy + 1) * srcH / dstH);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > srcH) sy1 = srcH;

        const uint32_t boxH      = sy1 - sy0;
        const uint32_t *bandBase = src + (size_t) sy0 * srcPitchPx;

        for (uint32_t dx = 0; dx < dstW; dx++) {
            const uint32_t sx0  = sColStart[dx];
            const uint32_t boxW = sColEnd[dx] - sx0;
            const uint32_t n    = boxW * boxH;

            uint32_t r = 0, g = 0, b = 0;
            const uint32_t *rp = bandBase + sx0;
            for (uint32_t sy = 0; sy < boxH; sy++, rp += srcPitchPx) {
                for (uint32_t sx = 0; sx < boxW; sx++) {
                    const uint32_t p = rp[sx];
                    r += (p >> 24) & 0xFF;
                    g += (p >> 16) & 0xFF;
                    b += (p >> 8) & 0xFF;
                }
            }

            uint8_t rr, gg, bb;
            if (n < RECIP_TABLE_SIZE) {
                const uint32_t recip = sRecip[n];
                rr = averageChannel(r, recip);
                gg = averageChannel(g, recip);
                bb = averageChannel(b, recip);
            } else {
                // Only reachable for absurd downscale ratios; correctness over speed.
                rr = (uint8_t) (r / n);
                gg = (uint8_t) (g / n);
                bb = (uint8_t) (b / n);
            }

            if (applySrgb) {
                rr = sSrgbLut[rr];
                gg = sSrgbLut[gg];
                bb = sSrgbLut[bb];
            }

            *dst++ = rr;
            *dst++ = gg;
            *dst++ = bb;
        }
    }
    return true;
}

/** The whole frame on the leader's core. */
bool encodeSafe(const GX2Surface &surface, uint32_t dstW, uint32_t dstH, bool applySrgb, int quality,
                const uint8_t *&payload, uint32_t &payloadSize, FrameTiming &timing) {
    const OSTime start = OSGetSystemTime();

    // Invalidate on the core that is about to read: dcbi only affects the cache
    // of the core executing it.
    DCInvalidateRange(surface.image, surface.imageSize);

    if (!ensureSafeJpeg(dstW, dstH)) {
        return false;
    }

    const bool needsPass = (dstW != surface.width) || (dstH != surface.height) || applySrgb;
    const uint8_t *encodeSrc;
    int pixelFormat;
    int pitch;
    if (needsPass) {
        if (!ensureScratch(dstW * dstH * 3) ||
            !resampleToRGB((const uint32_t *) surface.image, surface.pitch, surface.width, surface.height,
                           sScratchRGB, dstW, dstH, applySrgb)) {
            return false; // scratch is only partly written - encoding it would send garbage
        }
        encodeSrc   = sScratchRGB;
        pixelFormat = TJPF_RGB;
        pitch       = (int) (dstW * 3);
    } else {
        // Nothing to correct and nothing to resize: hand the captured surface
        // straight to the encoder.
        encodeSrc   = (const uint8_t *) surface.image;
        pixelFormat = TJPF_RGBA;
        pitch       = (int) (surface.pitch * 4);
    }

    const OSTime compressStart = OSGetSystemTime();
    unsigned long jpegSize     = sSafeJpegCap;
    unsigned char *jpegBuf     = sSafeJpeg;
    if (tjCompress2(sTjLegacy, encodeSrc, (int) dstW, pitch, (int) dstH, pixelFormat, &jpegBuf, &jpegSize,
                    JPEG_SUBSAMPLING, quality, TJFLAG_NOREALLOC | TJFLAG_FASTDCT) != 0) {
        DEBUG_FUNCTION_LINE_ERR("tjCompress2 failed: %s", tjGetErrorStr());
        return false;
    }
    const OSTime done = OSGetSystemTime();

    timing.convUs = ticksToUs(compressStart - start);
    timing.jpegUs = ticksToUs(done - compressStart);
    payload       = sSafeJpeg;
    payloadSize   = (uint32_t) jpegSize;
    return true;
}

// =============================================================================
// FAST path
// =============================================================================

/** Everything a worker needs to know about the frame it is helping with. */
struct FrameJob {
    YuvConvert::Source src;
    YuvConvert::Target dst;
    int quality;
    uint32_t bandH;    // height of every band but the last (a multiple of 16)
    uint32_t numBands;
};

// Jobs live in a small ring, each stamped with the tag of the work it belongs
// to, and a follower copies its job out when it picks it up. A follower whose
// core the game has taken can sit on a message for a long time - hundreds of
// milliseconds were measured on a starved core - and by the time it runs, the
// leader may be rewriting that same ring entry for a newer frame. The tag is
// cleared before such a rewrite and set after it, so a follower that reads the
// same tag before and after its copy knows the copy is whole and current; any
// other outcome means the work is stale, and it skips it.
constexpr uint32_t JOB_RING = 8;
FrameJob sJobs[JOB_RING];
volatile uint32_t sJobTags[JOB_RING];

constexpr int JOB_QUEUE_SIZE  = 4;
constexpr int DONE_QUEUE_SIZE = 16;

struct Worker {
    int index        = 0;
    int core         = 0;
    OSThread *thread = nullptr;
    void *stack      = nullptr;
    tjhandle tj      = nullptr;

    // The band this worker last produced. Written by the worker before it posts
    // its completion, read by the leader only after receiving that completion.
    uint8_t *jpeg       = nullptr;
    size_t jpegCap      = 0;
    size_t jpegSize     = 0;
    bool lastOk         = false;
    uint32_t lastRows   = 0;
    uint32_t lastConvUs = 0;
    uint32_t lastJpegUs = 0;
    uint32_t lastBandUs = 0;
    OSTime lastDoneAt   = 0;

    uint8_t *yuv    = nullptr;
    uint32_t yuvCap = 0;
    YuvConvert::Scratch scratch;

    // Followers only.
    OSMessageQueue jobQueue;
    OSMessage jobMessages[JOB_QUEUE_SIZE];

    // Leader-side bookkeeping, only ever touched by the leader thread.
    bool benched             = true;
    bool busy                = false;
    bool probing             = false; // the outstanding band is a trial, not part of a frame
    uint32_t outstandingTag  = 0;
    uint32_t completedTag    = 0;
    OSTime dispatchedAt      = 0;
    OSTime lastProbeAt       = 0;
    OSTime unbenchedAt       = 0;
    uint32_t probeIntervalMs = PROBE_INTERVAL_MIN_MS;
    uint32_t slowStreak      = 0;
    uint32_t lastLatencyUs   = 0; // dispatch to done, as the leader experiences it
    uint32_t lastProbeUs     = 0;
};

Worker sWorkers[MAX_WORKERS];
int sWorkerCount = 0;

OSMessageQueue sDoneQueue;
OSMessage sDoneMessages[DONE_QUEUE_SIZE];

// Leader only.
uint32_t sTagCounter     = 0;
uint32_t sLeaderNsPerRow = 0; // smoothed; what a trial band is measured against
uint8_t *sSpliceBuf      = nullptr;
uint32_t sSpliceCap      = 0;

uint32_t nextTag() {
    if (++sTagCounter == 0) {
        ++sTagCounter; // 0 marks a ring entry that is being rewritten
    }
    return sTagCounter;
}

void publishJob(uint32_t tag, const FrameJob &job) {
    const uint32_t i = tag % JOB_RING;
    sJobTags[i]      = 0;
    OSMemoryBarrier();
    sJobs[i] = job;
    OSMemoryBarrier();
    sJobTags[i] = tag;
    OSMemoryBarrier();
}

bool readJob(uint32_t tag, FrameJob &job) {
    const uint32_t i = tag % JOB_RING;
    if (sJobTags[i] != tag) {
        return false;
    }
    OSMemoryBarrier();
    job = sJobs[i];
    OSMemoryBarrier();
    return sJobTags[i] == tag;
}

bool ensureYuv(Worker &w, uint32_t bytes) {
    if (w.yuvCap >= bytes && w.yuv != nullptr) {
        return true;
    }
    free(w.yuv);
    w.yuv = (uint8_t *) memalign(0x40, bytes);
    if (w.yuv == nullptr) {
        w.yuvCap = 0;
        DEBUG_FUNCTION_LINE_ERR("Worker %d: failed to allocate %u bytes of YCbCr planes", w.index, bytes);
        return false;
    }
    w.yuvCap = bytes;
    return true;
}

bool ensureBandJpeg(Worker &w, size_t bytes) {
    if (w.jpegCap >= bytes && w.jpeg != nullptr) {
        return true;
    }
    tj3Free(w.jpeg);
    w.jpeg = (uint8_t *) tj3Alloc(bytes);
    if (w.jpeg == nullptr) {
        w.jpegCap = 0;
        DEBUG_FUNCTION_LINE_ERR("Worker %d: failed to allocate %u bytes for a JPEG band", w.index, (uint32_t) bytes);
        return false;
    }
    w.jpegCap = bytes;
    return true;
}

bool ensureSplice(uint32_t bytes) {
    if (sSpliceCap >= bytes && sSpliceBuf != nullptr) {
        return true;
    }
    const uint32_t cap = (bytes + 0xFFFF) & ~0xFFFFu;
    auto *grown        = (uint8_t *) realloc(sSpliceBuf, cap);
    if (grown == nullptr) {
        DEBUG_FUNCTION_LINE_ERR("Failed to grow the splice buffer to %u bytes", cap);
        return false;
    }
    sSpliceBuf = grown;
    sSpliceCap = cap;
    return true;
}

/**
 * Converts and compresses band `band` of `job` on the calling worker's core.
 */
bool doBand(Worker &w, const FrameJob &job, uint32_t band) {
    if (band >= job.numBands) {
        return false;
    }
    const uint32_t y0 = band * job.bandH;
    const uint32_t y1 = (band + 1 == job.numBands) ? job.dst.height : y0 + job.bandH;
    if (y0 >= y1 || y1 > job.dst.height) {
        return false;
    }
    const uint32_t rows = y1 - y0;
    w.lastRows          = rows;
    const OSTime t0     = OSGetSystemTime();

    // Invalidate exactly the source rows this band reads, on the core about to
    // read them - dcbi works on the executing core's cache.
    uint32_t sy0, sy1;
    YuvConvert::SourceRows(job.src, job.dst, y0, y1, sy0, sy1);
    DCInvalidateRange((void *) (job.src.pixels + (size_t) sy0 * job.src.pitchPx),
                      (sy1 - sy0) * job.src.pitchPx * 4);

    if (!ensureYuv(w, YuvConvert::BandBytes(job.dst.width, rows))) {
        return false;
    }
    const YuvConvert::BandPlanes planes = YuvConvert::LayoutBand(w.yuv, job.dst.width, rows);
    if (!YuvConvert::ConvertBand(job.src, job.dst, y0, y1, planes, w.scratch)) {
        return false;
    }
    const OSTime t1 = OSGetSystemTime();

    const size_t need = tj3JPEGBufSize((int) job.dst.width, (int) rows, JPEG_SUBSAMPLING);
    if (need == 0 || !ensureBandJpeg(w, need)) {
        return false;
    }
    tj3Set(w.tj, TJPARAM_QUALITY, job.quality);

    const unsigned char *srcPlanes[3] = {planes.y, planes.cb, planes.cr};
    const int strides[3]              = {(int) planes.strideY, (int) planes.strideC, (int) planes.strideC};
    unsigned char *out                = w.jpeg;
    size_t outSize                    = w.jpegCap;
    if (tj3CompressFromYUVPlanes8(w.tj, srcPlanes, (int) job.dst.width, strides, (int) rows, &out, &outSize) != 0) {
        DEBUG_FUNCTION_LINE_ERR("Worker %d: tj3CompressFromYUVPlanes8 failed: %s", w.index, tj3GetErrorStr(w.tj));
        return false;
    }
    if (out != w.jpeg) {
        // TJPARAM_NOREALLOC rules this out, but if the library ever did hand back
        // a different buffer it now owns that one - adopt it rather than leak it.
        w.jpeg    = out;
        w.jpegCap = outSize;
    }
    w.jpegSize = outSize;

    const OSTime t2 = OSGetSystemTime();
    w.lastConvUs    = ticksToUs(t1 - t0);
    w.lastJpegUs    = ticksToUs(t2 - t1);
    w.lastBandUs    = ticksToUs(t2 - t0);
    return true;
}

int followerEntry(int argc, const char ** /*argv*/) {
    Worker &w = sWorkers[argc];
    for (;;) {
        OSMessage msg;
        OSReceiveMessage(&w.jobQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
        if ((uint32_t) (uintptr_t) msg.message == MSG_STOP) {
            break;
        }

        const uint32_t tag = msg.args[0];
        w.lastOk           = false;
        w.lastRows         = 0;
        FrameJob job;
        if (readJob(tag, job)) {
            w.lastOk = doBand(w, job, msg.args[1]);
        }
        w.lastDoneAt = OSGetSystemTime();
        OSMemoryBarrier();

        OSMessage done;
        memset(&done, 0, sizeof(done));
        done.message = (void *) (uintptr_t) MSG_DONE;
        done.args[0] = (uint32_t) w.index;
        done.args[1] = tag;
        OSSendMessage(&sDoneQueue, &done, OS_MESSAGE_FLAGS_BLOCKING);
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Leader-side follower management
// -----------------------------------------------------------------------------

void bench(Worker &w, const char *why, OSTime now) {
    if (!w.benched) {
        // Benched again soon after earning its place back: the core is busy on
        // and off, so wait longer before the next trial.
        if (w.unbenchedAt != 0 && ticksToMs(now - w.unbenchedAt) < SETTLED_MS) {
            w.probeIntervalMs = w.probeIntervalMs * 2 > PROBE_INTERVAL_MAX_MS ? PROBE_INTERVAL_MAX_MS
                                                                              : w.probeIntervalMs * 2;
        } else {
            w.probeIntervalMs = PROBE_INTERVAL_MIN_MS;
        }
        DEBUG_FUNCTION_LINE("Benching core %d: %s (next trial in %u ms)", w.core, why, w.probeIntervalMs);
    }
    w.benched     = true;
    w.slowStreak  = 0;
    w.lastProbeAt = now;
}

/** Applies one completion message to the bookkeeping of the worker it came from. */
void onDone(const OSMessage &msg) {
    const uint32_t idx = msg.args[0];
    if (idx == 0 || idx >= (uint32_t) sWorkerCount) {
        return;
    }
    Worker &w          = sWorkers[idx];
    const uint32_t tag = msg.args[1];
    if (!w.busy || tag != w.outstandingTag) {
        return; // stale - already accounted for
    }
    w.busy          = false;
    w.completedTag  = tag;
    w.lastLatencyUs = ticksToUs(w.lastDoneAt - w.dispatchedAt);

    if (w.probing) {
        w.probing         = false;
        w.lastProbeUs     = w.lastLatencyUs;
        const uint32_t ns = nsPerRow(w.lastLatencyUs, w.lastRows);
        if (w.benched && w.lastOk && ns > 0 && sLeaderNsPerRow > 0 &&
            (uint64_t) ns * 10 <= (uint64_t) sLeaderNsPerRow * FIT_RATIO_X10) {
            w.benched     = false;
            w.slowStreak  = 0;
            w.unbenchedAt = OSGetSystemTime();
            DEBUG_FUNCTION_LINE("Core %d rejoins: trial band at %u ns/row vs leader %u ns/row",
                                w.core, ns, sLeaderNsPerRow);
        }
    }
}

void drainDone() {
    OSMessage msg;
    while (OSReceiveMessage(&sDoneQueue, &msg, OS_MESSAGE_FLAGS_NONE)) {
        onDone(msg);
    }
}

bool dispatch(Worker &w, uint32_t tag, uint32_t band, OSTime now, bool probe) {
    OSMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.message = (void *) (uintptr_t) MSG_BAND;
    msg.args[0] = tag;
    msg.args[1] = band;
    if (!OSSendMessage(&w.jobQueue, &msg, OS_MESSAGE_FLAGS_NONE)) {
        return false;
    }
    w.outstandingTag = tag;
    w.busy           = true;
    w.probing        = probe;
    w.dispatchedAt   = now;
    return true;
}

/**
 * Bands of equal height (a multiple of 16, so each is a whole number of JPEG
 * MCU rows) for every band but the last, which takes the remainder. Collapses
 * the count when the image is too short to give everyone a band.
 */
uint32_t layoutBands(uint32_t height, uint32_t &count) {
    if (count <= 1 || height < 32) {
        count = 1;
        return height;
    }
    uint32_t bh = (((height + count - 1) / count) + 15) & ~15u;
    while (count > 1 && (count - 1) * bh >= height) {
        count--;
        bh = (((height + count - 1) / count) + 15) & ~15u;
    }
    return (count == 1) ? height : bh;
}

/**
 * Encodes one frame across the leader and every follower in play. Always
 * releases the slot, as early as it safely can.
 */
bool encodeFast(CaptureSlot *slot, const YuvConvert::Source &src, const YuvConvert::Target &dst, int quality,
                const uint8_t *&payload, uint32_t &payloadSize, FrameTiming &timing) {
    Worker &leader   = sWorkers[0];
    const OSTime now = OSGetSystemTime();

    Worker *helpers[MAX_WORKERS];
    uint32_t helperCount = 0;
    for (int i = 1; i < sWorkerCount; i++) {
        if (!sWorkers[i].benched && !sWorkers[i].busy) {
            helpers[helperCount++] = &sWorkers[i];
        }
    }

    uint32_t numBands    = helperCount + 1;
    const uint32_t bandH = layoutBands(dst.height, numBands);
    helperCount          = numBands - 1;

    const uint32_t tag = nextTag();
    const FrameJob job = {src, dst, quality, bandH, numBands};
    publishJob(tag, job);
    timing.frameTag = tag;

    // Followers take the full-height bands; the leader takes the last one, the
    // remainder, because it also has the splice to do.
    for (uint32_t i = 0; i < helperCount; i++) {
        if (!dispatch(*helpers[i], tag, i, now, false)) {
            // A follower that is not busy has an empty queue, so this cannot
            // happen - but if it did, that band would never be encoded. Bands
            // already handed out finish on their own and are simply not used.
            ScreenCapture::ReleaseFrame(slot);
            return false;
        }
    }

    // A benched follower that is due a trial gets a real band of this frame to
    // encode on the side. Nothing waits for it: its only output is how long it
    // took, which is the honest test of whether its core has room again.
    uint32_t probeTag = 0;
    for (int i = 1; i < sWorkerCount; i++) {
        Worker &w = sWorkers[i];
        if (!w.benched || w.busy) {
            continue;
        }
        if (w.lastProbeAt != 0 && ticksToMs(now - w.lastProbeAt) < w.probeIntervalMs) {
            continue; // not due yet (a follower that has never had a trial is due at once)
        }
        if (probeTag == 0) {
            uint32_t trialBands   = 2;
            const uint32_t trialH = layoutBands(dst.height, trialBands);
            if (trialBands < 2) {
                break; // image too short to split - nothing to gain from helpers anyway
            }
            probeTag = nextTag();
            publishJob(probeTag, FrameJob{src, dst, quality, trialH, trialBands});
        }
        if (dispatch(w, probeTag, 0, now, true)) {
            w.lastProbeAt = now;
        }
    }

    const bool leaderOk     = doBand(leader, job, numBands - 1);
    const OSTime leaderDone = OSGetSystemTime();
    const uint32_t leaderNs = nsPerRow(leader.lastBandUs, leader.lastRows);
    if (leaderOk && leaderNs > 0) {
        sLeaderNsPerRow = sLeaderNsPerRow == 0 ? leaderNs
                                               : (uint32_t) (((uint64_t) sLeaderNsPerRow * 3 + leaderNs) / 4);
    }

    uint32_t waitUs = leader.lastBandUs * 2;
    if (waitUs < FOLLOWER_WAIT_MIN_US) waitUs = FOLLOWER_WAIT_MIN_US;
    const OSTime deadline = leaderDone + (OSTime) OSMicrosecondsToTicks(waitUs);

    bool allDone;
    for (;;) {
        drainDone();
        allDone = true;
        for (uint32_t i = 0; i < helperCount; i++) {
            if (helpers[i]->busy || helpers[i]->completedTag != tag) {
                allDone = false;
                break;
            }
        }
        if (allDone || OSGetSystemTime() >= deadline) {
            break;
        }
        OSSleepTicks(OSMicrosecondsToTicks(100));
    }

    // Every band that is going to be used has been read out of the capture. A
    // follower that missed the deadline (or a trial band) may still be reading
    // it, but only to produce output nobody will use - and the memory itself
    // stays valid until shutdown, which joins every worker first.
    ScreenCapture::ReleaseFrame(slot);

    if (!allDone) {
        const OSTime t = OSGetSystemTime();
        for (uint32_t i = 0; i < helperCount; i++) {
            Worker &w = *helpers[i];
            if (w.busy || w.completedTag != tag) {
                bench(w, "missed the frame deadline", t);
            }
        }
        return false;
    }

    // A follower that keeps costing much more per row than the leader is making
    // frames slower, not faster. Latency (dispatch to done) is the measure, since
    // time spent waiting to be scheduled delays the frame just the same.
    bool bandsOk = leaderOk;
    for (uint32_t i = 0; i < helperCount; i++) {
        Worker &w         = *helpers[i];
        bandsOk           = bandsOk && w.lastOk;
        const uint32_t ns = nsPerRow(w.lastLatencyUs, w.lastRows);
        if (leaderNs > 0 && w.lastLatencyUs > 4000 && (uint64_t) ns * 10 > (uint64_t) leaderNs * SLOW_RATIO_X10) {
            if (++w.slowStreak >= SLOW_STREAK) {
                bench(w, "consistently slower than the leader", OSGetSystemTime());
            }
        } else {
            w.slowStreak = 0;
        }
    }
    if (!bandsOk) {
        return false;
    }

    timing.convUs = leader.lastConvUs;
    timing.jpegUs = leader.lastJpegUs;
    timing.bands  = numBands;

    if (numBands == 1) {
        timing.spliceUs = 0;
        payload         = leader.jpeg;
        payloadSize     = (uint32_t) leader.jpegSize;
        return true;
    }

    const OSTime spliceStart = OSGetSystemTime();
    const uint8_t *bands[JpegStitch::MAX_BANDS];
    uint32_t sizes[JpegStitch::MAX_BANDS];
    for (uint32_t i = 0; i < helperCount; i++) {
        bands[i] = helpers[i]->jpeg;
        sizes[i] = (uint32_t) helpers[i]->jpegSize;
    }
    bands[numBands - 1] = leader.jpeg;
    sizes[numBands - 1] = (uint32_t) leader.jpegSize;

    const uint32_t need = JpegStitch::SplicedSize(bands, sizes, numBands);
    uint32_t spliced    = 0;
    if (need == 0 || !ensureSplice(need) ||
        !JpegStitch::Splice(bands, sizes, numBands, dst.width, dst.height, bandH, sSpliceBuf, sSpliceCap, spliced)) {
        DEBUG_FUNCTION_LINE_ERR("Failed to splice %u bands", numBands);
        return false;
    }
    timing.spliceUs = ticksToUs(OSGetSystemTime() - spliceStart);
    payload         = sSpliceBuf;
    payloadSize     = spliced;
    return true;
}

// -----------------------------------------------------------------------------
// Reporting (debug builds only)
// -----------------------------------------------------------------------------

#ifdef DEBUG
struct ReportWindow {
    OSTime start;
    uint32_t frames;
    uint32_t dropped;
    uint32_t gpuTimeouts;
    uint64_t frameUs, gpuUs, convUs, jpegUs, spliceUs;
    uint64_t bands;
    uint64_t followerUs[MAX_WORKERS];
    uint32_t followerBands[MAX_WORKERS];
    uint32_t lastW, lastH;
    // Baselines for counters owned by other modules.
    uint32_t framesSent;
    uint64_t bytesSent;
    uint64_t wireSent;
    uint32_t submitDrops;
    uint32_t sendUs;
    uint32_t sendCount;
};

void resetWindow(ReportWindow &r) {
    memset(&r, 0, sizeof(r));
    r.start       = OSGetTime();
    r.framesSent  = StreamSender::GetFramesSent();
    r.bytesSent   = StreamSender::GetBytesSent();
    r.wireSent    = StreamSender::GetWireBytesSent();
    r.submitDrops = StreamSender::GetSubmitDrops();
    r.sendUs      = StreamSender::GetSendUsTotal();
    r.sendCount   = StreamSender::GetSendCount();
    ScreenCapture::ResetCounters();
}

#define MS2(us) (uint32_t) ((us) / 1000), (uint32_t) (((us) % 1000) / 10)

void report(ReportWindow &r) {
    // Millisecond window, not floored whole seconds: dividing by an integer 5
    // when the window ran 5.4 s inflates every rate, worst when frames are sparse.
    const uint32_t elapsedMs = ticksToMs(OSGetTime() - r.start);
    if (elapsedMs < 5000) {
        return;
    }

    // Snapshot every counter once so the lines below agree with each other.
    const uint32_t presented  = ScreenCapture::GetPresentedCount();
    const uint32_t captured   = ScreenCapture::GetCapturedCount();
    const uint32_t encBusy    = ScreenCapture::GetSkippedCount();
    const uint32_t framesSent = StreamSender::GetFramesSent() - r.framesSent;
    const uint64_t bytesSent  = StreamSender::GetBytesSent() - r.bytesSent;
    const uint64_t wireSent   = StreamSender::GetWireBytesSent() - r.wireSent;
    const uint32_t replaced   = StreamSender::GetSubmitDrops() - r.submitDrops;
    const uint32_t sendUs     = StreamSender::GetSendUsTotal() - r.sendUs;
    const uint32_t sendCount  = StreamSender::GetSendCount() - r.sendCount;

    auto perSec      = [elapsedMs](uint64_t count) { return (uint32_t) (count * 1000 / elapsedMs); };
    const uint32_t f = r.frames ? r.frames : 1;

    const uint64_t avgFrame  = r.frameUs / f;
    const uint64_t avgGpu    = r.gpuUs / f;
    const uint64_t avgConv   = r.convUs / f;
    const uint64_t avgJpeg   = r.jpegUs / f;
    const uint64_t avgSplice = r.spliceUs / f;
    const uint64_t avgSend   = sendCount ? sendUs / sendCount : 0;
    const uint32_t avgKB     = framesSent ? (uint32_t) (bytesSent / framesSent / 1024) : 0;
    // Application-layer throughput (payload + 44-byte header); true link use is
    // ~5-8% higher once UDP/IP/Ethernet framing is added.
    const uint32_t mbitx100 = (uint32_t) ((wireSent * 800ull) / (elapsedMs * 1000ull));
    const uint32_t bandsx10 = (uint32_t) (r.bands * 10 / f);

    // present is the ceiling (the game's own present rate). enc-busy is frames the
    // encoder had no free slot for: the CPU-bottleneck signal. send-replaced is
    // frames the sender had not got to before a newer one replaced them: the
    // network-bottleneck signal.
    DEBUG_FUNCTION_LINE("[fps] present %u | capture %u | encode %u | tx %u   "
                        "(enc-busy %u/s, dropped %u, gpu-timeouts %u, send-replaced %u, sendfail %u total)",
                        perSec(presented), perSec(captured), perSec(r.frames), perSec(framesSent),
                        perSec(encBusy), r.dropped, r.gpuTimeouts, replaced, StreamSender::GetSendFailures());
    DEBUG_FUNCTION_LINE("[cost] frame %u.%02u ms (gpu wait %u.%02u, lead band: convert %u.%02u + jpeg %u.%02u, "
                        "splice %u.%02u) | send %u.%02u ms | %u.%02u Mbit/s | %u KB | %ux%u",
                        MS2(avgFrame), MS2(avgGpu), MS2(avgConv), MS2(avgJpeg), MS2(avgSplice), MS2(avgSend),
                        mbitx100 / 100, mbitx100 % 100, avgKB, r.lastW, r.lastH);

    char cores[192];
    int n = snprintf(cores, sizeof(cores), "%s, %u.%u bands/frame |",
                     gEncodePath == WUPS_STREAMING_PATH_SAFE ? "SAFE" : "FAST", bandsx10 / 10, bandsx10 % 10);
    for (int i = 0; i < sWorkerCount && n > 0 && n < (int) sizeof(cores); i++) {
        const Worker &w = sWorkers[i];
        if (i == 0) {
            n += snprintf(cores + n, sizeof(cores) - n, " core%d leads", w.core);
        } else if (w.benched) {
            n += snprintf(cores + n, sizeof(cores) - n, " core%d benched (trial %u.%02u ms)", w.core,
                          MS2(w.lastProbeUs));
        } else if (r.followerBands[i] == 0) {
            n += snprintf(cores + n, sizeof(cores) - n, " core%d idle", w.core);
        } else {
            const uint64_t avg = r.followerUs[i] / r.followerBands[i];
            n += snprintf(cores + n, sizeof(cores) - n, " core%d %u.%02u ms/band", w.core, MS2(avg));
        }
    }
    // cpu-check: fixed work timed on the leader. If it doubles when the scene
    // gets busy, the leader's core is being shared with the game.
    DEBUG_FUNCTION_LINE("[cores] %s | cpu-check %u us%s", cores, measureDilationUs(),
                        UseBlockingGpuSync() ? " | GPU sync BLOCKING" : "");

    resetWindow(r);
}
#endif // DEBUG

// -----------------------------------------------------------------------------
// Leader thread
// -----------------------------------------------------------------------------

int leaderEntry(int /*argc*/, const char ** /*argv*/) {
    DEBUG_FUNCTION_LINE("Encoder running: %d core(s), leader on core %d", sWorkerCount, sWorkers[0].core);

    uint32_t gpuStreak = 0;
#ifdef DEBUG
    ReportWindow window;
    bool windowAnchored = false;
#endif

    // The stop sentinel from ScreenCapture::SignalStop() is the *only* exit.
    // Checking a flag here as well would let a thread that was mid-frame when
    // Stop() was called leave the sentinel in the queue, where the next encoder
    // would pop it and die immediately.
    for (;;) {
        CaptureSlot *slot = ScreenCapture::WaitForFrame();
        if (slot == nullptr) {
            break;
        }

#ifdef DEBUG
        if (!windowAnchored) {
            // Measure from the first frame, not from thread creation, which can
            // be minutes before a client connects.
            windowAnchored = true;
            resetWindow(window);
        }
#endif

        const OSTime frameStart = OSGetSystemTime();
        drainDone();

        if (!ScreenCapture::WaitForGpu(slot, GPU_WAIT_TIMEOUT_MS)) {
            ScreenCapture::ReleaseFrame(slot);
            if (StreamingActive()) {
#ifdef DEBUG
                window.gpuTimeouts++;
#endif
                if (++gpuStreak >= GPU_FALLBACK_STREAK && !gGpuSyncFallback) {
                    gGpuSyncFallback = true;
                    OSMemoryBarrier();
                    DEBUG_FUNCTION_LINE_WARN("GPU copies are not retiring in time; "
                                             "falling back to blocking GPU sync for this title");
                }
            }
            continue;
        }
        gpuStreak            = 0;
        const OSTime gpuDone = OSGetSystemTime();

        const GX2Surface &surface = slot->colorBuffer.surface;
        if (surface.format != GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8 || surface.image == nullptr) {
            DEBUG_FUNCTION_LINE_ERR("Unexpected capture surface (format 0x%08X)", surface.format);
            ScreenCapture::ReleaseFrame(slot);
            continue;
        }

        uint32_t dstW, dstH;
        computeTargetSize(surface.width, surface.height, dstW, dstH);
        const bool applySrgb = (gColorMode == WUPS_STREAMING_COLOR_AUTO) && slot->sourceIsSRGB;
        int quality          = gQuality;
        if (quality < STREAM_QUALITY_MIN) quality = STREAM_QUALITY_MIN;
        if (quality > STREAM_QUALITY_MAX) quality = STREAM_QUALITY_MAX;

        const uint8_t *payload = nullptr;
        uint32_t payloadSize   = 0;
        FrameTiming timing     = {0, 0, 0, 1, 0};
        bool ok;

        if (gEncodePath == WUPS_STREAMING_PATH_SAFE) {
            ok = encodeSafe(surface, dstW, dstH, applySrgb, quality, payload, payloadSize, timing);
            ScreenCapture::ReleaseFrame(slot);
        } else {
            const YuvConvert::Source src = {(const uint32_t *) surface.image, surface.pitch, surface.width, surface.height};
            const YuvConvert::Target dst = {dstW, dstH, applySrgb};
            ok = encodeFast(slot, src, dst, quality, payload, payloadSize, timing);
        }

        if (ok) {
            const StreamSender::FrameMeta meta = {
                    .width           = (uint16_t) dstW,
                    .height          = (uint16_t) dstH,
                    .stride          = 0, // JPEG carries its own dimensions
                    .compressionType = STREAM_COMP_JPEG,
                    .pixelFormat     = STREAM_PIXFMT_JPEG,
            };
            ok = StreamSender::Submit(payload, payloadSize, meta);
        }

#ifdef DEBUG
        if (ok) {
            window.frames++;
            window.frameUs += ticksToUs(OSGetSystemTime() - frameStart);
            window.gpuUs += ticksToUs(gpuDone - frameStart);
            window.convUs += timing.convUs;
            window.jpegUs += timing.jpegUs;
            window.spliceUs += timing.spliceUs;
            window.bands += timing.bands;
            window.lastW = dstW;
            window.lastH = dstH;
            if (timing.bands > 1) {
                for (int i = 1; i < sWorkerCount; i++) {
                    const Worker &w = sWorkers[i];
                    if (!w.busy && w.completedTag == timing.frameTag) {
                        window.followerUs[i] += w.lastLatencyUs;
                        window.followerBands[i]++;
                    }
                }
            }
        } else {
            window.dropped++;
        }
        report(window);
#else
        (void) frameStart;
        (void) gpuDone;
#endif
    }

    DEBUG_FUNCTION_LINE("Encoder stopping");
    return 0;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

/** Cores to use, leader first, for an "Encoder cores" setting. */
int coresForPreset(int32_t preset, int *cores) {
    switch (preset) {
        case WUPS_STREAMING_CORES_0:
            cores[0] = 0;
            return 1;
        case WUPS_STREAMING_CORES_2:
            cores[0] = 2;
            return 1;
        case WUPS_STREAMING_CORES_1:
            cores[0] = 1;
            return 1;
        case WUPS_STREAMING_CORES_ALL:
            // Core 1 runs most games' main thread, so it goes last: it is the one
            // most likely to spend its time benched.
            cores[0] = 0;
            cores[1] = 2;
            cores[2] = 1;
            return 3;
        case WUPS_STREAMING_CORES_0_2:
        default:
            cores[0] = 0;
            cores[1] = 2;
            return 2;
    }
}

void freeWorker(Worker &w) {
    free(w.stack);
    free(w.thread);
    if (w.tj != nullptr) {
        tj3Destroy(w.tj);
    }
    tj3Free(w.jpeg);
    free(w.yuv);
    YuvConvert::FreeScratch(w.scratch);
    w = Worker{};
}

bool createWorkerThread(Worker &w, OSThreadEntryPointFn entry, uint32_t stackSize, const char *name) {
    w.thread = (OSThread *) memalign(8, sizeof(OSThread));
    w.stack  = memalign(0x20, stackSize);
    if (w.thread == nullptr || w.stack == nullptr) {
        free(w.thread);
        free(w.stack);
        w.thread = nullptr;
        w.stack  = nullptr;
        return false;
    }
    memset(w.thread, 0, sizeof(OSThread));
    if (!OSCreateThread(w.thread, entry, w.index, nullptr, (char *) w.stack + stackSize, stackSize,
                        WORKER_PRIORITY, (OSThreadAttributes) (1 << w.core))) {
        free(w.thread);
        free(w.stack);
        w.thread = nullptr;
        w.stack  = nullptr;
        return false;
    }
    OSSetThreadName(w.thread, name);
    OSResumeThread(w.thread);
    return true;
}

} // namespace

bool Start() {
    if (sWorkerCount > 0) {
        return true;
    }

    buildSafeTables();
    YuvConvert::Init();
    OSInitMessageQueue(&sDoneQueue, sDoneMessages, DONE_QUEUE_SIZE);
    for (uint32_t i = 0; i < JOB_RING; i++) {
        sJobTags[i] = 0;
    }
    sTagCounter     = 0;
    sLeaderNsPerRow = 0;

    // Set up every compressor here rather than inside the threads, so a failure
    // leaves no half-started encoder behind.
    sTjLegacy = tjInitCompress();
    if (sTjLegacy == nullptr) {
        DEBUG_FUNCTION_LINE_ERR("tjInitCompress failed: %s", tjGetErrorStr());
        return false;
    }

    int cores[MAX_WORKERS];
    const int count = coresForPreset(gEncoderCores, cores);
    for (int i = 0; i < count; i++) {
        Worker &w = sWorkers[i];
        w         = Worker{};
        w.index   = i;
        w.core    = cores[i];
        // Followers start benched and earn their place with a trial band, so the
        // first frames never wait on a core the game turns out to be using.
        w.benched = (i != 0);
        w.tj      = tj3Init(TJINIT_COMPRESS);
        if (w.tj == nullptr) {
            DEBUG_FUNCTION_LINE_ERR("tj3Init failed for worker %d", i);
            for (int j = 0; j <= i; j++) {
                freeWorker(sWorkers[j]);
            }
            tjDestroy(sTjLegacy);
            sTjLegacy = nullptr;
            return false;
        }
        // Identical settings on every worker are what make the bands splice: same
        // quantisation tables, standard (non-optimised) Huffman tables, baseline,
        // and no restart markers of their own.
        tj3Set(w.tj, TJPARAM_SUBSAMP, JPEG_SUBSAMPLING);
        tj3Set(w.tj, TJPARAM_FASTDCT, 1);
        tj3Set(w.tj, TJPARAM_NOREALLOC, 1);
        tj3Set(w.tj, TJPARAM_OPTIMIZE, 0);
        tj3Set(w.tj, TJPARAM_PROGRESSIVE, 0);
        tj3Set(w.tj, TJPARAM_ARITHMETIC, 0);
        tj3Set(w.tj, TJPARAM_RESTARTBLOCKS, 0);
        tj3Set(w.tj, TJPARAM_RESTARTROWS, 0);
        if (i > 0) {
            OSInitMessageQueue(&w.jobQueue, w.jobMessages, JOB_QUEUE_SIZE);
        }
    }
    sWorkerCount = count;

    // Followers first, so they are waiting by the time the leader hands out work.
    for (int i = count - 1; i >= 1; i--) {
        if (!createWorkerThread(sWorkers[i], followerEntry, FOLLOWER_STACK_SIZE, "WiiStreamUStream follower")) {
            DEBUG_FUNCTION_LINE_ERR("Failed to start the follower on core %d", sWorkers[i].core);
            Stop();
            return false;
        }
    }
    if (!createWorkerThread(sWorkers[0], leaderEntry, LEADER_STACK_SIZE, "WiiStreamUStream encoder")) {
        DEBUG_FUNCTION_LINE_ERR("Failed to start the encoder on core %d", sWorkers[0].core);
        Stop();
        return false;
    }
    return true;
}

void Stop() {
    if (sWorkerCount == 0) {
        return;
    }

    // The leader first: the stop sentinel ends its loop. Only send it if the
    // leader actually exists - a sentinel nobody consumes would kill the *next*
    // encoder on its first wait.
    if (sWorkers[0].thread != nullptr) {
        ScreenCapture::SignalStop();
        int result = 0;
        OSJoinThread(sWorkers[0].thread, &result);
    }

    // Then the followers. One may still be finishing a band the leader gave up
    // on; it completes, posts to the done queue (which always has room) and then
    // reads the stop.
    for (int i = 1; i < sWorkerCount; i++) {
        Worker &w = sWorkers[i];
        if (w.thread == nullptr) {
            continue;
        }
        OSMessage msg;
        memset(&msg, 0, sizeof(msg));
        msg.message = (void *) (uintptr_t) MSG_STOP;
        OSSendMessage(&w.jobQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
        int result = 0;
        OSJoinThread(w.thread, &result);
    }

    // Only now is it safe to free what the threads were using.
    for (int i = 0; i < sWorkerCount; i++) {
        freeWorker(sWorkers[i]);
    }
    sWorkerCount = 0;

    OSMessage msg;
    while (OSReceiveMessage(&sDoneQueue, &msg, OS_MESSAGE_FLAGS_NONE)) {
    }

    tjDestroy(sTjLegacy);
    sTjLegacy = nullptr;
    tjFree(sSafeJpeg);
    sSafeJpeg    = nullptr;
    sSafeJpegCap = 0;
    free(sScratchRGB);
    sScratchRGB      = nullptr;
    sScratchCapacity = 0;
    free(sColStart);
    free(sColEnd);
    sColStart    = nullptr;
    sColEnd      = nullptr;
    sColCapacity = 0;
    free(sSpliceBuf);
    sSpliceBuf = nullptr;
    sSpliceCap = 0;

    DEBUG_FUNCTION_LINE("Encoder stopped");
}

bool IsRunning() {
    // Whether threads are *owned*, which is what Start() tests too, so the two
    // agree even while a thread is starting up or winding down.
    return sWorkerCount > 0;
}

} // namespace ImageEncoder
