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
#include "ScreenCapture.hpp"
#include "retain_vars.hpp"
#include "utils/logger.h"

#include <gx2/surface.h>
#include <string.h>
#include <wups.h>

namespace {

/**
 * Set once we have seen the game hand a frame to GX2CopyColorBufferToScanBuffer
 * for a given target. Some titles never call it and instead write the scan
 * buffer themselves and announce it with GX2MarkScanBufferCopied; those are the
 * ones that showed a black stream on upstream. We support both, but only fall
 * back to the second path for targets the first one never fires for, so a normal
 * game is never captured twice per frame.
 */
bool sSawCopyToScanBufferTV = false;
bool sSawCopyToScanBufferDRC = false;

GX2ColorBuffer sLastTVScanBuffer;
GX2ColorBuffer sLastDRCScanBuffer;
bool sHaveLastTVScanBuffer = false;
bool sHaveLastDRCScanBuffer = false;

bool isSelectedTarget(GX2ScanTarget target) {
    return (gScreen == WUPS_STREAMING_SCREEN_TV)
                   ? (target == GX2_SCAN_TARGET_TV)
                   : (target == GX2_SCAN_TARGET_DRC);
}

void captureIfEnabled(const GX2ColorBuffer *colorBuffer, GX2ScanTarget target) {
    if (!StreamingActive() || colorBuffer == nullptr || !isSelectedTarget(target)) {
        return;
    }

    // Register before re-checking the gate. Teardown clears the gate and then
    // waits for this counter, so this ordering is what guarantees a capture is
    // never running while the buffers are being freed.
    __atomic_add_fetch(&gCaptureInFlight, 1, __ATOMIC_ACQ_REL);
    if (StreamingActive()) {
        ScreenCapture::CaptureFrame(colorBuffer, target);
    }
    __atomic_sub_fetch(&gCaptureInFlight, 1, __ATOMIC_ACQ_REL);
}

} // namespace

DECL_FUNCTION(void, GX2CopyColorBufferToScanBuffer, const GX2ColorBuffer *colorBuffer, GX2ScanTarget scan_target) {
    if (scan_target == GX2_SCAN_TARGET_TV) {
        sSawCopyToScanBufferTV = true;
    } else if (scan_target == GX2_SCAN_TARGET_DRC) {
        sSawCopyToScanBufferDRC = true;
    }

    captureIfEnabled(colorBuffer, scan_target);

    real_GX2CopyColorBufferToScanBuffer(colorBuffer, scan_target);
}

DECL_FUNCTION(void, GX2GetCurrentScanBuffer, GX2ScanTarget scanTarget, GX2ColorBuffer *cb) {
    real_GX2GetCurrentScanBuffer(scanTarget, cb);

    if (cb == nullptr) {
        return;
    }
    if (scanTarget == GX2_SCAN_TARGET_TV) {
        memcpy(&sLastTVScanBuffer, cb, sizeof(GX2ColorBuffer));
        sHaveLastTVScanBuffer = true;
    } else if (scanTarget == GX2_SCAN_TARGET_DRC) {
        memcpy(&sLastDRCScanBuffer, cb, sizeof(GX2ColorBuffer));
        sHaveLastDRCScanBuffer = true;
    }
}

DECL_FUNCTION(void, GX2MarkScanBufferCopied, GX2ScanTarget scan_target) {
    if (scan_target == GX2_SCAN_TARGET_TV && !sSawCopyToScanBufferTV && sHaveLastTVScanBuffer) {
        captureIfEnabled(&sLastTVScanBuffer, scan_target);
    } else if (scan_target == GX2_SCAN_TARGET_DRC && !sSawCopyToScanBufferDRC && sHaveLastDRCScanBuffer) {
        captureIfEnabled(&sLastDRCScanBuffer, scan_target);
    }

    real_GX2MarkScanBufferCopied(scan_target);
}

/*
 * The colour buffer handed to GX2CopyColorBufferToScanBuffer does not say
 * whether the scan-out is sRGB, so watch the buffers being configured. Without
 * this the stream has no way to know it should gamma-encode, which is exactly
 * what upstream's "some games might be too dark, some might be too bright" note
 * was describing.
 */
DECL_FUNCTION(void, GX2SetTVBuffer, void *buffer, uint32_t buffer_size, int32_t tv_render_mode,
              GX2SurfaceFormat surface_format, GX2BufferingMode buffering_mode) {
    gTVSurfaceFormat = surface_format;
    DEBUG_FUNCTION_LINE_VERBOSE("TV scan buffer format is now 0x%08X", surface_format);
    real_GX2SetTVBuffer(buffer, buffer_size, tv_render_mode, surface_format, buffering_mode);
}

DECL_FUNCTION(void, GX2SetDRCBuffer, void *buffer, uint32_t buffer_size, uint32_t drc_mode,
              GX2SurfaceFormat surface_format, GX2BufferingMode buffering_mode) {
    gDRCSurfaceFormat = surface_format;
    DEBUG_FUNCTION_LINE_VERBOSE("DRC scan buffer format is now 0x%08X", surface_format);
    real_GX2SetDRCBuffer(buffer, buffer_size, drc_mode, surface_format, buffering_mode);
}

WUPS_MUST_REPLACE(GX2CopyColorBufferToScanBuffer, WUPS_LOADER_LIBRARY_GX2, GX2CopyColorBufferToScanBuffer);
WUPS_MUST_REPLACE(GX2GetCurrentScanBuffer, WUPS_LOADER_LIBRARY_GX2, GX2GetCurrentScanBuffer);
WUPS_MUST_REPLACE(GX2MarkScanBufferCopied, WUPS_LOADER_LIBRARY_GX2, GX2MarkScanBufferCopied);
WUPS_MUST_REPLACE(GX2SetTVBuffer, WUPS_LOADER_LIBRARY_GX2, GX2SetTVBuffer);
WUPS_MUST_REPLACE(GX2SetDRCBuffer, WUPS_LOADER_LIBRARY_GX2, GX2SetDRCBuffer);
