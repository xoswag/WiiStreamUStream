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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/
#pragma once

#include <gx2/context.h>
#include <gx2/enum.h>
#include <gx2/surface.h>
#include <stdint.h>

/**
 * One capture buffer. Allocated once and recycled for the lifetime of the
 * stream rather than per frame.
 *
 * Upstream allocated and freed the whole RGBA surface on every single frame -
 * 3.6 MB at 720p, 60 times a second, most of which was then thrown away because
 * the encoder was still busy. That is 220 MB/s of allocator traffic, and it
 * fragments the heap until memalign starts failing and frames silently vanish.
 */
struct CaptureSlot {
    GX2ColorBuffer colorBuffer;
    uint32_t imageCapacity;   // bytes currently allocated for colorBuffer.surface.image
    void *resolveImage;       // scratch for the MSAA resolve path, allocated on demand
    uint32_t resolveCapacity;
    bool sourceIsSRGB;        // captured at submit time, see gTVSurfaceFormat
};

/** How many frames may be in flight between the GX2 hook and the encoder. */
#define CAPTURE_SLOT_COUNT 2

class ScreenCapture {
public:
    /** Allocates the queues. Surfaces are allocated lazily on first capture. */
    static bool Init();

    /**
     * Stops the pipeline and frees every buffer. Safe to call twice.
     * The caller must have cleared the gates behind StreamingActive() and called
     * StreamWaitForCapturesToFinish() first.
     */
    static void Shutdown();

    /**
     * Called from the GX2 hook on the game's render thread.
     * Returns false if the frame was skipped, which is the normal case whenever
     * the encoder is still busy.
     */
    static bool CaptureFrame(const GX2ColorBuffer *srcBuffer, GX2ScanTarget scanTarget);

    /**
     * Blocks until a captured frame is available.
     * Returns nullptr when the pipeline is shutting down.
     */
    static CaptureSlot *WaitForFrame();

    /** Hands a slot back so the hook can fill it again. */
    static void ReleaseFrame(CaptureSlot *slot);

    /** Wakes WaitForFrame() so the encoder thread can exit. */
    static void SignalStop();

    static uint32_t GetCapturedCount();
    static uint32_t GetSkippedCount();
    static void ResetCounters();
};
