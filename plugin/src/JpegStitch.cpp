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
#include "JpegStitch.hpp"

#include <string.h>

namespace JpegStitch {
namespace {

constexpr uint32_t DRI_BYTES = 6; // FF DD 00 04 RIhi RIlo
constexpr uint32_t RST_BYTES = 2; // FF D0..D7
constexpr uint32_t EOI_BYTES = 2; // FF D9

struct Segments {
    uint32_t sof;          // offset of the SOF0 marker (its 0xFF)
    uint32_t sos;          // offset of the SOS marker (its 0xFF)
    uint32_t entropyStart; // first byte of entropy-coded data
    uint32_t entropyEnd;   // one past the last byte, i.e. the EOI marker
};

bool isOtherSof(uint8_t m) {
    // C0 is baseline. C4 (DHT), C8 (JPG) and CC (DAC) share the range but are not
    // frame headers. Anything else in C1..CF is progressive, lossless or
    // arithmetic, none of which this scheme is valid for.
    return m >= 0xC1 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
}

/** Walks the marker segments up to SOS. Mirrors the Python it was validated with. */
bool parse(const uint8_t *j, uint32_t n, Segments &s) {
    if (j == nullptr || n < 4 || j[0] != 0xFF || j[1] != 0xD8 || j[n - 2] != 0xFF || j[n - 1] != 0xD9) {
        return false;
    }
    bool haveSof = false;
    uint32_t pos = 2;
    while (pos + 4 <= n) {
        if (j[pos] != 0xFF) {
            return false;
        }
        const uint32_t start = pos;
        while (pos < n && j[pos] == 0xFF) { // fill bytes
            pos++;
        }
        if (pos >= n) {
            return false;
        }
        const uint8_t marker = j[pos++];
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            continue; // standalone, no length
        }
        if (pos + 2 > n) {
            return false;
        }
        const uint32_t len = ((uint32_t) j[pos] << 8) | j[pos + 1];
        if (len < 2 || pos + len > n) {
            return false;
        }
        if (isOtherSof(marker)) {
            return false;
        }
        if (marker == 0xDD) {
            return false; // already has a restart interval - not something we produce
        }
        if (marker == 0xC0) {
            if (len < 8) {
                return false;
            }
            s.sof   = start;
            haveSof = true;
        }
        if (marker == 0xDA) {
            s.sos          = start;
            s.entropyStart = pos + len;
            s.entropyEnd   = n - 2;
            return haveSof && s.sof < s.sos && s.entropyStart <= s.entropyEnd;
        }
        pos += len;
    }
    return false;
}

} // namespace

uint32_t SplicedSize(const uint8_t *const *bands, const uint32_t *sizes, uint32_t count) {
    if (count == 0 || count > MAX_BANDS) {
        return 0;
    }
    Segments s;
    if (!parse(bands[0], sizes[0], s)) {
        return 0;
    }
    uint32_t total = s.sos + DRI_BYTES + (s.entropyEnd - s.sos);
    for (uint32_t i = 1; i < count; i++) {
        if (!parse(bands[i], sizes[i], s)) {
            return 0;
        }
        total += RST_BYTES + (s.entropyEnd - s.entropyStart);
    }
    return total + EOI_BYTES;
}

bool Splice(const uint8_t *const *bands, const uint32_t *sizes, uint32_t count,
            uint32_t width, uint32_t totalHeight, uint32_t bandHeight,
            uint8_t *out, uint32_t outCapacity, uint32_t &outSize) {
    outSize = 0;
    if (count < 2 || count > MAX_BANDS || out == nullptr || width == 0 || totalHeight > 0xFFFF) {
        return false;
    }
    if (bandHeight == 0 || (bandHeight % 16) != 0 || (count - 1) * bandHeight >= totalHeight) {
        return false;
    }

    // One restart interval = one band = (bandHeight / 16) rows of 16x16 MCUs.
    const uint32_t mcusPerRow = (width + 15) / 16;
    const uint32_t interval   = (bandHeight / 16) * mcusPerRow;
    if (interval == 0 || interval > 0xFFFF) {
        return false;
    }

    Segments seg[MAX_BANDS];
    for (uint32_t i = 0; i < count; i++) {
        if (!parse(bands[i], sizes[i], seg[i])) {
            return false;
        }
    }

    uint32_t need = seg[0].sos + DRI_BYTES + (seg[0].entropyEnd - seg[0].sos) + EOI_BYTES;
    for (uint32_t i = 1; i < count; i++) {
        need += RST_BYTES + (seg[i].entropyEnd - seg[i].entropyStart);
    }
    if (need > outCapacity) {
        return false;
    }

    uint32_t o = 0;
    // Band 0's headers (SOI, APP0, DQT, SOF0, DHT ...) up to but not including SOS.
    memcpy(out + o, bands[0], seg[0].sos);
    o += seg[0].sos;

    out[o++] = 0xFF;
    out[o++] = 0xDD;
    out[o++] = 0x00;
    out[o++] = 0x04;
    out[o++] = (uint8_t) (interval >> 8);
    out[o++] = (uint8_t) (interval & 0xFF);

    // SOS segment and band 0's entropy data.
    memcpy(out + o, bands[0] + seg[0].sos, seg[0].entropyEnd - seg[0].sos);
    o += seg[0].entropyEnd - seg[0].sos;

    for (uint32_t i = 1; i < count; i++) {
        out[o++] = 0xFF;
        out[o++] = (uint8_t) (0xD0 + ((i - 1) & 7));
        const uint32_t len = seg[i].entropyEnd - seg[i].entropyStart;
        memcpy(out + o, bands[i] + seg[i].entropyStart, len);
        o += len;
    }

    out[o++] = 0xFF;
    out[o++] = 0xD9;

    // SOF0 sits before SOS, so it is at the same offset in the output. Its height
    // field is 5 bytes in: FF C0 Lh Ll P Yh Yl.
    out[seg[0].sof + 5] = (uint8_t) (totalHeight >> 8);
    out[seg[0].sof + 6] = (uint8_t) (totalHeight & 0xFF);

    outSize = o;
    return o == need;
}

} // namespace JpegStitch
