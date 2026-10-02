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
#include "ScreenCapture.hpp"
#include "retain_vars.hpp"
#include "utils/logger.h"

#include <coreinit/cache.h>
#include <coreinit/memory.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <memory/mappedmemory.h>
#include <string.h>

extern "C" {
void GX2ResolveAAColorBuffer(const GX2ColorBuffer *srcColorBuffer,
                             GX2Surface *dstSurface,
                             uint32_t dstMip,
                             uint32_t dstSlice);
}

namespace {

constexpr uint32_t STOP_MESSAGE = 0xDEADBEEF;
constexpr uint32_t FRAME_MESSAGE = 0x1337;

CaptureSlot sSlots[CAPTURE_SLOT_COUNT];

// A pair of queues used as a lock-free hand-off. `sFreeQueue` holds slots the
// hook may fill, `sReadyQueue` holds slots waiting to be encoded. Because both
// have room for every slot, neither send can ever fail, which removes the whole
// "queue was full, now leak the buffer" family of bugs upstream had.
OSMessageQueue sFreeQueue;
OSMessage sFreeQueueMessages[CAPTURE_SLOT_COUNT];

OSMessageQueue sReadyQueue;
OSMessage sReadyQueueMessages[CAPTURE_SLOT_COUNT + 1]; // +1 so the stop message always fits

bool sInitialised = false;

// Written on the game's render thread, read and reset on the encoder thread.
// Touched with __atomic builtins: volatile would neither make the read-modify-write
// atomic nor keep -Wall quiet about incrementing a volatile.
uint32_t sPresented = 0;
uint32_t sCaptured = 0;
uint32_t sSkipped = 0;
uint32_t sFrameSkipCounter = 0;
uint32_t sLoggedAllocFailureSize = 0;

void freeSlotMemory(CaptureSlot &slot) {
    if (slot.colorBuffer.surface.image != nullptr) {
        MEMFreeToMappedMemory(slot.colorBuffer.surface.image);
        slot.colorBuffer.surface.image = nullptr;
    }
    slot.imageCapacity = 0;

    if (slot.resolveImage != nullptr) {
        MEMFreeToMappedMemory(slot.resolveImage);
        slot.resolveImage = nullptr;
    }
    slot.resolveCapacity = 0;
}

/**
 * Waits until the GPU has finished the last copy queued into this slot.
 *
 * With asynchronous GPU sync the encoder can hand a slot back before that copy
 * has landed (it gives a frame up after a timeout), so the slot's memory may
 * still have a GPU write pending when the hook takes it again. That is harmless
 * while the memory stays put - the next copy is queued behind the old one - but
 * freeing it would let the GPU write into memory that now belongs to someone
 * else. Only needed when a slot's buffers are about to be replaced, which
 * happens about once per stream, and always on the render thread, where waiting
 * on GX2 is allowed.
 */
void waitForSlotGpu(CaptureSlot &slot) {
    if (slot.gpuTimestamp != 0) {
        GX2WaitTimeStamp(slot.gpuTimestamp);
        slot.gpuTimestamp = 0;
    }
}

/**
 * Writes back any dirty cache lines a CPU left in freshly allocated GPU memory,
 * once. If one were written back later it would land on top of what the GPU had
 * copied in. Nothing on the CPU ever writes a capture buffer after this - the
 * encoder only reads it - so once is enough, where the old path paid for a
 * flush of the whole buffer on every frame.
 */
void flushOnce(void *image, uint32_t size) {
    DCFlushRange(image, size);
}

/**
 * Shapes the destination surface and makes sure it owns enough memory.
 *
 * The destination is always the *same size as the source*. GX2CopySurface has
 * not been able to resize since Cafe SDK 2.04 ("Using GX2CopySurface to copy
 * between two different formats or surface dimensions has been removed"); when
 * the dimensions disagree it copies nothing at all and leaves the destination
 * holding whatever was in the freshly allocated memory. Upstream asked it for
 * 854x480 out of a 1280x720 source on every frame, which is exactly why its
 * README says some games "doesn't work at all". Scaling now happens on the CPU
 * in the encoder, after the copy.
 */
bool ensureSurface(CaptureSlot &slot, uint32_t width, uint32_t height) {
    GX2ColorBuffer &cb = slot.colorBuffer;

    if (cb.surface.image != nullptr && cb.surface.width == width && cb.surface.height == height) {
        return true; // already the right shape - the steady state, no work at all
    }

    // The shape changed (or this is the first frame). Release the old image
    // outright rather than trying to reuse it: a different geometry can demand a
    // stricter alignment than the existing block was allocated with, and shape
    // changes happen about once per stream, so there is nothing to optimise.
    if (cb.surface.image != nullptr) {
        waitForSlotGpu(slot);
        MEMFreeToMappedMemory(cb.surface.image);
    }
    slot.imageCapacity = 0;

    memset(&cb, 0, sizeof(GX2ColorBuffer));
    cb.surface.use       = (GX2SurfaceUse) (GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
    cb.surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    cb.surface.width     = width;
    cb.surface.height    = height;
    cb.surface.depth     = 1;
    cb.surface.mipLevels = 1;
    cb.surface.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    cb.surface.aa        = GX2_AA_MODE1X;
    cb.surface.tileMode  = GX2_TILE_MODE_LINEAR_ALIGNED;
    cb.viewMip           = 0;
    cb.viewFirstSlice    = 0;
    cb.viewNumSlices     = 1;

    GX2CalcSurfaceSizeAndAlignment(&cb.surface);
    GX2InitColorBufferRegs(&cb);

    // Must come from mapped memory: GX2 cannot DMA out of the regions the plugin
    // heap hands back, so a plain memalign here gives a surface the GPU will
    // refuse to write to.
    cb.surface.image = MEMAllocFromMappedMemoryForGX2Ex(cb.surface.imageSize, cb.surface.alignment);
    if (cb.surface.image == nullptr) {
        // Retried on every frame, so log once per size rather than 60 times a
        // second. There is no useful fallback: a smaller destination would make
        // GX2CopySurface a no-op, so all we can do is say why nothing is arriving.
        if (sLoggedAllocFailureSize != cb.surface.imageSize) {
            sLoggedAllocFailureSize = cb.surface.imageSize;
            DEBUG_FUNCTION_LINE_ERR("Out of mapped memory: cannot allocate %u bytes for a %ux%u "
                                    "capture buffer. The stream will stay black.",
                                    cb.surface.imageSize, width, height);
        }
        return false;
    }
    sLoggedAllocFailureSize = 0;
    slot.imageCapacity      = cb.surface.imageSize;
    flushOnce(cb.surface.image, cb.surface.imageSize);
    DEBUG_FUNCTION_LINE("Allocated a %ux%u capture buffer (%u bytes, pitch %u)",
                        width, height, cb.surface.imageSize, cb.surface.pitch);

    return true;
}

bool resolveAA(CaptureSlot &slot, const GX2ColorBuffer *srcBuffer, GX2Surface &outSurface) {
    outSurface    = srcBuffer->surface;
    outSurface.aa = GX2_AA_MODE1X;
    GX2CalcSurfaceSizeAndAlignment(&outSurface);

    if (slot.resolveCapacity < outSurface.imageSize) {
        if (slot.resolveImage != nullptr) {
            waitForSlotGpu(slot);
            MEMFreeToMappedMemory(slot.resolveImage);
        }
        slot.resolveImage = MEMAllocFromMappedMemoryForGX2Ex(outSurface.imageSize, outSurface.alignment);
        if (slot.resolveImage == nullptr) {
            slot.resolveCapacity = 0;
            DEBUG_FUNCTION_LINE_ERR("Failed to allocate %u bytes to resolve MSAA", outSurface.imageSize);
            return false;
        }
        slot.resolveCapacity = outSurface.imageSize;
        flushOnce(slot.resolveImage, slot.resolveCapacity);
    }

    outSurface.image = slot.resolveImage;
    GX2ResolveAAColorBuffer(srcBuffer, &outSurface, 0, 0);
    return true;
}

} // namespace

bool ScreenCapture::Init() {
    if (sInitialised) {
        return true;
    }

    memset(sSlots, 0, sizeof(sSlots));

    OSInitMessageQueue(&sFreeQueue, sFreeQueueMessages, CAPTURE_SLOT_COUNT);
    OSInitMessageQueue(&sReadyQueue, sReadyQueueMessages, CAPTURE_SLOT_COUNT + 1);

    for (auto &slot : sSlots) {
        OSMessage msg;
        msg.message = (void *) FRAME_MESSAGE;
        msg.args[0] = (uint32_t) &slot;
        if (!OSSendMessage(&sFreeQueue, &msg, OS_MESSAGE_FLAGS_NONE)) {
            DEBUG_FUNCTION_LINE_ERR("Failed to seed the free queue");
            return false;
        }
    }

    sFrameSkipCounter = 0;
    sInitialised      = true;
    return true;
}

void ScreenCapture::Shutdown() {
    if (!sInitialised) {
        return;
    }
    sInitialised = false;

    // Drain both queues so nothing is left pointing at memory we are about to
    // release, then free the surfaces themselves.
    OSMessage msg;
    while (OSReceiveMessage(&sFreeQueue, &msg, OS_MESSAGE_FLAGS_NONE)) {
    }
    while (OSReceiveMessage(&sReadyQueue, &msg, OS_MESSAGE_FLAGS_NONE)) {
    }

    for (auto &slot : sSlots) {
        freeSlotMemory(slot);
    }

    DEBUG_FUNCTION_LINE("Capture buffers released");
}

bool ScreenCapture::CaptureFrame(const GX2ColorBuffer *srcBuffer, GX2ScanTarget scanTarget) {
    if (!sInitialised || srcBuffer == nullptr || srcBuffer->surface.image == nullptr) {
        return false;
    }

    // Count the present before any gate: this is the ceiling the encoder is
    // measured against.
    __atomic_add_fetch(&sPresented, 1, __ATOMIC_RELAXED);

    // Cheap rejections first. Upstream did the 3.6 MB copy and a full GPU
    // pipeline stall *before* asking whether anyone could take the frame, so it
    // paid the entire cost of every frame it then threw away.
    if (++sFrameSkipCounter <= (uint32_t) gFrameSkip) {
        return false;
    }

    OSMessage msg;
    if (!OSReceiveMessage(&sFreeQueue, &msg, OS_MESSAGE_FLAGS_NONE)) {
        __atomic_add_fetch(&sSkipped, 1, __ATOMIC_RELAXED);
        return false; // encoder still busy - drop before doing any work
    }

    // Only spend the pacing budget on a frame we actually took. Resetting above
    // would mean a frame dropped for a busy encoder also cost the next
    // gFrameSkip vsyncs of not even checking whether a slot had freed up.
    sFrameSkipCounter = 0;

    auto *slot = (CaptureSlot *) msg.args[0];

    const uint32_t width  = srcBuffer->surface.width;
    const uint32_t height = srcBuffer->surface.height;

    bool ok = ensureSurface(*slot, width, height);
    if (ok) {
        GX2Surface *source = nullptr;
        GX2Surface resolved;
        uint32_t srcMip   = 0;
        uint32_t srcSlice = 0;

        if (srcBuffer->surface.aa == GX2_AA_MODE1X) {
            source   = (GX2Surface *) &srcBuffer->surface;
            srcMip   = srcBuffer->viewMip;
            srcSlice = srcBuffer->viewFirstSlice;
        } else if (resolveAA(*slot, srcBuffer, resolved)) {
            // The resolve already flattened the view down to a single mip/slice.
            source = &resolved;
        } else {
            ok = false;
        }

        if (ok) {
            const bool blocking = UseBlockingGpuSync();
            if (blocking) {
                // The old capture path, kept exactly as it was as the fallback.
                // The async path flushes once at allocation instead (see
                // flushOnce): this is ~115k cache operations on the game's render
                // thread for every 720p frame captured.
                GX2Invalidate(GX2_INVALIDATE_MODE_CPU, slot->colorBuffer.surface.image, slot->colorBuffer.surface.imageSize);
            }

            GX2CopySurface(source, srcMip, srcSlice, &slot->colorBuffer.surface, 0, 0);

            GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, slot->colorBuffer.surface.image, slot->colorBuffer.surface.imageSize);

            if (blocking) {
                GX2DrawDone();
                slot->gpuTimestamp = 0;
            } else {
                // GX2DrawDone() is precisely GX2Flush() followed by
                // GX2WaitTimeStamp(GX2GetLastSubmittedTimeStamp()). Only the first
                // half belongs on the game's render thread: submit the copy and note
                // the timestamp that retires with it, then leave the waiting to the
                // encoder. The blocking version stalled the game until the GPU went
                // idle on every frame we captured, which at 60 captures a second
                // would be every frame it drew.
                GX2Flush();
                slot->gpuTimestamp = GX2GetLastSubmittedTimeStamp();
            }
            // The matching DCInvalidateRange happens on the encoder, per band, on the
            // core that is about to read - dcbi works on the executing core's cache.
        }
    }

    if (!ok) {
        __atomic_add_fetch(&sSkipped, 1, __ATOMIC_RELAXED);
        OSSendMessage(&sFreeQueue, &msg, OS_MESSAGE_FLAGS_NONE);
        return false;
    }

    const GX2SurfaceFormat scanFormat =
            (scanTarget == GX2_SCAN_TARGET_TV) ? gTVSurfaceFormat : gDRCSurfaceFormat;
    // Bit 0x400 marks the sRGB variants of the surface formats.
    slot->sourceIsSRGB = (scanFormat & 0x400) != 0;

    __atomic_add_fetch(&sCaptured, 1, __ATOMIC_RELAXED);
    OSSendMessage(&sReadyQueue, &msg, OS_MESSAGE_FLAGS_NONE);
    return true;
}

CaptureSlot *ScreenCapture::WaitForFrame() {
    OSMessage msg;
    if (!OSReceiveMessage(&sReadyQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING)) {
        return nullptr;
    }
    if ((uint32_t) msg.message == STOP_MESSAGE) {
        return nullptr;
    }
    return (CaptureSlot *) msg.args[0];
}

bool ScreenCapture::WaitForGpu(const CaptureSlot *slot, uint32_t timeoutMs) {
    const OSTime ts = slot->gpuTimestamp;
    if (ts == 0) {
        return true; // blocking sync already waited on the render thread
    }

    // Registered exactly like a capture in progress. Teardown clears the gates and
    // then waits for gCaptureInFlight to drain before the title is allowed to shut
    // GX2 down, so this can never be left polling a GPU that no longer exists.
    // The full fence is what stops the gate read below from overtaking this
    // registration - see captureIfEnabled() in function_patcher.cpp.
    __atomic_add_fetch(&gCaptureInFlight, 1, __ATOMIC_ACQ_REL);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    bool done            = false;
    const OSTime deadline = OSGetSystemTime() + OSMillisecondsToTicks(timeoutMs);
    while (StreamingActive()) {
        // A plain read of the retired timestamp (TCLReadTimestamp underneath) with
        // no main-core check - unlike issuing commands, it is safe from any thread.
        if (GX2GetRetiredTimeStamp() >= ts) {
            done = true;
            break;
        }
        if (OSGetSystemTime() >= deadline) {
            break;
        }
        OSSleepTicks(OSMicrosecondsToTicks(200));
    }
    __atomic_sub_fetch(&gCaptureInFlight, 1, __ATOMIC_ACQ_REL);
    return done;
}

void ScreenCapture::ReleaseFrame(CaptureSlot *slot) {
    if (slot == nullptr) {
        return;
    }
    OSMessage msg;
    msg.message = (void *) FRAME_MESSAGE;
    msg.args[0] = (uint32_t) slot;
    OSSendMessage(&sFreeQueue, &msg, OS_MESSAGE_FLAGS_NONE);
}

void ScreenCapture::SignalStop() {
    OSMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.message = (void *) STOP_MESSAGE;
    // Jump the queue: the encoder must wake up even when every slot is pending.
    OSSendMessage(&sReadyQueue, &msg, OS_MESSAGE_FLAGS_HIGH_PRIORITY);
}

uint32_t ScreenCapture::GetPresentedCount() {
    return __atomic_load_n(&sPresented, __ATOMIC_RELAXED);
}

uint32_t ScreenCapture::GetCapturedCount() {
    return __atomic_load_n(&sCaptured, __ATOMIC_RELAXED);
}

uint32_t ScreenCapture::GetSkippedCount() {
    return __atomic_load_n(&sSkipped, __ATOMIC_RELAXED);
}

void ScreenCapture::ResetCounters() {
    __atomic_store_n(&sPresented, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sCaptured, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sSkipped, 0, __ATOMIC_RELAXED);
}
