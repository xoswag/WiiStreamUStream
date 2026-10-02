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
#pragma once

#include <stdint.h>

/**
 * Downscale + colour-convert in one pass, straight into the planar 4:2:0 YCbCr
 * that JPEG stores.
 *
 * Handing libjpeg-turbo pre-converted planes (tj3CompressFromYUVPlanes8) skips
 * its own RGB->YCbCr conversion and chroma downsampling, both of which run as
 * plain C on the Espresso. We are already touching every source pixel to
 * downscale it, so doing the conversion here costs a few table lookups per
 * pixel instead of a whole extra pass over the frame inside libjpeg.
 *
 * Work is done per band of output rows so several cores can each convert (and
 * then compress) their own slice of the same frame.
 */
namespace YuvConvert {

/** The captured surface: UNORM_R8_G8_B8_A8, so a big-endian 32-bit load is 0xRRGGBBAA. */
struct Source {
    const uint32_t *pixels;
    uint32_t pitchPx; // row pitch in pixels, >= width
    uint32_t width;
    uint32_t height;
};

/** The output image, i.e. the whole frame (not just one band). */
struct Target {
    uint32_t width;
    uint32_t height;
    bool srgb; // gamma-encode linear values on the way through
};

struct BandPlanes {
    uint8_t *y;
    uint8_t *cb;
    uint8_t *cr;
    uint32_t strideY;
    uint32_t strideC;
};

/** Per-thread scratch for the general (non-integer ratio) path. */
struct Scratch {
    uint32_t *colStart = nullptr;
    uint32_t *colEnd   = nullptr;
    uint32_t capacity  = 0;
    uint32_t forDstW   = 0;
    uint32_t forSrcW   = 0;
};

/** Builds the shared lookup tables. Call once, before any thread converts. */
void Init();

void FreeScratch(Scratch &scratch);

/**
 * Bytes needed for one band's Y + Cb + Cr planes. Width and row count are rounded
 * up to even so odd image sizes still get whole 2x2 chroma blocks; the extra
 * row/column is filled by duplicating the edge.
 */
uint32_t BandBytes(uint32_t width, uint32_t rows);

/** Carves the three planes out of one BandBytes()-sized buffer. */
BandPlanes LayoutBand(uint8_t *buffer, uint32_t width, uint32_t rows);

/**
 * The source rows [sy0, sy1) that converting output rows [y0, y1) will read -
 * exactly the range a worker must invalidate from its own data cache first.
 */
void SourceRows(const Source &src, const Target &dst, uint32_t y0, uint32_t y1,
                uint32_t &sy0, uint32_t &sy1);

/**
 * Converts output rows [y0, y1) of the frame into `out`, whose row 0 is frame
 * row y0. y0 must be even; y1 must not exceed dst.height.
 */
bool ConvertBand(const Source &src, const Target &dst, uint32_t y0, uint32_t y1,
                 const BandPlanes &out, Scratch &scratch);

} // namespace YuvConvert
