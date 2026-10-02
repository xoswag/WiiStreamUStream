#include "StatusReport.hpp"
#include "AudioCapture.hpp"
#include "ImageEncoder.hpp"
#include "ScreenCapture.hpp"
#include "StreamProtocol.h"
#include "StreamSender.hpp"
#include "retain_vars.hpp"

#include <coreinit/time.h>
#include <coreinit/title.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace StatusReport {
namespace {

/** Every counter the perf line is built from, taken at one moment. */
struct Sample {
    bool valid;
    OSTime at;
    uint32_t presented;
    uint32_t captured;
    uint32_t encBusy;
    ImageEncoder::Stats enc;
    uint32_t framesSent;
    uint64_t wireBytes;
    uint32_t replaced;
    uint32_t sendFailures;
    AudioCapture::Stats audio;
};

Sample sPrev = {};

void take(Sample &s) {
    s.valid     = true;
    s.at        = OSGetSystemTime();
    s.presented = ScreenCapture::GetPresentedCount();
    s.captured  = ScreenCapture::GetCapturedCount();
    s.encBusy   = ScreenCapture::GetSkippedCount();
    ImageEncoder::GetStats(s.enc);
    s.framesSent   = StreamSender::GetFramesSent();
    s.wireBytes    = StreamSender::GetWireBytesSent();
    s.replaced     = StreamSender::GetSubmitDrops();
    s.sendFailures = StreamSender::GetSendFailures();
    AudioCapture::GetStats(s.audio);
}

/** vsnprintf that appends and never runs past the buffer, however long the input. */
int appendf(char *buf, int cap, int n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
int appendf(char *buf, int cap, int n, const char *fmt, ...) {
    if (n >= cap - 1) {
        return n;
    }
    va_list ap;
    va_start(ap, fmt);
    const int wrote = vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (wrote < 0) {
        return n;
    }
    return (n + wrote >= cap) ? cap - 1 : n + wrote;
}

const char *sizeName(int32_t v) {
    switch (v) {
        case WUPS_STREAMING_SIZE_720P: return "720p";
        case WUPS_STREAMING_SIZE_480P: return "480p";
        case WUPS_STREAMING_SIZE_360P: return "360p";
        case WUPS_STREAMING_SIZE_240P: return "240p";
        case WUPS_STREAMING_SIZE_180P: return "180p";
        default: return "native";
    }
}

const char *coresName(int32_t v) {
    switch (v) {
        case WUPS_STREAMING_CORES_0: return "0";
        case WUPS_STREAMING_CORES_2: return "2";
        case WUPS_STREAMING_CORES_1: return "1";
        case WUPS_STREAMING_CORES_ALL: return "0+2+1";
        default: return "0+2";
    }
}

/** What kind of title is running, from the upper half of its title ID. */
const char *titleKind(uint64_t titleId) {
    const uint32_t hi = (uint32_t) (titleId >> 32);
    const uint32_t lo = (uint32_t) titleId;
    // The Wii U Menu is 00050010-10040x00 (x = region: 0 JPN, 1 USA, 2 EUR).
    if (hi == 0x00050010 && (lo & 0xFFFFF0FF) == 0x10040000) {
        return "Wii U Menu";
    }
    switch (hi) {
        case 0x00050000: return "game";
        case 0x00050002: return "demo";
        case 0x00050010: return "system app";
        case 0x00050030: return "applet";
        default: return "other title";
    }
}

inline uint32_t perSecond(uint32_t count, uint32_t elapsedMs) {
    return (uint32_t) ((uint64_t) count * 1000 / elapsedMs);
}

} // namespace

void Reset() {
    sPrev.valid = false;
}

void Send() {
    Sample now;
    take(now);

    char text[STREAM_STATUS_MAX_TEXT];
    const int cap = (int) sizeof(text);
    int n         = 0;

    const char *gpu = gGpuSync == WUPS_STREAMING_GPUSYNC_BLOCKING ? "blocking"
                      : gGpuSyncFallback                         ? "async(fell back to blocking)"
                                                                 : "async";
    n = appendf(text, cap, n, "settings res=%s quality=%d skip=%d cores=%s encoder=%s gpu=%s screen=%s colour=%s audio=%s\n",
                sizeName(gCaptureSize), (int) gQuality, (int) gFrameSkip, coresName(gEncoderCores),
                gEncodePath == WUPS_STREAMING_PATH_SAFE ? "safe" : "fast", gpu,
                gScreen == WUPS_STREAMING_SCREEN_DRC ? "gamepad" : "tv",
                gColorMode == WUPS_STREAMING_COLOR_RAW ? "raw" : "auto", gAudioEnabled ? "on" : "off");

    const uint64_t titleId = OSGetTitleID();
    n = appendf(text, cap, n, "state %s %08X-%08X | %s | streaming %ux%u\n", titleKind(titleId),
                (unsigned) (titleId >> 32), (unsigned) titleId,
                gHasForeground ? "in front" : "paused (HOME menu open or title not in front)",
                (unsigned) now.enc.width, (unsigned) now.enc.height);

    if (sPrev.valid) {
        const Sample &p = sPrev;
        uint32_t elapsedMs = (uint32_t) OSTicksToMilliseconds(now.at - p.at);
        if (elapsedMs == 0) {
            elapsedMs = 1;
        }
        const uint32_t encoded  = now.enc.framesEncoded - p.enc.framesEncoded;
        const uint32_t sent     = now.framesSent - p.framesSent;
        const uint64_t wire     = now.wireBytes - p.wireBytes;
        const uint32_t frameUs  = encoded ? (now.enc.frameUsTotal - p.enc.frameUsTotal) / encoded : 0;
        const uint32_t bandsX10 = encoded ? (now.enc.bandsTotal - p.enc.bandsTotal) * 10 / encoded : 0;
        const uint32_t mbitX100 = (uint32_t) (wire * 800 / ((uint64_t) elapsedMs * 1000));
        const uint32_t avgKB    = sent ? (uint32_t) (wire / sent / 1024) : 0;

        // present = the game's own frame rate (the ceiling); enc-busy = frames the
        // encoder had no room for (CPU-bound); send-replaced = frames the network had
        // not sent before a newer one replaced them (network-bound).
        n = appendf(text, cap, n,
                    "perf present %u | capture %u | encode %u | sent %u fps | frame %u.%02u ms, %u.%u bands | "
                    "enc-busy %u/s | dropped %u | gpu-timeouts %u | send-replaced %u | sendfail %u | %u.%02u Mbit/s, %u KB/frame",
                    perSecond(now.presented - p.presented, elapsedMs), perSecond(now.captured - p.captured, elapsedMs),
                    perSecond(encoded, elapsedMs), perSecond(sent, elapsedMs), frameUs / 1000, (frameUs % 1000) / 10,
                    bandsX10 / 10, bandsX10 % 10, perSecond(now.encBusy - p.encBusy, elapsedMs),
                    now.enc.framesDropped - p.enc.framesDropped, now.enc.gpuTimeouts - p.enc.gpuTimeouts,
                    now.replaced - p.replaced, now.sendFailures - p.sendFailures, mbitX100 / 100, mbitX100 % 100,
                    avgKB);

        for (int i = 0; i < now.enc.coreCount && i < ImageEncoder::MAX_ENCODER_CORES; i++) {
            if (i == 0) {
                n = appendf(text, cap, n, " | core%d leads", (int) now.enc.core[i]);
            } else if (now.enc.benched[i]) {
                n = appendf(text, cap, n, ", core%d benched", (int) now.enc.core[i]);
            } else {
                const uint32_t us = now.enc.bandLatencyUs[i];
                n = appendf(text, cap, n, ", core%d helps (%u.%02u ms/band)", (int) now.enc.core[i], us / 1000,
                            (us % 1000) / 10);
            }
        }

        if (!gAudioEnabled) {
            n = appendf(text, cap, n, " | audio off");
        } else if (!now.audio.hooked) {
            n = appendf(text, cap, n, " | audio not available in this title");
        } else {
            const uint32_t kbit = (uint32_t) ((uint64_t) (now.audio.bytesSent - p.audio.bytesSent) * 8 / elapsedMs);
            n = appendf(text, cap, n, " | audio %u Hz, %u kbit/s, %u ms dropped", (unsigned) now.audio.sampleRate,
                        kbit,
                        now.audio.sampleRate
                                ? (now.audio.framesDropped - p.audio.framesDropped) * 1000 / now.audio.sampleRate
                                : 0);
        }
        n = appendf(text, cap, n, "\n");
    }
    sPrev = now;

    uint8_t packet[STREAM_STATUS_HEADER_SIZE + STREAM_STATUS_MAX_TEXT];
    auto *h    = (StreamStatusHeader *) packet;
    h->magic   = STREAM_STATUS_MAGIC;
    h->version = STREAM_STATUS_VERSION;
    h->kind    = 0;
    h->length  = (uint16_t) n;
    memcpy(packet + STREAM_STATUS_HEADER_SIZE, text, n);
    StreamSender::SendSide(packet, STREAM_STATUS_HEADER_SIZE + n);
}

} // namespace StatusReport
