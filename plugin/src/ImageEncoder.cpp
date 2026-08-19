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
#include "ScreenCapture.hpp"
#include "StreamProtocol.h"
#include "StreamSender.hpp"
#include "retain_vars.hpp"
#include "utils/logger.h"

#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

namespace ImageEncoder {
namespace {

constexpr uint32_t THREAD_STACK_SIZE = 0x40000;

/**
 * 4:2:0, not upstream's 4:1:1. Both store a quarter of the chroma, but 4:2:0 is
 * the format every JPEG decoder is tuned for and it subsamples vertically as
 * well as horizontally, which suits real game footage better.
 */
constexpr int JPEG_SUBSAMPLING = TJSAMP_420;

OSThread *sThread = nullptr;
void *sThreadStack = nullptr;

// Wall-clock spent in the CPU stages of a frame, summed since the last report
// tick and divided by the frame count for the on-console diagnostics. sEncodeUs
// is the whole path (cache invalidate + resample + JPEG); sCompressUs is just the
// tjCompress2 call, so the two together say how much of the cost is the JPEG DCT
// itself versus the surrounding memory work.
uint64_t sEncodeUs = 0;
uint64_t sCompressUs = 0;
uint64_t sSendUs = 0;
uint32_t sEncodeCount = 0;

/**
 * A fixed amount of pure-register integer work, timed on the encoder thread once
 * per report window.
 *
 * The absolute figure is meaningless - what matters is the RATIO between a quiet
 * scene and a busy one. This thread runs at priority 25, so Cafe OS (strict
 * priority, no round-robin among equals) only schedules it when nothing more
 * important on its core is runnable. Every measurement we take around
 * tjCompress2 or send() is wall clock, so it silently includes time the thread
 * spent descheduled. Timing known work is the one way to see that dilation
 * directly, and it needs no OS API we would have to guess at.
 */
volatile uint32_t sCalibrationSink = 0;

uint32_t measureDilationUs() {
    constexpr uint32_t ITERATIONS = 200000;
    const OSTime start = OSGetSystemTime();
    uint32_t x = 1;
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        x = x * 3u + 1u;
    }
    sCalibrationSink = x; // keep the loop from being optimised away
    return (uint32_t) OSTicksToMicroseconds(OSGetSystemTime() - start);
}

tjhandle sTjHandle = nullptr;

uint8_t *sJpegBuffer = nullptr;
unsigned long sJpegCapacity = 0;

uint8_t *sScratchRGB = nullptr;
uint32_t sScratchCapacity = 0;

// Per-column source bounds for the downscale, rebuilt only when the width changes.
uint32_t *sColStart = nullptr;
uint32_t *sColEnd = nullptr;
uint32_t sColCapacity = 0;

uint8_t sSrgbLut[256];

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
constexpr uint32_t RECIP_SHIFT = 22;
constexpr uint32_t RECIP_TABLE_SIZE = 1024;
uint32_t *sRecip = nullptr;

bool ensureRecipTable() {
    if (sRecip != nullptr) {
        return true;
    }
    sRecip = (uint32_t *) malloc(RECIP_TABLE_SIZE * sizeof(uint32_t));
    if (sRecip == nullptr) {
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate the reciprocal table");
        return false;
    }
    sRecip[0] = 0;
    for (uint32_t i = 1; i < RECIP_TABLE_SIZE; i++) {
        // Round up, so that a full-white box still averages to 255 rather than 254.
        sRecip[i] = ((1u << RECIP_SHIFT) + i - 1) / i;
    }
    return true;
}

inline uint8_t averageChannel(uint32_t sum, uint32_t recip) {
    const uint32_t v = (sum * recip) >> RECIP_SHIFT;
    return (uint8_t) (v > 255 ? 255 : v);
}

void buildSrgbLut() {
    for (int i = 0; i < 256; i++) {
        const float v = (float) i / 255.0f;
        const float s = (v <= 0.0031308f) ? (v * 12.92f)
                                          : (1.055f * powf(v, 1.0f / 2.4f) - 0.055f);
        int out = (int) (s * 255.0f + 0.5f);
        if (out < 0) out = 0;
        if (out > 255) out = 255;
        sSrgbLut[i] = (uint8_t) out;
    }
}

/** Longest edge allowed for the configured capture size, 0 meaning "no limit". */
void targetLimits(uint32_t &maxW, uint32_t &maxH) {
    switch (gCaptureSize) {
        case WUPS_STREAMING_SIZE_720P: maxW = 1280; maxH = 720; break;
        case WUPS_STREAMING_SIZE_480P: maxW = 854;  maxH = 480; break;
        case WUPS_STREAMING_SIZE_360P: maxW = 640;  maxH = 360; break;
        case WUPS_STREAMING_SIZE_240P: maxW = 426;  maxH = 240; break;
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

bool ensureJpegBuffer(uint32_t w, uint32_t h) {
    const unsigned long needed = tjBufSize((int) w, (int) h, JPEG_SUBSAMPLING);
    if (sJpegCapacity >= needed && sJpegBuffer != nullptr) {
        return true;
    }
    tjFree(sJpegBuffer);
    sJpegBuffer = (uint8_t *) tjAlloc((int) needed);
    if (sJpegBuffer == nullptr) {
        sJpegCapacity = 0;
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate %lu bytes for the JPEG buffer", needed);
        return false;
    }
    sJpegCapacity = needed;
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
    // 1:1 - no averaging, no division at all. This is the path a 720p game on
    // the default settings takes whenever colour correction is on, so it must
    // not go anywhere near the general resampler below: that one does five
    // integer divisions per output pixel, two of them 64-bit (and so libgcc
    // calls, since PPC32 has no 64-bit divide), to compute sx0 = dx, sx1 = dx+1
    // and n = 1.
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

    if (!ensureRecipTable()) {
        return false;
    }

    // Exact integer ratio - the case worth special-casing, because 1280x720 -> 640x360
    // is a clean 2:1 and every box is then the same size. The divisor is constant for
    // the whole image, so it collapses to one shift (or one multiply) known up front,
    // and the bounds arithmetic disappears entirely.
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

        const uint32_t boxH = sy1 - sy0;
        // Hoisted out of the pixel loop: the band's first row is fixed for this
        // output row, so the per-pixel row-address multiply goes away.
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

void encodeAndSend(CaptureSlot *slot) {
    const GX2Surface &surface = slot->colorBuffer.surface;

    const uint32_t srcW     = surface.width;
    const uint32_t srcH     = surface.height;
    const uint32_t srcPitch = surface.pitch; // in pixels

    if (surface.format != GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8 || surface.image == nullptr) {
        DEBUG_FUNCTION_LINE_ERR("Unexpected capture surface (format 0x%08X)", surface.format);
        return;
    }

    // Time the whole CPU cost of the frame (cache invalidate + resample + JPEG
    // compress) for the diagnostics. The network send is deliberately excluded.
    const OSTime encodeStart = OSGetSystemTime();

    // Invalidate on the core that is about to read. dcbi only affects the cache
    // of the core executing it, so doing this in the GX2 hook (which runs on the
    // game's render thread, on a different core) would not help this thread at
    // all - and would put a 115k-block cache walk on the game's critical path.
    DCInvalidateRange(surface.image, surface.imageSize);

    uint32_t dstW, dstH;
    computeTargetSize(srcW, srcH, dstW, dstH);

    const bool applySrgb = (gColorMode == WUPS_STREAMING_COLOR_AUTO) && slot->sourceIsSRGB;
    const bool needsPass = (dstW != srcW) || (dstH != srcH) || applySrgb;

    if (!ensureJpegBuffer(dstW, dstH)) {
        return;
    }

    const uint8_t *encodeSrc;
    int encodePixelFormat;
    int encodePitch;

    if (needsPass) {
        if (!ensureScratch(dstW * dstH * 3)) {
            return;
        }
        if (!resampleToRGB((const uint32_t *) surface.image, srcPitch, srcW, srcH,
                           sScratchRGB, dstW, dstH, applySrgb)) {
            return; // scratch is only partly written - encoding it would send garbage
        }
        encodeSrc         = sScratchRGB;
        encodePixelFormat = TJPF_RGB;
        encodePitch       = (int) (dstW * 3);
    } else {
        // Nothing to correct and nothing to resize: hand the captured surface
        // straight to the encoder. This is the 720p-native path and it is the
        // reason 720p is affordable at all.
        encodeSrc         = (const uint8_t *) surface.image;
        encodePixelFormat = TJPF_RGBA;
        encodePitch       = (int) (srcPitch * 4);
    }

    int quality = gQuality;
    if (quality < STREAM_QUALITY_MIN) quality = STREAM_QUALITY_MIN;
    if (quality > STREAM_QUALITY_MAX) quality = STREAM_QUALITY_MAX;

    unsigned long jpegSize = sJpegCapacity;
    unsigned char *jpegBuf = sJpegBuffer;

    // TJFLAG_NOREALLOC keeps turbojpeg from allocating per frame; TJFLAG_FASTDCT
    // is worth a lot here because the Espresso has no AltiVec, so libjpeg-turbo
    // runs its plain-C path and the DCT dominates.
    const OSTime compressStart = OSGetSystemTime();
    const int rc = tjCompress2(sTjHandle, encodeSrc, (int) dstW, encodePitch, (int) dstH,
                               encodePixelFormat, &jpegBuf, &jpegSize,
                               JPEG_SUBSAMPLING, quality,
                               TJFLAG_NOREALLOC | TJFLAG_FASTDCT);
    if (rc != 0) {
        DEBUG_FUNCTION_LINE_ERR("tjCompress2 failed: %s", tjGetErrorStr());
        return;
    }

    const OSTime doneTime = OSGetSystemTime();
    sCompressUs += OSTicksToMicroseconds(doneTime - compressStart);
    sEncodeUs += OSTicksToMicroseconds(doneTime - encodeStart);
    sEncodeCount++;

    const StreamSender::FrameMeta meta = {
            .width           = (uint16_t) dstW,
            .height          = (uint16_t) dstH,
            .stride          = 0, // JPEG carries its own dimensions
            .compressionType = STREAM_COMP_JPEG,
            .pixelFormat     = STREAM_PIXFMT_JPEG,
    };

    // Timed separately from the encode. Working back from the reported bitrate
    // showed 30-50% of every frame's wall time was being spent here and going
    // completely unmeasured: a 720p frame is ~93 datagrams and each send() is a
    // blocking IPC round trip to IOSU. Encode and send are serialised on this one
    // thread, so this cost is directly in the frame-rate path.
    const OSTime sendStart = OSGetSystemTime();
    StreamSender::SendFrame(sJpegBuffer, (uint32_t) jpegSize, meta);
    sSendUs += OSTicksToMicroseconds(OSGetSystemTime() - sendStart);
}

int threadEntry(int /*argc*/, const char ** /*argv*/) {
    DEBUG_FUNCTION_LINE("Encoder thread running");

    OSTime lastReport = OSGetTime();
    uint32_t lastFramesSent   = StreamSender::GetFramesSent();
    uint64_t lastBytesSent    = StreamSender::GetBytesSent();
    uint64_t lastWireBytesSent = StreamSender::GetWireBytesSent();
    bool windowAnchored = false;
    (void) lastFramesSent; // only read by the DEBUG-only log below
    (void) lastBytesSent;
    (void) lastWireBytesSent;

    // The stop sentinel is the *only* exit. Checking sShouldExit here as well
    // would let a thread that was mid-encode when Stop() was called leave the
    // sentinel sitting in the queue, where the next encoder thread would pop it
    // and die immediately - which is exactly what changing "Encoder core" from
    // the config menu does.
    for (;;) {
        CaptureSlot *slot = ScreenCapture::WaitForFrame();
        if (slot == nullptr) {
            break;
        }

        if (!windowAnchored) {
            // Anchor the very first measurement window to the first frame of the
            // stream. The encoder thread is created at title launch but frames only
            // flow once a client connects, seconds-to-minutes later; without this the
            // first report would divide a whole window of idle time by a handful of
            // frames and read near-zero. It also discards any counts left over from a
            // previous run of this thread (an "Encoder core" change stops and restarts
            // it), so the first report is never a blend of two sessions.
            windowAnchored = true;
            ScreenCapture::ResetCounters();
            sEncodeUs         = 0;
            sCompressUs       = 0;
            sEncodeCount      = 0;
            lastFramesSent    = StreamSender::GetFramesSent();
            lastBytesSent     = StreamSender::GetBytesSent();
            lastWireBytesSent = StreamSender::GetWireBytesSent();
            lastReport        = OSGetTime();
        }

        encodeAndSend(slot);
        ScreenCapture::ReleaseFrame(slot);

        const OSTime now = OSGetTime();
        // Millisecond window, not floored whole seconds: dividing counts by an
        // integer 5 when the window actually ran 5.4 s inflates every rate, and the
        // inflation is worst exactly when frames are sparse (720p) - the regime the
        // measurement is meant to characterise.
        const uint32_t elapsedMs = (uint32_t) OSTicksToMilliseconds(now - lastReport);
        if (elapsedMs >= 5000) {
            // Snapshot every counter once, so the two log lines below are consistent
            // with each other and the divisions cannot see a mid-update value.
            const uint32_t presented  = ScreenCapture::GetPresentedCount();
            const uint32_t captured   = ScreenCapture::GetCapturedCount();
            const uint32_t encBusy    = ScreenCapture::GetSkippedCount();
            const uint32_t encodeCnt  = sEncodeCount;
            const uint32_t framesSent = StreamSender::GetFramesSent() - lastFramesSent;
            const uint64_t bytesSent  = StreamSender::GetBytesSent() - lastBytesSent;
            const uint64_t wireSent   = StreamSender::GetWireBytesSent() - lastWireBytesSent;

            const uint32_t avgEncodeUs = encodeCnt ? (uint32_t) (sEncodeUs / encodeCnt) : 0;
            const uint32_t avgJpegUs   = encodeCnt ? (uint32_t) (sCompressUs / encodeCnt) : 0;
            const uint32_t avgSendUs   = encodeCnt ? (uint32_t) (sSendUs / encodeCnt) : 0;
            const uint32_t avgBytes    = framesSent ? (uint32_t) (bytesSent / framesSent) : 0;
            const uint32_t dilationUs  = measureDilationUs();

            // Rates over the real window. present is the ceiling (game present rate);
            // encBusy is the honest "encoder could not take the frame, no free slot"
            // count - the CPU-bottleneck signal. present - capture is NOT that signal,
            // because frame-skip drops frames on purpose before the slot check.
            const uint32_t presentFps = (uint32_t) ((uint64_t) presented * 1000 / elapsedMs);
            const uint32_t captureFps = (uint32_t) ((uint64_t) captured * 1000 / elapsedMs);
            const uint32_t encodeFps  = (uint32_t) ((uint64_t) encodeCnt * 1000 / elapsedMs);
            const uint32_t txFps      = (uint32_t) ((uint64_t) framesSent * 1000 / elapsedMs);
            const uint32_t encBusyPerS = (uint32_t) ((uint64_t) encBusy * 1000 / elapsedMs);
            // Application-layer throughput (payload + 44-byte header); true link use
            // is ~5-8% higher once UDP/IP/Ethernet framing is added.
            const uint32_t mbitx100   = (uint32_t) ((wireSent * 800ull) / (elapsedMs * 1000ull));

            // Only read by the DEBUG_FUNCTION_LINE calls below, which compile to
            // while(0) in a release build - void them so that build stays warning-clean.
            (void) avgEncodeUs;
            (void) avgJpegUs;
            (void) avgSendUs;
            (void) avgBytes;
            (void) mbitx100;
            (void) presentFps;
            (void) captureFps;
            (void) encodeFps;
            (void) txFps;
            (void) encBusyPerS;
            (void) dilationUs;

            DEBUG_FUNCTION_LINE("[fps] present %u | capture %u | encode %u | tx %u   (enc-busy %u/s, sendfail %u total)",
                                presentFps, captureFps, encodeFps, txFps, encBusyPerS,
                                StreamSender::GetSendFailures());
            DEBUG_FUNCTION_LINE("[cost] encode %u.%02u ms (jpeg %u.%02u) | send %u.%02u ms | %u.%02u Mbit/s | avg %u KB/frame",
                                avgEncodeUs / 1000, (avgEncodeUs % 1000) / 10,
                                avgJpegUs / 1000, (avgJpegUs % 1000) / 10,
                                avgSendUs / 1000, (avgSendUs % 1000) / 10,
                                mbitx100 / 100, mbitx100 % 100, avgBytes / 1024);
            // Compare this across runs: if it doubles when the scene gets busy, the
            // encode/send figures above are inflated by that same factor and the
            // real compute cost is correspondingly lower.
            DEBUG_FUNCTION_LINE("[load] cpu-check %u us for fixed work (higher = this thread is being starved)",
                                dilationUs);

            sEncodeUs     = 0;
            sCompressUs   = 0;
            sSendUs       = 0;
            sEncodeCount  = 0;
            lastFramesSent    = StreamSender::GetFramesSent();
            lastBytesSent     = StreamSender::GetBytesSent();
            lastWireBytesSent = StreamSender::GetWireBytesSent();
            lastReport    = now;
            ScreenCapture::ResetCounters();
        }
    }

    DEBUG_FUNCTION_LINE("Encoder thread stopping");
    return 0;
}

} // namespace

bool Start() {
    if (sThread != nullptr) {
        return true;
    }

    buildSrgbLut();

    // Set up the compressor here rather than inside the thread. If it failed in
    // the thread body, the thread would exit before reaching its loop while
    // sThread stayed non-null - so IsRunning() would claim a live encoder, and a
    // later Stop() would push a stop sentinel that nobody consumes, killing the
    // *next* encoder thread on its first wait.
    sTjHandle = tjInitCompress();
    if (sTjHandle == nullptr) {
        DEBUG_FUNCTION_LINE_ERR("tjInitCompress failed: %s", tjGetErrorStr());
        return false;
    }

    sThread = (OSThread *) memalign(8, sizeof(OSThread));
    if (sThread == nullptr) {
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate the encoder thread");
        tjDestroy(sTjHandle);
        sTjHandle = nullptr;
        return false;
    }
    memset(sThread, 0, sizeof(OSThread));

    sThreadStack = memalign(0x20, THREAD_STACK_SIZE);
    if (sThreadStack == nullptr) {
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate the encoder stack");
        free(sThread);
        sThread = nullptr;
        tjDestroy(sTjHandle);
        sTjHandle = nullptr;
        return false;
    }

    int core = gEncoderCore;
    if (core < 0 || core > 2) {
        core = 2;
    }
    const uint8_t affinity = (uint8_t) (1 << core);

    // Priority 25 sits below a typical game thread (~16), so the console stays
    // playable and the encoder soaks up whatever is left.
    if (!OSCreateThread(sThread, threadEntry, 0, nullptr,
                        (char *) sThreadStack + THREAD_STACK_SIZE, THREAD_STACK_SIZE,
                        25, (OSThreadAttributes) affinity)) {
        DEBUG_FUNCTION_LINE_ERR("OSCreateThread failed");
        free(sThreadStack);
        free(sThread);
        sThreadStack = nullptr;
        sThread      = nullptr;
        tjDestroy(sTjHandle);
        sTjHandle = nullptr;
        return false;
    }

    OSSetThreadName(sThread, "ScreenStreaming encoder");
    OSResumeThread(sThread);
    return true;
}

void Stop() {
    if (sThread == nullptr) {
        return;
    }

    ScreenCapture::SignalStop(); // the sentinel is what ends the loop

    int result = 0;
    OSJoinThread(sThread, &result);

    free(sThreadStack);
    free(sThread);
    sThreadStack = nullptr;
    sThread      = nullptr;

    // Only safe once the thread is definitely gone.
    tjDestroy(sTjHandle);
    sTjHandle = nullptr;

    tjFree(sJpegBuffer);
    sJpegBuffer   = nullptr;
    sJpegCapacity = 0;

    free(sScratchRGB);
    sScratchRGB      = nullptr;
    sScratchCapacity = 0;

    free(sColStart);
    free(sColEnd);
    sColStart    = nullptr;
    sColEnd      = nullptr;
    sColCapacity = 0;

    free(sRecip);
    sRecip = nullptr;

    DEBUG_FUNCTION_LINE("Encoder stopped");
}

bool IsRunning() {
    // Deliberately reports whether a thread is *owned*, not whether it has
    // reached its loop yet. Start() tests the same thing, so the two predicates
    // agree; using sRunning here made a dead-but-not-joined encoder look
    // stopped to callers and started to Start().
    return sThread != nullptr;
}

} // namespace ImageEncoder
