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
#include "YuvConvert.hpp"
#include "utils/logger.h"

#include <malloc.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

namespace YuvConvert {
namespace {

// Luma tables: JFIF / BT.601 full range, using libjpeg's own 16-bit fixed-point
// coefficients (FIX(0.299) = 19595, FIX(0.587) = 38470, FIX(0.114) = 7471) so the
// result matches what libjpeg would have produced itself. The rounding constant is
// folded into sYB. 3 KB total - stays resident in L1.
uint32_t sYR[256];
uint32_t sYG[256];
uint32_t sYB[256];

uint8_t sSrgb[256];

// ceil(2^22 / n): averaging a box of runtime size becomes a multiply and a shift.
// 22 fractional bits keep the result within 1/255 of a true divide while the
// largest product (255 * n * recip) stays ~4x inside 32 bits.
constexpr uint32_t RECIP_SHIFT = 22;
constexpr uint32_t RECIP_SIZE  = 1024;
uint32_t sRecip[RECIP_SIZE];

// Chroma is computed from the SUM of a 2x2 block of output pixels (each <= 255, so
// sums <= 1020); the four-way average folds into the shift (>> 18, not >> 16). The
// bias is the 128 zero point plus just-under-half for rounding, which keeps every
// result inside [0, 255] without a clamp - libjpeg's CBCR_OFFSET + ONE_HALF - 1.
constexpr int32_t CHROMA_BIAS = (128 << 18) + (1 << 17) - 1;

// How far ahead of the read position to touch the cache: 64 pixels = 256 bytes =
// 8 lines. Without it every new line is a full MEM2 round trip with the in-order
// core stalled on it; the old resampler spent a large share of its 166 cycles per
// output pixel exactly there.
constexpr uint32_t PREFETCH_PX = 64;

inline void prefetch(const uint32_t *base, size_t offsetPx) {
    // Integer arithmetic so running off the end of the surface on the last row is
    // just an address, not an out-of-bounds pointer. dcbt never faults: an
    // untranslatable address is a no-op.
    __builtin_prefetch((const void *) ((uintptr_t) base + offsetPx * 4));
}

inline uint8_t lumaOf(uint32_t r, uint32_t g, uint32_t b) {
    return (uint8_t) ((sYR[r] + sYG[g] + sYB[b]) >> 16);
}

inline uint8_t cbOf(int32_t sr, int32_t sg, int32_t sb) {
    return (uint8_t) ((-11059 * sr - 21709 * sg + 32768 * sb + CHROMA_BIAS) >> 18);
}

inline uint8_t crOf(int32_t sr, int32_t sg, int32_t sb) {
    return (uint8_t) ((32768 * sr - 27439 * sg - 5329 * sb + CHROMA_BIAS) >> 18);
}

constexpr int ilog2(uint32_t v) {
    int r = 0;
    while (v > 1) {
        v >>= 1;
        r++;
    }
    return r;
}

/** Source rows [s0, s1) that output row d is averaged from. */
inline void spanBounds(uint32_t srcLen, uint32_t dstLen, uint32_t d, uint32_t &s0, uint32_t &s1) {
    // 32-bit is enough: GX2 surfaces top out at 8192, and 8192 * 8192 < 2^32.
    s0 = d * srcLen / dstLen;
    s1 = (d + 1) * srcLen / dstLen;
    if (s1 <= s0) s1 = s0 + 1;
    if (s1 > srcLen) s1 = srcLen;
}

/** Averages the BxB block of source pixels whose top-left is p. */
template <int B>
inline void boxFixed(const uint32_t *p, uint32_t pitch, uint32_t &r, uint32_t &g, uint32_t &b) {
    if constexpr (B == 1) {
        const uint32_t v = p[0];
        r                = v >> 24;
        g                = (v >> 16) & 0xFF;
        b                = (v >> 8) & 0xFF;
    } else {
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int yy = 0; yy < B; yy++) {
            const uint32_t *row = p + (size_t) yy * pitch;
            for (int xx = 0; xx < B; xx++) {
                const uint32_t v = row[xx];
                sr += v >> 24;
                sg += (v >> 16) & 0xFF;
                sb += (v >> 8) & 0xFF;
            }
        }
        constexpr uint32_t N = (uint32_t) (B * B);
        if constexpr ((N & (N - 1)) == 0) {
            // A power-of-two box (2x2, 4x4) needs no multiply at all.
            constexpr int S = ilog2(N);
            r               = (sr + N / 2) >> S;
            g               = (sg + N / 2) >> S;
            b               = (sb + N / 2) >> S;
        } else {
            constexpr uint32_t R    = ((1u << RECIP_SHIFT) + N / 2) / N;
            constexpr uint32_t HALF = 1u << (RECIP_SHIFT - 1);
            r                       = (sr * R + HALF) >> RECIP_SHIFT;
            g                       = (sg * R + HALF) >> RECIP_SHIFT;
            b                       = (sb * R + HALF) >> RECIP_SHIFT;
        }
    }
}

/** Averages a w x h block of runtime size. */
inline void boxRuntime(const uint32_t *p, uint32_t pitch, uint32_t w, uint32_t h,
                       uint32_t &r, uint32_t &g, uint32_t &b) {
    uint32_t sr = 0, sg = 0, sb = 0;
    for (uint32_t yy = 0; yy < h; yy++) {
        const uint32_t *row = p + (size_t) yy * pitch;
        for (uint32_t xx = 0; xx < w; xx++) {
            const uint32_t v = row[xx];
            sr += v >> 24;
            sg += (v >> 16) & 0xFF;
            sb += (v >> 8) & 0xFF;
        }
    }
    const uint32_t n = w * h;
    if (n < RECIP_SIZE) {
        const uint32_t rc = sRecip[n];
        r                 = (sr * rc) >> RECIP_SHIFT;
        g                 = (sg * rc) >> RECIP_SHIFT;
        b                 = (sb * rc) >> RECIP_SHIFT;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
    } else {
        // Only reachable for absurd ratios; correctness over speed.
        r = sr / n;
        g = sg / n;
        b = sb / n;
    }
}

/**
 * Writes one 2x2 block of output: four luma samples and one Cb/Cr pair.
 * r0..b3 are the four pixels in reading order (top-left, top-right, bottom-left,
 * bottom-right), already averaged and, if asked for, sRGB-encoded.
 */
inline void emitBlock(uint8_t *oy0, uint8_t *oy1, uint8_t *ocb, uint8_t *ocr, uint32_t x,
                      uint32_t r0, uint32_t g0, uint32_t b0, uint32_t r1, uint32_t g1, uint32_t b1,
                      uint32_t r2, uint32_t g2, uint32_t b2, uint32_t r3, uint32_t g3, uint32_t b3) {
    oy0[x]     = lumaOf(r0, g0, b0);
    oy0[x + 1] = lumaOf(r1, g1, b1);
    oy1[x]     = lumaOf(r2, g2, b2);
    oy1[x + 1] = lumaOf(r3, g3, b3);

    const int32_t sr = (int32_t) (r0 + r1 + r2 + r3);
    const int32_t sg = (int32_t) (g0 + g1 + g2 + g3);
    const int32_t sb = (int32_t) (b0 + b1 + b2 + b3);
    ocb[x >> 1]      = cbOf(sr, sg, sb);
    ocr[x >> 1]      = crOf(sr, sg, sb);
}

/**
 * Exact integer ratio B:1 in both axes (1 = native size). Every box is the same
 * size, so it is a compile-time constant and the box loops unroll completely.
 * 1280x720 -> 640x360 is this path with B = 2, and it is the one a 60 fps
 * stream lives on.
 */
template <int B, bool SRGB>
void convertFixed(const Source &src, const Target &dst, uint32_t y0, uint32_t y1, const BandPlanes &out) {
    const uint32_t pitch = src.pitchPx;
    const uint32_t evenW = (dst.width + 1) & ~1u;
    const uint32_t lastX = dst.width - 1;
    const uint32_t lastY = dst.height - 1;

    // Each iteration covers 2*B source pixels (8*B bytes) of every source row;
    // prefetch once per 32-byte cache line rather than on every iteration.
    constexpr uint32_t PF_MASK = (B == 1) ? 7u : (B == 2) ? 3u : 1u;

    for (uint32_t y = y0; y < y1; y += 2) {
        // An odd-height image gets its missing last row by repeating the edge.
        const uint32_t yb       = (y + 1 <= lastY) ? y + 1 : lastY;
        const uint32_t *rowA    = src.pixels + (size_t) (y * B) * pitch;
        const uint32_t *rowB    = src.pixels + (size_t) (yb * B) * pitch;
        const uint32_t ly       = y - y0;
        uint8_t *oy0            = out.y + (size_t) ly * out.strideY;
        uint8_t *oy1            = oy0 + out.strideY;
        uint8_t *ocb            = out.cb + (size_t) (ly >> 1) * out.strideC;
        uint8_t *ocr            = out.cr + (size_t) (ly >> 1) * out.strideC;

        for (uint32_t x = 0; x < evenW; x += 2) {
            const uint32_t xb = (x + 1 <= lastX) ? x + 1 : lastX;

            if ((x & PF_MASK) == 0) {
                for (int k = 0; k < B; k++) {
                    prefetch(rowA + (size_t) k * pitch, x * B + PREFETCH_PX);
                    prefetch(rowB + (size_t) k * pitch, x * B + PREFETCH_PX);
                }
            }

            uint32_t r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
            boxFixed<B>(rowA + x * B, pitch, r0, g0, b0);
            boxFixed<B>(rowA + xb * B, pitch, r1, g1, b1);
            boxFixed<B>(rowB + x * B, pitch, r2, g2, b2);
            boxFixed<B>(rowB + xb * B, pitch, r3, g3, b3);

            if constexpr (SRGB) {
                r0 = sSrgb[r0], g0 = sSrgb[g0], b0 = sSrgb[b0];
                r1 = sSrgb[r1], g1 = sSrgb[g1], b1 = sSrgb[b1];
                r2 = sSrgb[r2], g2 = sSrgb[g2], b2 = sSrgb[b2];
                r3 = sSrgb[r3], g3 = sSrgb[g3], b3 = sSrgb[b3];
            }

            emitBlock(oy0, oy1, ocb, ocr, x, r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3);
        }
    }
}

/** Any other ratio (e.g. 1280x720 -> 854x480): box sizes vary per pixel. */
template <bool SRGB>
void convertGeneral(const Source &src, const Target &dst, uint32_t y0, uint32_t y1, const BandPlanes &out,
                    const uint32_t *colStart, const uint32_t *colEnd) {
    const uint32_t pitch = src.pitchPx;
    const uint32_t evenW = (dst.width + 1) & ~1u;
    const uint32_t lastX = dst.width - 1;
    const uint32_t lastY = dst.height - 1;

    for (uint32_t y = y0; y < y1; y += 2) {
        const uint32_t yb = (y + 1 <= lastY) ? y + 1 : lastY;
        uint32_t ay0, ay1, by0, by1;
        spanBounds(src.height, dst.height, y, ay0, ay1);
        spanBounds(src.height, dst.height, yb, by0, by1);
        const uint32_t *baseA = src.pixels + (size_t) ay0 * pitch;
        const uint32_t *baseB = src.pixels + (size_t) by0 * pitch;
        const uint32_t hA     = ay1 - ay0;
        const uint32_t hB     = by1 - by0;

        const uint32_t ly = y - y0;
        uint8_t *oy0      = out.y + (size_t) ly * out.strideY;
        uint8_t *oy1      = oy0 + out.strideY;
        uint8_t *ocb      = out.cb + (size_t) (ly >> 1) * out.strideC;
        uint8_t *ocr      = out.cr + (size_t) (ly >> 1) * out.strideC;

        for (uint32_t x = 0; x < evenW; x += 2) {
            const uint32_t xb  = (x + 1 <= lastX) ? x + 1 : lastX;
            const uint32_t cx0 = colStart[x];
            const uint32_t wx0 = colEnd[x] - cx0;
            const uint32_t cx1 = colStart[xb];
            const uint32_t wx1 = colEnd[xb] - cx1;

            if ((x & 7) == 0) {
                for (uint32_t k = 0; k < hA; k++) {
                    prefetch(baseA + (size_t) k * pitch, cx0 + PREFETCH_PX);
                }
                for (uint32_t k = 0; k < hB; k++) {
                    prefetch(baseB + (size_t) k * pitch, cx0 + PREFETCH_PX);
                }
            }

            uint32_t r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
            boxRuntime(baseA + cx0, pitch, wx0, hA, r0, g0, b0);
            boxRuntime(baseA + cx1, pitch, wx1, hA, r1, g1, b1);
            boxRuntime(baseB + cx0, pitch, wx0, hB, r2, g2, b2);
            boxRuntime(baseB + cx1, pitch, wx1, hB, r3, g3, b3);

            if constexpr (SRGB) {
                r0 = sSrgb[r0], g0 = sSrgb[g0], b0 = sSrgb[b0];
                r1 = sSrgb[r1], g1 = sSrgb[g1], b1 = sSrgb[b1];
                r2 = sSrgb[r2], g2 = sSrgb[g2], b2 = sSrgb[b2];
                r3 = sSrgb[r3], g3 = sSrgb[g3], b3 = sSrgb[b3];
            }

            emitBlock(oy0, oy1, ocb, ocr, x, r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3);
        }
    }
}

bool ensureColumns(Scratch &s, uint32_t srcW, uint32_t dstW) {
    if (s.colStart != nullptr && s.colEnd != nullptr && s.forDstW == dstW && s.forSrcW == srcW) {
        return true;
    }
    if (s.capacity < dstW || s.colStart == nullptr || s.colEnd == nullptr) {
        free(s.colStart);
        free(s.colEnd);
        s.colStart = (uint32_t *) malloc(dstW * sizeof(uint32_t));
        s.colEnd   = (uint32_t *) malloc(dstW * sizeof(uint32_t));
        if (s.colStart == nullptr || s.colEnd == nullptr) {
            FreeScratch(s);
            DEBUG_FUNCTION_LINE_ERR("Failed to allocate the resample column table");
            return false;
        }
        s.capacity = dstW;
    }
    for (uint32_t x = 0; x < dstW; x++) {
        uint32_t c0, c1;
        spanBounds(srcW, dstW, x, c0, c1);
        s.colStart[x] = c0;
        s.colEnd[x]   = c1;
    }
    s.forDstW = dstW;
    s.forSrcW = srcW;
    return true;
}

template <int B>
void dispatchFixed(const Source &src, const Target &dst, uint32_t y0, uint32_t y1, const BandPlanes &out) {
    if (dst.srgb) {
        convertFixed<B, true>(src, dst, y0, y1, out);
    } else {
        convertFixed<B, false>(src, dst, y0, y1, out);
    }
}

} // namespace

void Init() {
    for (uint32_t i = 0; i < 256; i++) {
        sYR[i] = 19595u * i;
        sYG[i] = 38470u * i;
        sYB[i] = 7471u * i + 32768u;

        const float v = (float) i / 255.0f;
        const float s = (v <= 0.0031308f) ? (v * 12.92f) : (1.055f * powf(v, 1.0f / 2.4f) - 0.055f);
        int o         = (int) (s * 255.0f + 0.5f);
        if (o < 0) o = 0;
        if (o > 255) o = 255;
        sSrgb[i] = (uint8_t) o;
    }

    sRecip[0] = 0;
    for (uint32_t i = 1; i < RECIP_SIZE; i++) {
        sRecip[i] = ((1u << RECIP_SHIFT) + i - 1) / i;
    }
}

void FreeScratch(Scratch &scratch) {
    free(scratch.colStart);
    free(scratch.colEnd);
    scratch.colStart = nullptr;
    scratch.colEnd   = nullptr;
    scratch.capacity = 0;
    scratch.forDstW  = 0;
    scratch.forSrcW  = 0;
}

uint32_t BandBytes(uint32_t width, uint32_t rows) {
    const uint32_t w = (width + 1) & ~1u;
    const uint32_t h = (rows + 1) & ~1u;
    return w * h + 2 * ((w / 2) * (h / 2));
}

BandPlanes LayoutBand(uint8_t *buffer, uint32_t width, uint32_t rows) {
    const uint32_t w = (width + 1) & ~1u;
    const uint32_t h = (rows + 1) & ~1u;
    BandPlanes p;
    p.y       = buffer;
    p.strideY = w;
    p.cb      = buffer + (size_t) w * h;
    p.strideC = w / 2;
    p.cr      = p.cb + (size_t) (w / 2) * (h / 2);
    return p;
}

void SourceRows(const Source &src, const Target &dst, uint32_t y0, uint32_t y1, uint32_t &sy0, uint32_t &sy1) {
    // The last output row read is y1 - 1: edge duplication only ever repeats rows
    // that are already inside the band, never reaches past it.
    const uint32_t lastOut = ((y1 > dst.height) ? dst.height : y1) - 1;
    uint32_t a0, a1, b0, b1;
    spanBounds(src.height, dst.height, y0, a0, a1);
    spanBounds(src.height, dst.height, lastOut, b0, b1);
    sy0 = a0;
    sy1 = b1;
}

bool ConvertBand(const Source &src, const Target &dst, uint32_t y0, uint32_t y1, const BandPlanes &out,
                 Scratch &scratch) {
    if (src.pixels == nullptr || src.width == 0 || src.height == 0 || src.pitchPx < src.width) {
        return false;
    }
    // Never asked to upscale; the PC does that. Refusing here also guarantees
    // every box below is at least 1x1.
    if (dst.width == 0 || dst.height == 0 || dst.width > src.width || dst.height > src.height) {
        return false;
    }
    if ((y0 & 1) != 0 || y0 >= y1 || y1 > dst.height) {
        return false;
    }

    if (src.width % dst.width == 0 && src.height % dst.height == 0 &&
        src.width / dst.width == src.height / dst.height) {
        switch (src.width / dst.width) {
            case 1:
                dispatchFixed<1>(src, dst, y0, y1, out);
                return true;
            case 2:
                dispatchFixed<2>(src, dst, y0, y1, out);
                return true;
            case 3:
                dispatchFixed<3>(src, dst, y0, y1, out);
                return true;
            case 4:
                dispatchFixed<4>(src, dst, y0, y1, out);
                return true;
            default:
                break; // larger ratios take the general path
        }
    }

    if (!ensureColumns(scratch, src.width, dst.width)) {
        return false;
    }
    if (dst.srgb) {
        convertGeneral<true>(src, dst, y0, y1, out, scratch.colStart, scratch.colEnd);
    } else {
        convertGeneral<false>(src, dst, y0, y1, out, scratch.colStart, scratch.colEnd);
    }
    return true;
}

} // namespace YuvConvert
