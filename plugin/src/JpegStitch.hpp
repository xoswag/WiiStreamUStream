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
 * Splices horizontal bands, each compressed as its own baseline JPEG, into one
 * standard JPEG.
 *
 * libjpeg cannot spread one image across cores, so each core compresses a band
 * on its own. The bands share identical tables (same quality, standard Huffman,
 * 4:2:0), and every band but the last is a whole number of 16-row MCU rows. That
 * makes each band's entropy-coded data exactly one JPEG *restart interval*:
 * baseline JPEG resets the DC predictors at every restart marker, which is the
 * state each band was encoded from. So the output is band 0's headers plus a DRI
 * marker, then each band's entropy data separated by RSTn markers.
 *
 * The result decodes to exactly the pixels a single-threaded encode of the whole
 * frame would (verified bit-exact against libjpeg), so the client needs no
 * changes at all.
 */
namespace JpegStitch {

constexpr uint32_t MAX_BANDS = 8;

/** Bytes Splice() needs for the given bands, or 0 if any band is unusable. */
uint32_t SplicedSize(const uint8_t *const *bands, const uint32_t *sizes, uint32_t count);

/**
 * Writes the spliced JPEG into out.
 *
 * bandHeight is the height of every band except the last, and must be a
 * multiple of 16. totalHeight is the height of the whole image.
 */
bool Splice(const uint8_t *const *bands, const uint32_t *sizes, uint32_t count,
            uint32_t width, uint32_t totalHeight, uint32_t bandHeight,
            uint8_t *out, uint32_t outCapacity, uint32_t &outSize);

} // namespace JpegStitch
