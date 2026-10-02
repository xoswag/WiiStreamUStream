// Fake ScreenCapture and StreamSender for the host tests.
//
// The capture fake behaves like the real one from the encoder's side: a fixed
// set of slots, a ready queue with room for a stop sentinel, frames handed over
// in order. It also polices slot ownership, so a double release or a slot the
// encoder never gives back is reported instead of silently tolerated.
#include "test_hooks.hpp"

#include "ScreenCapture.hpp"
#include "StreamProtocol.h"
#include "StreamSender.hpp"
#include "retain_vars.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace {

enum class SlotState { Free, Filling, Ready, Taken };

std::mutex sMutex;
std::condition_variable sCv;
CaptureSlot sSlots[CAPTURE_SLOT_COUNT];
SlotState sState[CAPTURE_SLOT_COUNT];
uint32_t sFrameOf[CAPTURE_SLOT_COUNT];
std::deque<int> sFree;
std::deque<int> sReady; // -1 is the stop sentinel

uint32_t sWidth = 0, sHeight = 0, sPitch = 0;
bool sSrgb = false;

std::thread sProducer;
std::atomic<bool> sProducing{false};
std::atomic<uint32_t> sNextFrame{1};
std::atomic<uint32_t> sProduced{0};
std::atomic<uint32_t> sLastReleased{0};
std::atomic<int> sGpuFailures{0};
std::atomic<int> sErrors{0};
std::atomic<uint32_t> sPresented{0}, sCaptured{0};

fakes::SubmitHandler sHandler = nullptr;
std::atomic<uint32_t> sSubmitted{0};
std::atomic<uint64_t> sBytes{0}, sWire{0};

int slotIndex(const CaptureSlot *slot) {
    for (int i = 0; i < CAPTURE_SLOT_COUNT; i++) {
        if (&sSlots[i] == slot) {
            return i;
        }
    }
    return -1;
}

uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

void producerLoop(uint32_t pacingUs) {
    while (sProducing) {
        int idx = -1;
        {
            std::unique_lock<std::mutex> l(sMutex);
            sCv.wait_for(l, std::chrono::milliseconds(5), [] { return !sFree.empty() || !sProducing; });
            if (!sProducing) {
                break;
            }
            sPresented++;
            if (sFree.empty()) {
                continue;
            }
            idx = sFree.front();
            sFree.pop_front();
            sState[idx] = SlotState::Filling;
        }
        const uint32_t id = sNextFrame++;
        GX2Surface &s     = sSlots[idx].colorBuffer.surface;
        fakes::fillFrame((uint32_t *) s.image, sWidth, sHeight, sPitch, id);
        sSlots[idx].sourceIsSRGB = sSrgb;
        sSlots[idx].gpuTimestamp = 1;
        {
            std::lock_guard<std::mutex> l(sMutex);
            sFrameOf[idx] = id;
            sState[idx]   = SlotState::Ready;
            sReady.push_back(idx);
        }
        sProduced++;
        sCaptured++;
        sCv.notify_all();
        if (pacingUs) {
            std::this_thread::sleep_for(std::chrono::microseconds(pacingUs));
        }
    }
}

} // namespace

// --- fakes API -------------------------------------------------------------------

namespace fakes {

void fillFrame(uint32_t *pixels, uint32_t width, uint32_t height, uint32_t pitch, uint32_t id) {
    const uint32_t sqW = width / 8 + 1, sqH = height / 8 + 1;
    const uint32_t sqX = (id * 7) % width, sqY = (id * 3) % height;
    for (uint32_t y = 0; y < height; y++) {
        uint32_t *row = pixels + (size_t) y * pitch;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t r = (x * 200 / width + id * 2) & 255;
            uint32_t g = (y * 220 / height + id) & 255;
            uint32_t b = ((x + 2 * y) * 160 / (width + 2 * height) + 40) & 255;
            if (x >= sqX && x < sqX + sqW && y >= sqY && y < sqY + sqH) {
                r = 250;
                g = 30;
                b = 30;
            }
            const uint32_t n = hash3(x, y, id) & 7;
            r                = r + n > 255 ? 255 : r + n;
            g                = g + n > 255 ? 255 : g + n;
            b                = b + n > 255 ? 255 : b + n;
            row[x]           = (r << 24) | (g << 16) | (b << 8) | 0xFF;
        }
        // Pitch padding: poison, so reading past the width shows up as wrong pixels.
        for (uint32_t x = width; x < pitch; x++) {
            row[x] = 0x00FF00FFu;
        }
    }
}

void captureSetup(uint32_t width, uint32_t height, bool srgb) {
    std::lock_guard<std::mutex> l(sMutex);
    sWidth  = width;
    sHeight = height;
    // GX2 pads a linear-aligned 32bpp surface's pitch to a multiple of 64 pixels.
    sPitch = (width + 63) & ~63u;
    sSrgb  = srgb;
    sFree.clear();
    sReady.clear();
    for (int i = 0; i < CAPTURE_SLOT_COUNT; i++) {
        CaptureSlot &slot = sSlots[i];
        memset(&slot, 0, sizeof(slot));
        GX2Surface &s = slot.colorBuffer.surface;
        s.width       = width;
        s.height      = height;
        s.pitch       = sPitch;
        s.format      = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        s.imageSize   = (sPitch * height * 4 + 0x7FF) & ~0x7FFu;
        s.alignment   = 0x800;
        s.image       = aligned_alloc(0x800, s.imageSize);
        memset(s.image, 0, s.imageSize);
        slot.imageCapacity = s.imageSize;
        sState[i]          = SlotState::Free;
        sFree.push_back(i);
    }
}

void captureTeardown() {
    std::lock_guard<std::mutex> l(sMutex);
    for (int i = 0; i < CAPTURE_SLOT_COUNT; i++) {
        if (sState[i] == SlotState::Taken) {
            printf("FAKE-CAPTURE: slot %d still held by the encoder at teardown\n", i);
            sErrors++;
        }
        free(sSlots[i].colorBuffer.surface.image);
        sSlots[i].colorBuffer.surface.image = nullptr;
    }
    sFree.clear();
    sReady.clear();
}

void producerStart(uint32_t pacingUs) {
    sProducing = true;
    sProducer  = std::thread(producerLoop, pacingUs);
}

void producerStop() {
    sProducing = false;
    sCv.notify_all();
    if (sProducer.joinable()) {
        sProducer.join();
    }
}

void failGpu(int count) {
    sGpuFailures = count;
}

uint32_t framesProduced() {
    return sProduced;
}

uint32_t lastReleasedFrame() {
    return sLastReleased;
}

int captureErrors() {
    return sErrors;
}

uint32_t sourceWidth() {
    return sWidth;
}

uint32_t sourceHeight() {
    return sHeight;
}

bool sourceSrgb() {
    return sSrgb;
}

void setSubmitHandler(SubmitHandler handler) {
    sHandler = handler;
}

uint32_t submittedCount() {
    return sSubmitted;
}

} // namespace fakes

// --- ScreenCapture, as the encoder sees it ------------------------------------------

CaptureSlot *ScreenCapture::WaitForFrame() {
    std::unique_lock<std::mutex> l(sMutex);
    sCv.wait(l, [] { return !sReady.empty(); });
    const int idx = sReady.front();
    sReady.pop_front();
    if (idx < 0) {
        return nullptr;
    }
    if (sState[idx] != SlotState::Ready) {
        printf("FAKE-CAPTURE: slot %d handed out while not ready\n", idx);
        sErrors++;
    }
    sState[idx] = SlotState::Taken;
    return &sSlots[idx];
}

bool ScreenCapture::WaitForGpu(const CaptureSlot *slot, uint32_t /*timeoutMs*/) {
    if (slotIndex(slot) < 0) {
        printf("FAKE-CAPTURE: WaitForGpu on an unknown slot\n");
        sErrors++;
    }
    int left = sGpuFailures.load();
    while (left > 0) {
        if (sGpuFailures.compare_exchange_weak(left, left - 1)) {
            return false;
        }
    }
    return true;
}

void ScreenCapture::ReleaseFrame(CaptureSlot *slot) {
    const int idx = slotIndex(slot);
    std::lock_guard<std::mutex> l(sMutex);
    if (idx < 0 || sState[idx] != SlotState::Taken) {
        printf("FAKE-CAPTURE: release of slot %d, which the encoder does not hold\n", idx);
        sErrors++;
        return;
    }
    sState[idx]   = SlotState::Free;
    sLastReleased = sFrameOf[idx];
    sFree.push_back(idx);
    sCv.notify_all();
}

void ScreenCapture::SignalStop() {
    std::lock_guard<std::mutex> l(sMutex);
    sReady.push_back(-1);
    sCv.notify_all();
}

uint32_t ScreenCapture::GetPresentedCount() {
    return sPresented;
}

uint32_t ScreenCapture::GetCapturedCount() {
    return sCaptured;
}

uint32_t ScreenCapture::GetSkippedCount() {
    return 0;
}

void ScreenCapture::ResetCounters() {
    sPresented = 0;
    sCaptured  = 0;
}

// --- StreamSender, as the encoder sees it ---------------------------------------------

namespace StreamSender {

bool Submit(const uint8_t *payload, uint32_t size, const FrameMeta &meta) {
    if (payload == nullptr || size == 0) {
        return false;
    }
    fakes::Submitted f;
    // The encoder releases a frame's slot before submitting it, on the same
    // thread, so the last release is the frame being submitted.
    f.frameId = sLastReleased;
    f.jpeg.assign(payload, payload + size);
    f.meta    = meta;
    f.path    = gEncodePath;
    int q     = gQuality;
    f.quality     = q < STREAM_QUALITY_MIN ? STREAM_QUALITY_MIN : q > STREAM_QUALITY_MAX ? STREAM_QUALITY_MAX : q;
    f.captureSize = gCaptureSize;
    f.srgb        = (gColorMode == WUPS_STREAMING_COLOR_AUTO) && sSrgb;
    sSubmitted++;
    sBytes += size;
    sWire += size + STREAM_HEADER_SIZE * ((size + STREAM_MAX_PAYLOAD - 1) / STREAM_MAX_PAYLOAD);
    if (sHandler != nullptr) {
        sHandler(std::move(f));
    }
    return true;
}

uint32_t GetSendFailures() {
    return 0;
}

uint32_t GetFramesSent() {
    return sSubmitted;
}

uint64_t GetBytesSent() {
    return sBytes;
}

uint64_t GetWireBytesSent() {
    return sWire;
}

uint32_t GetSubmitDrops() {
    return 0;
}

uint32_t GetSendUsTotal() {
    return 0;
}

uint32_t GetSendCount() {
    return sSubmitted;
}

} // namespace StreamSender
