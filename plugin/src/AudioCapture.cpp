#include "AudioCapture.hpp"
#include "Adpcm.hpp"
#include "AudioRing.hpp"
#include "StreamProtocol.h"
#include "StreamSender.hpp"
#include "retain_vars.hpp"
#include "utils/logger.h"

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <malloc.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <string.h>
#include <wups.h>

namespace {

/**
 * What AX hands a final mix callback. wut leaves the argument opaque; this layout
 * is the one the decaf emulator reverse engineered and runs real games' callbacks
 * against: data[device * channels + channel] points at `samples` 32-bit samples
 * holding 16-bit values. TV is 1 device x 6 channels (L, R, SL, SR, C, LFE), the
 * GamePad 2 devices x 4 channels. 144 samples is 3 ms at 48 kHz; 96 is the 32 kHz
 * renderer.
 */
struct FinalMixData {
    int32_t **data;
    uint16_t channels;
    uint16_t samples;
    uint16_t numDevices;
    uint16_t channelsOut;
};
static_assert(sizeof(FinalMixData) == 12, "AXDeviceFinalMixData is 12 bytes on the console");

constexpr uint32_t MAX_MIX_SAMPLES = 288;

constexpr uint32_t BLOCK_FRAMES   = 960; // 20 ms at 48 kHz
constexpr uint32_t MAX_BACKLOG_MS = 120; // more than this queued and we skip to the newest

// Above the encoder (25) and video sender (24) so audio is never starved by our
// own video work, still below a typical game thread (~16).
constexpr int AUDIO_THREAD_PRIORITY = 22;
constexpr int AUDIO_THREAD_CORE     = 0;
constexpr uint32_t STACK_SIZE       = 0x8000;

AudioRing sRing;

AXDeviceFinalMixCallback sGameCallback[2] = {nullptr, nullptr};
volatile bool sInstalled                  = false;

uint32_t sSampleRate    = 0; // __atomic; written by the mixer callback
uint32_t sFramesDropped = 0; // __atomic
uint32_t sPacketsSent   = 0; // __atomic
uint32_t sBytesSent     = 0; // __atomic

OSThread *sThread  = nullptr;
void *sStack       = nullptr;
volatile bool sStop = false;

inline int16_t clamp16(int32_t v) {
    return (int16_t) (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/** -3 dB, the usual fold-down weight for centre and surrounds, in 8.8 fixed point. */
inline int32_t fold(int32_t v) {
    return (int32_t) (((int64_t) v * 181) >> 8);
}

/**
 * Runs inside the console's audio processing every 3 ms. Integer-only, no OS
 * calls, no locks: copy the mix and publish it.
 */
void captureMix(const FinalMixData *d, bool tv) {
    const bool wanted = tv ? (gScreen == WUPS_STREAMING_SCREEN_TV) : (gScreen == WUPS_STREAMING_SCREEN_DRC);
    if (!wanted || !gAudioEnabled || !StreamingActive()) {
        return;
    }
    if (d == nullptr || d->data == nullptr || d->channels < 2 || d->samples == 0 || d->samples > MAX_MIX_SAMPLES) {
        return;
    }

    // First device only: the GamePad mix has one per pad, and they are the same game.
    const int32_t *left  = d->data[0];
    const int32_t *right = d->data[1];
    if (left == nullptr || right == nullptr) {
        return;
    }
    const int32_t *sl = nullptr, *sr = nullptr, *c = nullptr;
    if (tv && d->channels >= 6) {
        sl = d->data[2];
        sr = d->data[3];
        c  = d->data[4];
    }
    const bool surround = sl != nullptr && sr != nullptr && c != nullptr;

    __atomic_store_n(&sSampleRate, d->samples == 96 ? 32000u : 48000u, __ATOMIC_RELAXED);
    if (!sRing.CanWrite(d->samples)) {
        // Our sender is behind; this mix is lost, the ring is not corrupted.
        __atomic_add_fetch(&sFramesDropped, d->samples, __ATOMIC_RELAXED);
        return;
    }
    // Straight into the ring: no buffer on a stack we do not own.
    for (uint32_t i = 0; i < d->samples; i++) {
        int32_t l = left[i];
        int32_t r = right[i];
        if (surround) {
            // In stereo output mode these are silent and this changes nothing; in
            // surround mode it keeps dialogue (centre) and effects audible.
            const int32_t cc = fold(c[i]);
            l += cc + fold(sl[i]);
            r += cc + fold(sr[i]);
        }
        sRing.Put(i, clamp16(l), clamp16(r));
    }
    sRing.Commit(d->samples);
}

void finalMixTV(void *arg) {
    if (const AXDeviceFinalMixCallback game = sGameCallback[AX_DEVICE_TYPE_TV]) {
        game(arg); // the game's own processing first: we stream what the speakers get
    }
    captureMix((const FinalMixData *) arg, true);
}

void finalMixDRC(void *arg) {
    if (const AXDeviceFinalMixCallback game = sGameCallback[AX_DEVICE_TYPE_DRC]) {
        game(arg);
    }
    captureMix((const FinalMixData *) arg, false);
}

bool isWrapped(AXDeviceType type) {
    return sInstalled && (type == AX_DEVICE_TYPE_TV || type == AX_DEVICE_TYPE_DRC);
}

int senderEntry(int /*argc*/, const char ** /*argv*/) {
    Adpcm::State left, right;
    uint32_t sequence   = 0;
    uint32_t firstFrame = 0;

    int16_t pcm[2 * BLOCK_FRAMES];
    uint8_t packet[STREAM_AUDIO_HEADER_SIZE + BLOCK_FRAMES];

    while (!sStop) {
        if (!StreamingActive() || !gAudioEnabled) {
            // Nothing is being captured; forget anything left from before so a
            // resumed stream does not start with stale sound.
            const uint32_t stale = sRing.Available();
            if (stale > 0) {
                sRing.Skip(stale);
            }
            OSSleepTicks(OSMillisecondsToTicks(50));
            continue;
        }
        OSSleepTicks(OSMillisecondsToTicks(5));

        const uint32_t rate = __atomic_load_n(&sSampleRate, __ATOMIC_RELAXED);
        if (rate == 0) {
            continue;
        }

        uint32_t available = sRing.Available();
        const uint32_t maxBacklog = rate * MAX_BACKLOG_MS / 1000;
        if (available > maxBacklog) {
            // We fell behind (the network stalled a send, or this thread was
            // starved): skip to the newest audio rather than build up delay.
            const uint32_t skip = available - BLOCK_FRAMES;
            sRing.Skip(skip);
            firstFrame += skip;
            __atomic_add_fetch(&sFramesDropped, skip, __ATOMIC_RELAXED);
            available = BLOCK_FRAMES;
        }

        while (available >= BLOCK_FRAMES && !sStop) {
            sRing.Read(pcm, BLOCK_FRAMES);
            available -= BLOCK_FRAMES;

            auto *h         = (StreamAudioHeader *) packet;
            h->magic        = STREAM_AUDIO_MAGIC;
            h->version      = STREAM_AUDIO_VERSION;
            h->codec        = STREAM_AUDIO_CODEC_IMA_ADPCM;
            h->channels     = 2;
            h->reserved0    = 0;
            h->sampleRate   = rate;
            h->sequence     = sequence++;
            h->firstFrame   = firstFrame;
            h->frames       = (uint16_t) BLOCK_FRAMES;
            h->reserved1    = 0;
            h->predictor[0] = (int16_t) left.predictor;
            h->predictor[1] = (int16_t) right.predictor;
            h->stepIndex[0] = (uint8_t) left.index;
            h->stepIndex[1] = (uint8_t) right.index;
            h->reserved2    = 0;
            Adpcm::EncodeStereo(pcm, BLOCK_FRAMES, left, right, packet + STREAM_AUDIO_HEADER_SIZE);
            firstFrame += BLOCK_FRAMES;

            const uint32_t size = STREAM_AUDIO_HEADER_SIZE + BLOCK_FRAMES;
            if (StreamSender::SendSide(packet, size)) {
                __atomic_add_fetch(&sPacketsSent, 1, __ATOMIC_RELAXED);
                __atomic_add_fetch(&sBytesSent, size, __ATOMIC_RELAXED);
            }
        }
    }
    return 0;
}

/** Called right after the title initialises AX, on the title's own thread. */
void onAxInit(AXResult (*registerFn)(AXDeviceType, AXDeviceFinalMixCallback)) {
    sGameCallback[AX_DEVICE_TYPE_TV]  = nullptr;
    sGameCallback[AX_DEVICE_TYPE_DRC] = nullptr;
    sInstalled                        = false;
    if (!gAudioEnabled || registerFn == nullptr) {
        return; // "Audio: Off" means nothing of ours goes into the mixer at all
    }
    if (registerFn(AX_DEVICE_TYPE_TV, finalMixTV) == AX_RESULT_SUCCESS &&
        registerFn(AX_DEVICE_TYPE_DRC, finalMixDRC) == AX_RESULT_SUCCESS) {
        sInstalled = true;
        DEBUG_FUNCTION_LINE("Audio capture installed in the mixer");
    } else {
        registerFn(AX_DEVICE_TYPE_TV, nullptr);
        registerFn(AX_DEVICE_TYPE_DRC, nullptr);
        DEBUG_FUNCTION_LINE_WARN("Could not install the audio capture callback");
    }
}

} // namespace

// --- Mixer hooks ------------------------------------------------------------------
//
// Our callback has to stay the one AX calls even if the title registers its own
// later, so registration is intercepted: the title's callback is remembered and
// called from ours, and asking AX for the callback returns the title's, so the
// title never sees the difference.

DECL_FUNCTION(AXResult, AXRegisterDeviceFinalMixCallback, AXDeviceType type, AXDeviceFinalMixCallback func) {
    if (isWrapped(type)) {
        sGameCallback[type] = func;
        return AX_RESULT_SUCCESS;
    }
    return real_AXRegisterDeviceFinalMixCallback(type, func);
}

DECL_FUNCTION(AXResult, AXGetDeviceFinalMixCallback, AXDeviceType type, AXDeviceFinalMixCallback *func) {
    if (isWrapped(type)) {
        if (func != nullptr) {
            *func = sGameCallback[type];
        }
        return AX_RESULT_SUCCESS;
    }
    return real_AXGetDeviceFinalMixCallback(type, func);
}

DECL_FUNCTION(void, AXInit, void) {
    real_AXInit();
    onAxInit(real_AXRegisterDeviceFinalMixCallback);
}

DECL_FUNCTION(void, AXInitWithParams, AXInitParams *params) {
    real_AXInitWithParams(params);
    onAxInit(real_AXRegisterDeviceFinalMixCallback);
}

DECL_FUNCTION(void, AXQuit, void) {
    sInstalled                        = false;
    sGameCallback[AX_DEVICE_TYPE_TV]  = nullptr;
    sGameCallback[AX_DEVICE_TYPE_DRC] = nullptr;
    real_AXQuit();
}

WUPS_MUST_REPLACE(AXRegisterDeviceFinalMixCallback, WUPS_LOADER_LIBRARY_SNDCORE2, AXRegisterDeviceFinalMixCallback);
WUPS_MUST_REPLACE(AXGetDeviceFinalMixCallback, WUPS_LOADER_LIBRARY_SNDCORE2, AXGetDeviceFinalMixCallback);
WUPS_MUST_REPLACE(AXInit, WUPS_LOADER_LIBRARY_SNDCORE2, AXInit);
WUPS_MUST_REPLACE(AXInitWithParams, WUPS_LOADER_LIBRARY_SNDCORE2, AXInitWithParams);
WUPS_MUST_REPLACE(AXQuit, WUPS_LOADER_LIBRARY_SNDCORE2, AXQuit);

// --- Thread lifecycle -----------------------------------------------------------------

namespace AudioCapture {

bool Start() {
    if (sThread != nullptr) {
        return true;
    }
    sStop   = false;
    sThread = (OSThread *) memalign(8, sizeof(OSThread));
    sStack  = memalign(0x20, STACK_SIZE);
    if (sThread == nullptr || sStack == nullptr) {
        free(sThread);
        free(sStack);
        sThread = nullptr;
        sStack  = nullptr;
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate the audio thread");
        return false;
    }
    memset(sThread, 0, sizeof(OSThread));
    if (!OSCreateThread(sThread, senderEntry, 0, nullptr, (char *) sStack + STACK_SIZE, STACK_SIZE,
                        AUDIO_THREAD_PRIORITY, (OSThreadAttributes) (1 << AUDIO_THREAD_CORE))) {
        free(sThread);
        free(sStack);
        sThread = nullptr;
        sStack  = nullptr;
        DEBUG_FUNCTION_LINE_ERR("Failed to create the audio thread");
        return false;
    }
    OSSetThreadName(sThread, "WiiStreamUStream audio");
    OSResumeThread(sThread);
    return true;
}

void Stop() {
    if (sThread == nullptr) {
        return;
    }
    sStop = true;
    int result = 0;
    OSJoinThread(sThread, &result); // it polls the flag at least every 50 ms
    free(sStack);
    free(sThread);
    sStack  = nullptr;
    sThread = nullptr;
}

void GetStats(Stats &out) {
    out.packetsSent   = __atomic_load_n(&sPacketsSent, __ATOMIC_RELAXED);
    out.bytesSent     = __atomic_load_n(&sBytesSent, __ATOMIC_RELAXED);
    out.framesDropped = __atomic_load_n(&sFramesDropped, __ATOMIC_RELAXED);
    out.sampleRate    = __atomic_load_n(&sSampleRate, __ATOMIC_RELAXED);
    out.hooked        = sInstalled;
}

} // namespace AudioCapture
