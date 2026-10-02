// Host tests for the encoder: the real ImageEncoder / YuvConvert / JpegStitch
// sources, run against stubbed console APIs and a real libjpeg-turbo 3.x.
//
// What a pass means:
//  - YuvConvert produces the right samples, in the right places, for exact and
//    general ratios, odd sizes and sRGB (checked against an independent model).
//  - Spliced band JPEGs decode to exactly the pixels of a one-shot encode.
//  - Every frame the multi-core encoder hands to the sender decodes to exactly
//    what a single-core encode of the same frame would - including while a
//    "core" is being starved, followers are benched and rejoin, and the encoder
//    is stopped and restarted mid-stream.
//  - Stop() always returns (a hang here is what froze the console before), and
//    no capture slot is ever double-released or leaked.
//
// What it cannot tell you: anything about real Wii U timing, cache behaviour or
// the GPU. That needs hardware.
#include "test_hooks.hpp"

#include "ImageEncoder.hpp"
#include "JpegStitch.hpp"
#include "YuvConvert.hpp"
#include "retain_vars.hpp"

#include <turbojpeg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;
int gWarnings = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
            gFailures++;                                    \
        }                                                   \
    } while (0)

void warn(const char *msg) {
    printf("WARN: %s\n", msg);
    gWarnings++;
}

// =============================================================================
// Helpers
// =============================================================================

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgb;
};

bool decode(tjhandle tj, const uint8_t *jpeg, size_t size, Image &out) {
    if (tj3DecompressHeader(tj, jpeg, size) != 0) {
        return false;
    }
    out.w = (uint32_t) tj3Get(tj, TJPARAM_JPEGWIDTH);
    out.h = (uint32_t) tj3Get(tj, TJPARAM_JPEGHEIGHT);
    out.rgb.assign((size_t) out.w * out.h * 3, 0);
    return tj3Decompress8(tj, jpeg, size, out.rgb.data(), 0, TJPF_RGB) == 0;
}

bool hasRestartInterval(const std::vector<uint8_t> &jpeg) {
    for (size_t i = 2; i + 1 < jpeg.size(); i++) {
        if (jpeg[i] == 0xFF && jpeg[i + 1] == 0xDA) {
            return false; // reached SOS without a DRI
        }
        if (jpeg[i] == 0xFF && jpeg[i + 1] == 0xDD) {
            return true;
        }
    }
    return false;
}

void spanBounds(uint32_t srcLen, uint32_t dstLen, uint32_t d, uint32_t &s0, uint32_t &s1) {
    s0 = (uint32_t) ((uint64_t) d * srcLen / dstLen);
    s1 = (uint32_t) ((uint64_t) (d + 1) * srcLen / dstLen);
    if (s1 <= s0) s1 = s0 + 1;
    if (s1 > srcLen) s1 = srcLen;
}

double srgbEncode(double v) { // v in 0..255
    const double x = v / 255.0;
    const double s = x <= 0.0031308 ? x * 12.92 : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
    return s * 255.0;
}

uint8_t srgbLutLike(uint32_t i) { // the plugin's table, computed the same way
    const float v = (float) i / 255.0f;
    const float s = (v <= 0.0031308f) ? (v * 12.92f) : (1.055f * powf(v, 1.0f / 2.4f) - 0.055f);
    int o         = (int) (s * 255.0f + 0.5f);
    return (uint8_t) (o < 0 ? 0 : o > 255 ? 255 : o);
}

/** Float area-average downscale (+ optional sRGB) of a generated frame: the "what it should look like". */
Image idealDownscale(uint32_t id, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, bool srgb) {
    const uint32_t pitch = (sw + 63) & ~63u;
    std::vector<uint32_t> px((size_t) pitch * sh);
    fakes::fillFrame(px.data(), sw, sh, pitch, id);
    Image out;
    out.w = dw;
    out.h = dh;
    out.rgb.resize((size_t) dw * dh * 3);
    for (uint32_t y = 0; y < dh; y++) {
        uint32_t y0, y1;
        spanBounds(sh, dh, y, y0, y1);
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0, x1;
            spanBounds(sw, dw, x, x0, x1);
            double r = 0, g = 0, b = 0;
            for (uint32_t yy = y0; yy < y1; yy++) {
                for (uint32_t xx = x0; xx < x1; xx++) {
                    const uint32_t p = px[(size_t) yy * pitch + xx];
                    r += p >> 24;
                    g += (p >> 16) & 0xFF;
                    b += (p >> 8) & 0xFF;
                }
            }
            const double n = (double) (y1 - y0) * (x1 - x0);
            r /= n, g /= n, b /= n;
            if (srgb) {
                r = srgbEncode(r), g = srgbEncode(g), b = srgbEncode(b);
            }
            uint8_t *o = &out.rgb[((size_t) y * dw + x) * 3];
            o[0]       = (uint8_t) lround(r);
            o[1]       = (uint8_t) lround(g);
            o[2]       = (uint8_t) lround(b);
        }
    }
    return out;
}

double psnr(const Image &a, const Image &b) {
    if (a.w != b.w || a.h != b.h) {
        return 0;
    }
    double se = 0;
    for (size_t i = 0; i < a.rgb.size(); i++) {
        const double d = (double) a.rgb[i] - b.rgb[i];
        se += d * d;
    }
    const double mse = se / (double) a.rgb.size();
    return mse == 0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

/** What the plugin should pick for a given setting (mirrors computeTargetSize). */
void expectedSize(int32_t captureSize, uint32_t sw, uint32_t sh, uint32_t &dw, uint32_t &dh) {
    uint32_t mw = 0, mh = 0;
    switch (captureSize) {
        case WUPS_STREAMING_SIZE_720P: mw = 1280, mh = 720; break;
        case WUPS_STREAMING_SIZE_480P: mw = 854, mh = 480; break;
        case WUPS_STREAMING_SIZE_360P: mw = 640, mh = 360; break;
        case WUPS_STREAMING_SIZE_240P: mw = 426, mh = 240; break;
        case WUPS_STREAMING_SIZE_180P: mw = 320, mh = 180; break;
        default: break;
    }
    if (mw == 0 || (sw <= mw && sh <= mh)) {
        dw = sw, dh = sh;
        return;
    }
    const double s = std::min((double) mw / sw, (double) mh / sh);
    dw             = ((uint32_t) (sw * s + 0.5)) & ~1u;
    dh             = ((uint32_t) (sh * s + 0.5)) & ~1u;
    dw             = std::min(std::max(dw, 16u), sw);
    dh             = std::min(std::max(dh, 16u), sh);
}

/** One-shot single-band encode of the whole frame through the FAST pipeline. */
bool referenceFast(tjhandle tj, uint32_t id, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, bool srgb,
                   int quality, std::vector<uint8_t> &jpeg) {
    const uint32_t pitch = (sw + 63) & ~63u;
    std::vector<uint32_t> px((size_t) pitch * sh);
    fakes::fillFrame(px.data(), sw, sh, pitch, id);
    const YuvConvert::Source src = {px.data(), pitch, sw, sh};
    const YuvConvert::Target dst = {dw, dh, srgb};
    std::vector<uint8_t> planes(YuvConvert::BandBytes(dw, dh));
    const YuvConvert::BandPlanes p = YuvConvert::LayoutBand(planes.data(), dw, dh);
    YuvConvert::Scratch scratch;
    const bool ok = YuvConvert::ConvertBand(src, dst, 0, dh, p, scratch);
    YuvConvert::FreeScratch(scratch);
    if (!ok) {
        return false;
    }
    tj3Set(tj, TJPARAM_QUALITY, quality);
    const unsigned char *pl[3] = {p.y, p.cb, p.cr};
    const int st[3]            = {(int) p.strideY, (int) p.strideC, (int) p.strideC};
    unsigned char *buf         = nullptr;
    size_t size                = 0;
    if (tj3CompressFromYUVPlanes8(tj, pl, (int) dw, st, (int) dh, &buf, &size) != 0) {
        return false;
    }
    jpeg.assign(buf, buf + size);
    tj3Free(buf);
    return true;
}

tjhandle makeCompressor() {
    tjhandle tj = tj3Init(TJINIT_COMPRESS);
    tj3Set(tj, TJPARAM_SUBSAMP, TJSAMP_420);
    tj3Set(tj, TJPARAM_FASTDCT, 1);
    tj3Set(tj, TJPARAM_OPTIMIZE, 0);
    tj3Set(tj, TJPARAM_PROGRESSIVE, 0);
    tj3Set(tj, TJPARAM_ARITHMETIC, 0);
    tj3Set(tj, TJPARAM_RESTARTBLOCKS, 0);
    tj3Set(tj, TJPARAM_RESTARTROWS, 0);
    return tj;
}

// =============================================================================
// Unit: YuvConvert against an independent model
// =============================================================================

/** The averaged value the plugin computes for one output pixel, by its own rules. */
void modelAverage(const std::vector<uint32_t> &px, uint32_t pitch, uint32_t sw, uint32_t sh, uint32_t dw,
                  uint32_t dh, uint32_t x, uint32_t y, uint32_t &r, uint32_t &g, uint32_t &b, double &fr,
                  double &fg, double &fb) {
    uint32_t x0, x1, y0, y1;
    spanBounds(sw, dw, x, x0, x1);
    spanBounds(sh, dh, y, y0, y1);
    uint32_t sr = 0, sg = 0, sb = 0;
    for (uint32_t yy = y0; yy < y1; yy++) {
        for (uint32_t xx = x0; xx < x1; xx++) {
            const uint32_t p = px[(size_t) yy * pitch + xx];
            sr += p >> 24;
            sg += (p >> 16) & 0xFF;
            sb += (p >> 8) & 0xFF;
        }
    }
    const uint32_t n = (x1 - x0) * (y1 - y0);
    fr = (double) sr / n, fg = (double) sg / n, fb = (double) sb / n;
    const bool fixed = sw % dw == 0 && sh % dh == 0 && sw / dw == sh / dh && sw / dw <= 4;
    if (fixed) {
        if ((n & (n - 1)) == 0) {
            const uint32_t s = n == 1 ? 0 : n == 4 ? 2 : 4;
            r = (sr + n / 2) >> s, g = (sg + n / 2) >> s, b = (sb + n / 2) >> s;
        } else {
            const uint32_t R = ((1u << 22) + n / 2) / n, H = 1u << 21;
            r = (sr * R + H) >> 22, g = (sg * R + H) >> 22, b = (sb * R + H) >> 22;
        }
    } else {
        const uint32_t rc = ((1u << 22) + n - 1) / n;
        r = std::min<uint32_t>((sr * rc) >> 22, 255);
        g = std::min<uint32_t>((sg * rc) >> 22, 255);
        b = std::min<uint32_t>((sb * rc) >> 22, 255);
    }
}

void testYuvConvert() {
    printf("== YuvConvert vs model\n");
    struct Case {
        uint32_t sw, sh, dw, dh;
        bool srgb;
    } cases[] = {
            {1280, 720, 640, 360, false}, {1280, 720, 640, 360, true}, {1280, 720, 1280, 720, false},
            {1280, 720, 426, 240, false}, {1280, 720, 854, 480, true}, {1280, 720, 320, 180, false},
            {960, 540, 320, 180, true},   {853, 479, 853, 479, false}, {101, 67, 50, 33, false},
            {64, 64, 21, 21, true},       {854, 480, 640, 360, false},
    };
    std::mt19937 rng(7);
    for (const Case &c : cases) {
        const uint32_t pitch = (c.sw + 63) & ~63u;
        std::vector<uint32_t> px((size_t) pitch * c.sh);
        fakes::fillFrame(px.data(), c.sw, c.sh, pitch, 1234);
        const YuvConvert::Source src = {px.data(), pitch, c.sw, c.sh};
        const YuvConvert::Target dst = {c.dw, c.dh, c.srgb};
        YuvConvert::Scratch scratch;

        int maxY = 0, maxC = 0, maxAvg = 0, bad = 0;
        uint32_t y0 = 0;
        while (y0 < c.dh) {
            uint32_t rows = 2 + 2 * (rng() % 40); // random even band heights
            uint32_t y1   = std::min(c.dh, y0 + rows);
            std::vector<uint8_t> buf(YuvConvert::BandBytes(c.dw, y1 - y0), 0xCD);
            const YuvConvert::BandPlanes p = YuvConvert::LayoutBand(buf.data(), c.dw, y1 - y0);

            uint32_t sy0, sy1;
            YuvConvert::SourceRows(src, dst, y0, y1, sy0, sy1);
            uint32_t ey0, ey1, tmp;
            spanBounds(c.sh, c.dh, y0, ey0, tmp);
            spanBounds(c.sh, c.dh, y1 - 1, tmp, ey1);
            CHECK(sy0 == ey0 && sy1 == ey1, "%ux%u->%ux%u: SourceRows(%u,%u) = [%u,%u), expected [%u,%u)", c.sw,
                  c.sh, c.dw, c.dh, y0, y1, sy0, sy1, ey0, ey1);

            if (!YuvConvert::ConvertBand(src, dst, y0, y1, p, scratch)) {
                CHECK(false, "ConvertBand failed for %ux%u->%ux%u rows %u..%u", c.sw, c.sh, c.dw, c.dh, y0, y1);
                break;
            }
            for (uint32_t y = y0; y < y1; y++) {
                for (uint32_t x = 0; x < c.dw; x++) {
                    uint32_t r, g, b;
                    double fr, fg, fb;
                    modelAverage(px, pitch, c.sw, c.sh, c.dw, c.dh, x, y, r, g, b, fr, fg, fb);
                    // The plugin's integer average must stay within 1 of the true one.
                    const double dAvg = std::max({fabs((double) r - fr), fabs((double) g - fg), fabs((double) b - fb)});
                    maxAvg            = std::max(maxAvg, (int) ceil(dAvg));
                    if (c.srgb) {
                        r = srgbLutLike(r), g = srgbLutLike(g), b = srgbLutLike(b);
                    }
                    const int want = (int) lround(0.299 * r + 0.587 * g + 0.114 * b);
                    const int got  = p.y[(size_t) (y - y0) * p.strideY + x];
                    maxY           = std::max(maxY, abs(want - got));
                }
            }
            for (uint32_t cy = y0 / 2; cy < (y1 + 1) / 2; cy++) {
                for (uint32_t cx = 0; cx < (c.dw + 1) / 2; cx++) {
                    double R = 0, G = 0, B = 0;
                    for (int k = 0; k < 4; k++) {
                        const uint32_t x = std::min(cx * 2 + (k & 1), c.dw - 1);
                        const uint32_t y = std::min(cy * 2 + (k >> 1), c.dh - 1);
                        uint32_t r, g, b;
                        double fr, fg, fb;
                        modelAverage(px, pitch, c.sw, c.sh, c.dw, c.dh, x, y, r, g, b, fr, fg, fb);
                        if (c.srgb) {
                            r = srgbLutLike(r), g = srgbLutLike(g), b = srgbLutLike(b);
                        }
                        R += r, G += g, B += b;
                    }
                    R /= 4, G /= 4, B /= 4;
                    const int wantCb = (int) lround(128 - 0.168736 * R - 0.331264 * G + 0.5 * B);
                    const int wantCr = (int) lround(128 + 0.5 * R - 0.418688 * G - 0.081312 * B);
                    const size_t off = (size_t) (cy - y0 / 2) * p.strideC + cx;
                    maxC             = std::max({maxC, abs(wantCb - p.cb[off]), abs(wantCr - p.cr[off])});
                }
            }
            y0 = y1;
        }
        YuvConvert::FreeScratch(scratch);
        if (maxY > 1 || maxC > 1 || maxAvg > 1) {
            bad = 1;
        }
        printf("   %4ux%-4u -> %4ux%-4u srgb=%d  max |dY| %d  max |dC| %d  %s\n", c.sw, c.sh, c.dw, c.dh, c.srgb,
               maxY, maxC, bad ? "BAD" : "ok");
        CHECK(!bad, "YuvConvert %ux%u->%ux%u srgb=%d off by Y %d / C %d / avg %d", c.sw, c.sh, c.dw, c.dh, c.srgb,
              maxY, maxC, maxAvg);
    }

    // Inputs it must refuse rather than mis-handle.
    {
        std::vector<uint32_t> px(64 * 64);
        const YuvConvert::Source src = {px.data(), 64, 64, 64};
        std::vector<uint8_t> buf(YuvConvert::BandBytes(128, 128));
        YuvConvert::Scratch s;
        CHECK(!YuvConvert::ConvertBand(src, {128, 64, false}, 0, 64,
                                       YuvConvert::LayoutBand(buf.data(), 128, 64), s),
              "upscale accepted");
        CHECK(!YuvConvert::ConvertBand(src, {32, 32, false}, 1, 8, YuvConvert::LayoutBand(buf.data(), 32, 8), s),
              "odd y0 accepted");
        CHECK(!YuvConvert::ConvertBand(src, {32, 32, false}, 0, 34, YuvConvert::LayoutBand(buf.data(), 32, 34), s),
              "y1 past the image accepted");
        YuvConvert::FreeScratch(s);
    }
}

// =============================================================================
// Unit: JpegStitch with real libjpeg-turbo bands
// =============================================================================

uint32_t bandLayout(uint32_t h, uint32_t &count) {
    if (count <= 1 || h < 32) {
        count = 1;
        return h;
    }
    uint32_t bh = (((h + count - 1) / count) + 15) & ~15u;
    while (count > 1 && (count - 1) * bh >= h) {
        count--;
        bh = (((h + count - 1) / count) + 15) & ~15u;
    }
    return count == 1 ? h : bh;
}

void testJpegStitch() {
    printf("== JpegStitch vs one-shot encode\n");
    tjhandle ctj = makeCompressor();
    tjhandle dtj = tj3Init(TJINIT_DECOMPRESS);
    struct Case {
        uint32_t w, h, count;
        int quality;
    } cases[] = {{640, 360, 2, 55}, {640, 360, 3, 55}, {1280, 720, 2, 55}, {1280, 720, 3, 80},
                 {1280, 720, 8, 30}, {854, 480, 3, 55}, {320, 180, 2, 55},  {853, 479, 3, 55},
                 {100, 50, 2, 90},  {426, 240, 3, 10}};
    for (const Case &c : cases) {
        const uint32_t pitch = (c.w + 63) & ~63u;
        std::vector<uint32_t> px((size_t) pitch * c.h);
        fakes::fillFrame(px.data(), c.w, c.h, pitch, 99);
        const YuvConvert::Source src = {px.data(), pitch, c.w, c.h};
        const YuvConvert::Target dst = {c.w, c.h, false};
        std::vector<uint8_t> planes(YuvConvert::BandBytes(c.w, c.h));
        const YuvConvert::BandPlanes p = YuvConvert::LayoutBand(planes.data(), c.w, c.h);
        YuvConvert::Scratch scratch;
        YuvConvert::ConvertBand(src, dst, 0, c.h, p, scratch);
        YuvConvert::FreeScratch(scratch);
        tj3Set(ctj, TJPARAM_QUALITY, c.quality);

        auto encode = [&](uint32_t y0, uint32_t rows, std::vector<uint8_t> &out) {
            const unsigned char *pl[3] = {p.y + (size_t) y0 * p.strideY, p.cb + (size_t) (y0 / 2) * p.strideC,
                                          p.cr + (size_t) (y0 / 2) * p.strideC};
            const int st[3]            = {(int) p.strideY, (int) p.strideC, (int) p.strideC};
            unsigned char *buf         = nullptr;
            size_t size                = 0;
            const bool ok = tj3CompressFromYUVPlanes8(ctj, pl, (int) c.w, st, (int) rows, &buf, &size) == 0;
            if (ok) {
                out.assign(buf, buf + size);
            }
            tj3Free(buf);
            return ok;
        };

        std::vector<uint8_t> whole;
        CHECK(encode(0, c.h, whole), "one-shot encode failed");
        Image ref;
        CHECK(decode(dtj, whole.data(), whole.size(), ref), "one-shot decode failed");

        uint32_t count     = c.count;
        const uint32_t bh  = bandLayout(c.h, count);
        std::vector<std::vector<uint8_t>> bands(count);
        const uint8_t *ptrs[JpegStitch::MAX_BANDS];
        uint32_t sizes[JpegStitch::MAX_BANDS];
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t y0 = i * bh;
            const uint32_t y1 = (i + 1 == count) ? c.h : y0 + bh;
            CHECK(encode(y0, y1 - y0, bands[i]), "band encode failed");
            ptrs[i]  = bands[i].data();
            sizes[i] = (uint32_t) bands[i].size();
        }
        if (count < 2) {
            printf("   %4ux%-4u x%u: collapses to one band (as the encoder would)\n", c.w, c.h, c.count);
            continue;
        }
        const uint32_t need = JpegStitch::SplicedSize(ptrs, sizes, count);
        std::vector<uint8_t> out(need);
        uint32_t outSize = 0;
        const bool ok    = need > 0 && JpegStitch::Splice(ptrs, sizes, count, c.w, c.h, bh, out.data(),
                                                          (uint32_t) out.size(), outSize);
        CHECK(ok && outSize == need, "%ux%u x%u: splice failed", c.w, c.h, count);
        Image got;
        const bool dok = ok && decode(dtj, out.data(), outSize, got);
        CHECK(dok, "%ux%u x%u: spliced JPEG does not decode: %s", c.w, c.h, count, tj3GetErrorStr(dtj));
        const bool same = dok && got.w == ref.w && got.h == ref.h && got.rgb == ref.rgb;
        printf("   %4ux%-4u x%u q%-2d: %u bytes spliced vs %zu one-shot - %s\n", c.w, c.h, count, c.quality, outSize,
               whole.size(), same ? "bit-exact" : "MISMATCH");
        CHECK(same, "%ux%u x%u: spliced pixels differ from the one-shot encode", c.w, c.h, count);

        // Rejections.
        uint32_t dummy;
        CHECK(!JpegStitch::Splice(ptrs, sizes, count, c.w, c.h, bh + 8, out.data(), (uint32_t) out.size(), dummy),
              "band height that is not a multiple of 16 accepted");
        CHECK(!JpegStitch::Splice(ptrs, sizes, count, c.w, c.h, bh, out.data(), need - 1, dummy),
              "too-small output buffer accepted");
        uint32_t cut[JpegStitch::MAX_BANDS];
        memcpy(cut, sizes, sizeof(cut));
        cut[count - 1] -= 3; // truncated: no EOI
        CHECK(JpegStitch::SplicedSize(ptrs, cut, count) == 0, "truncated band accepted");
    }

    // Bands we must never splice: progressive, and ones with their own restart markers.
    {
        const uint32_t w = 64, h = 64, pitch = 64;
        std::vector<uint32_t> px(pitch * h);
        fakes::fillFrame(px.data(), w, h, pitch, 5);
        std::vector<uint8_t> planes(YuvConvert::BandBytes(w, h));
        const YuvConvert::BandPlanes p = YuvConvert::LayoutBand(planes.data(), w, h);
        YuvConvert::Scratch s;
        YuvConvert::ConvertBand({px.data(), pitch, w, h}, {w, h, false}, 0, h, p, s);
        YuvConvert::FreeScratch(s);
        auto enc = [&](int param, std::vector<uint8_t> &out) {
            tjhandle t = makeCompressor();
            tj3Set(t, TJPARAM_QUALITY, 50);
            tj3Set(t, param, 1);
            const unsigned char *pl[3] = {p.y, p.cb, p.cr};
            const int st[3]            = {(int) p.strideY, (int) p.strideC, (int) p.strideC};
            unsigned char *buf         = nullptr;
            size_t size                = 0;
            tj3CompressFromYUVPlanes8(t, pl, (int) w, st, 32, &buf, &size);
            out.assign(buf, buf + size);
            tj3Free(buf);
            tj3Destroy(t);
        };
        std::vector<uint8_t> prog, rst, plain;
        enc(TJPARAM_PROGRESSIVE, prog);
        enc(TJPARAM_RESTARTROWS, rst);
        enc(TJPARAM_FASTDCT, plain);
        const uint8_t *a[2] = {plain.data(), prog.data()};
        uint32_t as[2]      = {(uint32_t) plain.size(), (uint32_t) prog.size()};
        CHECK(JpegStitch::SplicedSize(a, as, 2) == 0, "progressive band accepted");
        const uint8_t *b[2] = {plain.data(), rst.data()};
        uint32_t bs[2]      = {(uint32_t) plain.size(), (uint32_t) rst.size()};
        CHECK(JpegStitch::SplicedSize(b, bs, 2) == 0, "band with its own restart markers accepted");
    }
    tj3Destroy(ctj);
    tj3Destroy(dtj);
}

// =============================================================================
// Integration: the real encoder threads
// =============================================================================

struct Stats {
    std::atomic<uint32_t> verified{0};
    std::atomic<uint32_t> multiBand{0};
    std::atomic<uint32_t> safe{0};
    std::atomic<uint32_t> bad{0};
};
Stats gStats;

std::mutex gQMutex;
std::condition_variable gQCv;
std::deque<fakes::Submitted> gQueue;
std::atomic<bool> gVerifying{true};
std::atomic<int> gInFlight{0};

void onSubmit(fakes::Submitted &&f) {
    std::lock_guard<std::mutex> l(gQMutex);
    gQueue.push_back(std::move(f));
    gInFlight++;
    gQCv.notify_one();
}

void verifyOne(tjhandle ctj, tjhandle dtj, const fakes::Submitted &f) {
    const uint32_t sw = fakes::sourceWidth(), sh = fakes::sourceHeight();
    uint32_t dw, dh;
    expectedSize(f.captureSize, sw, sh, dw, dh);

    Image got;
    if (!decode(dtj, f.jpeg.data(), f.jpeg.size(), got)) {
        printf("FAIL frame %u: does not decode (%s)\n", f.frameId, tj3GetErrorStr(dtj));
        gStats.bad++;
        return;
    }
    if (got.w != dw || got.h != dh || f.meta.width != dw || f.meta.height != dh) {
        printf("FAIL frame %u: %ux%u (header %ux%u), expected %ux%u\n", f.frameId, got.w, got.h, f.meta.width,
               f.meta.height, dw, dh);
        gStats.bad++;
        return;
    }

    if (f.path == WUPS_STREAMING_PATH_FAST) {
        std::vector<uint8_t> refJpeg;
        Image ref;
        if (!referenceFast(ctj, f.frameId, sw, sh, dw, dh, f.srgb, f.quality, refJpeg) ||
            !decode(dtj, refJpeg.data(), refJpeg.size(), ref)) {
            printf("FAIL frame %u: reference encode failed\n", f.frameId);
            gStats.bad++;
            return;
        }
        if (got.rgb != ref.rgb) {
            size_t diffs = 0;
            int maxd     = 0;
            for (size_t i = 0; i < got.rgb.size(); i++) {
                const int d = abs((int) got.rgb[i] - (int) ref.rgb[i]);
                diffs += d != 0;
                maxd = std::max(maxd, d);
            }
            printf("FAIL frame %u (%s): differs from a single-core encode of the same frame: %zu samples, max %d\n",
                   f.frameId, hasRestartInterval(f.jpeg) ? "spliced" : "one band", diffs, maxd);
            gStats.bad++;
            return;
        }
        if (hasRestartInterval(f.jpeg)) {
            gStats.multiBand++;
        }
    } else {
        gStats.safe++;
    }

    // Whatever the path, the picture must actually look like the frame. (The SAFE
    // path hands a native, non-sRGB surface to libjpeg as raw bytes; on this
    // little-endian host those bytes are in the wrong order, so skip that one.)
    const bool rawBytes = f.path == WUPS_STREAMING_PATH_SAFE && dw == sw && dh == sh && !f.srgb;
    if (!rawBytes) {
        const double q = psnr(got, idealDownscale(f.frameId, sw, sh, dw, dh, f.srgb));
        if (q < 22.0) {
            printf("FAIL frame %u: PSNR %.1f dB against the ideal downscale\n", f.frameId, q);
            gStats.bad++;
            return;
        }
    }
    gStats.verified++;
}

void verifierLoop() {
    tjhandle ctj = makeCompressor();
    tjhandle dtj = tj3Init(TJINIT_DECOMPRESS);
    for (;;) {
        fakes::Submitted f;
        {
            std::unique_lock<std::mutex> l(gQMutex);
            gQCv.wait(l, [] { return !gQueue.empty() || !gVerifying; });
            if (gQueue.empty()) {
                break;
            }
            f = std::move(gQueue.front());
            gQueue.pop_front();
        }
        verifyOne(ctj, dtj, f);
        gInFlight--;
    }
    tj3Destroy(ctj);
    tj3Destroy(dtj);
}

void drainVerifier() {
    while (gInFlight.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/** Stop() with a watchdog: a hang here is a console freeze on hardware. */
void stopEncoder(const char *where) {
    std::atomic<bool> done{false};
    std::thread dog([&] {
        for (int i = 0; i < 1500 && !done; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!done) {
            printf("FAIL: ImageEncoder::Stop() did not return within 15 s (%s) - deadlock\n", where);
            fflush(stdout);
            std::_Exit(2);
        }
    });
    ImageEncoder::Stop();
    done = true;
    dog.join();
}

void waitForFrames(uint32_t target, int timeoutMs) {
    const uint32_t start = fakes::submittedCount();
    for (int t = 0; t < timeoutMs && fakes::submittedCount() - start < target; t += 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void runFor(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct Scenario {
    const char *name;
    uint32_t sw, sh;
    bool srgb;
    int32_t cores, size, path;
};

void beginScenario(const Scenario &s) {
    printf("== %s\n", s.name);
    gEncoderCores = s.cores;
    gCaptureSize  = s.size;
    gEncodePath   = s.path;
    gQuality      = 55;
    gColorMode    = WUPS_STREAMING_COLOR_AUTO;
    fakes::captureSetup(s.sw, s.sh, s.srgb);
    testhooks::resetLogCounts();
    gStats.verified = gStats.multiBand = gStats.safe = gStats.bad = 0;
}

void endScenario(const char *name) {
    drainVerifier();
    fakes::captureTeardown();
    printf("   %s: %u frames verified (%u spliced from several cores, %u safe path), %u bad\n", name,
           gStats.verified.load(), gStats.multiBand.load(), gStats.safe.load(), gStats.bad.load());
    CHECK(gStats.bad == 0, "%s: %u frames failed verification", name, gStats.bad.load());
    CHECK(gStats.verified > 0, "%s: no frames verified", name);
}

void scenarioSteady(const Scenario &s, bool requireMultiBand) {
    beginScenario(s);
    CHECK(ImageEncoder::Start(), "Start failed");
    fakes::producerStart(0);
    // Long enough for a follower's first trial band and then plenty of frames.
    for (int i = 0; i < 15 && gStats.multiBand < 30; i++) {
        waitForFrames(10, 4000);
    }
    fakes::producerStop();
    stopEncoder(s.name);
    drainVerifier();
    if (requireMultiBand) {
        CHECK(gStats.multiBand > 0, "%s: no frame was ever split across cores", s.name);
    } else if (gStats.multiBand == 0) {
        warn("no multi-core frames in a scenario that allows them");
    }
    endScenario(s.name);
}

void scenarioStarved() {
    const Scenario s = {"core 2 starved by the game, then freed", 1280, 720, false, WUPS_STREAMING_CORES_0_2,
                        WUPS_STREAMING_SIZE_360P, WUPS_STREAMING_PATH_FAST};
    beginScenario(s);
    CHECK(ImageEncoder::Start(), "Start failed");
    fakes::producerStart(2000);
    runFor(1500); // let core 2 join first
    testhooks::setStarve(2, 70, 40, 160);
    runFor(9000);
    const int benched = testhooks::logCount("Benching core");
    testhooks::clearStarve();
    runFor(12000);
    const int rejoined = testhooks::logCount("rejoins");
    fakes::producerStop();
    stopEncoder(s.name);
    printf("   bench events while starved: %d, rejoin events overall: %d\n", benched, rejoined);
    CHECK(benched >= 1, "a starved core was never benched");
    if (rejoined < 2) {
        warn("core 2 did not rejoin after the starvation ended (timing-dependent on a shared CI runner)");
    }
    endScenario(s.name);
}

void scenarioChurn() {
    printf("== stop/start churn while frames keep arriving\n");
    gQuality   = 55;
    gColorMode = WUPS_STREAMING_COLOR_AUTO;
    fakes::captureSetup(1280, 720, true);
    testhooks::resetLogCounts();
    gStats.verified = gStats.multiBand = gStats.safe = gStats.bad = 0;
    fakes::producerStart(1000);
    std::mt19937 rng(42);
    const int32_t sizes[] = {WUPS_STREAMING_SIZE_NATIVE, WUPS_STREAMING_SIZE_480P, WUPS_STREAMING_SIZE_360P,
                             WUPS_STREAMING_SIZE_240P, WUPS_STREAMING_SIZE_180P};
    for (int cycle = 0; cycle < 30; cycle++) {
        gEncoderCores = (int32_t) (rng() % (WUPS_STREAMING_CORES_LAST + 1));
        gEncodePath   = (rng() % 4 == 0) ? WUPS_STREAMING_PATH_SAFE : WUPS_STREAMING_PATH_FAST;
        gCaptureSize  = sizes[rng() % 5];
        if (rng() % 3 == 0) {
            testhooks::setStarve(1 + (int) (rng() % 2), 50, 20, 250);
        } else {
            testhooks::clearStarve();
        }
        CHECK(ImageEncoder::Start(), "Start failed in cycle %d", cycle);
        // Settings are read per frame, so make sure the queue has moved on to
        // frames encoded under this cycle's settings before checking them.
        runFor(150 + (int) (rng() % 400));
        stopEncoder("churn");
        drainVerifier();
    }
    testhooks::clearStarve();
    fakes::producerStop();
    endScenario("churn");
}

void scenarioGpuFallback() {
    const Scenario s = {"GPU copies never retire -> blocking fallback", 640, 360, false, WUPS_STREAMING_CORES_0,
                        WUPS_STREAMING_SIZE_NATIVE, WUPS_STREAMING_PATH_FAST};
    beginScenario(s);
    gGpuSyncFallback = false;
    fakes::failGpu(3);
    CHECK(ImageEncoder::Start(), "Start failed");
    fakes::producerStart(1000);
    waitForFrames(20, 10000);
    fakes::producerStop();
    stopEncoder(s.name);
    CHECK(gGpuSyncFallback, "three GPU timeouts did not switch to blocking sync");
    CHECK(testhooks::logCount("falling back to blocking") == 1, "fallback logged %d times",
          testhooks::logCount("falling back to blocking"));
    gGpuSyncFallback = false;
    endScenario(s.name);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    YuvConvert::Init();

    testYuvConvert();
    testJpegStitch();

    gClientConnected = true;
    gHasForeground   = true;
    fakes::setSubmitHandler(onSubmit);
    std::thread verifier(verifierLoop);

    const Scenario two360   = {"cores 0+2, 1280x720 -> 640x360 (exact 2:1)", 1280, 720, false,
                               WUPS_STREAMING_CORES_0_2, WUPS_STREAMING_SIZE_360P, WUPS_STREAMING_PATH_FAST};
    const Scenario all480   = {"all three cores, 1280x720 -> 854x480 (general ratio), sRGB", 1280, 720, true,
                               WUPS_STREAMING_CORES_ALL, WUPS_STREAMING_SIZE_480P, WUPS_STREAMING_PATH_FAST};
    const Scenario native   = {"cores 0+2, native 1280x720", 1280, 720, false, WUPS_STREAMING_CORES_0_2,
                               WUPS_STREAMING_SIZE_NATIVE, WUPS_STREAMING_PATH_FAST};
    const Scenario odd      = {"cores 0+2, odd native 853x479", 853, 479, false, WUPS_STREAMING_CORES_0_2,
                               WUPS_STREAMING_SIZE_NATIVE, WUPS_STREAMING_PATH_FAST};
    const Scenario tiny     = {"all three cores, 64x24 (too short to split)", 64, 24, false,
                               WUPS_STREAMING_CORES_ALL, WUPS_STREAMING_SIZE_NATIVE, WUPS_STREAMING_PATH_FAST};
    const Scenario safe360  = {"SAFE path, 1280x720 -> 640x360, sRGB", 1280, 720, true, WUPS_STREAMING_CORES_0_2,
                               WUPS_STREAMING_SIZE_360P, WUPS_STREAMING_PATH_SAFE};

    scenarioSteady(two360, true);
    scenarioSteady(all480, false);
    scenarioSteady(native, false);
    scenarioSteady(odd, false);
    scenarioSteady(tiny, false);
    scenarioSteady(safe360, false);
    scenarioStarved();
    scenarioGpuFallback();
    scenarioChurn();

    gVerifying = false;
    gQCv.notify_all();
    verifier.join();

    CHECK(fakes::captureErrors() == 0, "%d capture-slot ownership errors", fakes::captureErrors());
    CHECK(testhooks::badCacheOps() == 0, "%d cache operations on partial lines", testhooks::badCacheOps());
    CHECK(testhooks::logCount("##ERROR##") == 0, "the encoder logged %d errors", testhooks::logCount("##ERROR##"));

    printf("\n%s: %d failure(s), %d warning(s)\n", gFailures ? "FAILED" : "PASSED", gFailures, gWarnings);
    return gFailures ? 1 : 0;
}
