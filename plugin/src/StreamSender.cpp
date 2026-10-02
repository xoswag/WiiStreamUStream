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
#include "StreamSender.hpp"
#include "StreamProtocol.h"
#include "crc32.h"
#include "utils/logger.h"

#include <arpa/inet.h> // htons
#include <coreinit/cache.h>
#include <coreinit/fastmutex.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// The Wii U socket library's own flag. Not always exposed by the headers, and its
// value is fixed by the OS, so define it if we have to rather than depend on it.
#ifndef SO_NONBLOCK
#define SO_NONBLOCK 0x1016
#endif

namespace StreamSender {
namespace {

OSFastMutex sMutex;      // the socket, and everything a send touches
OSFastMutex sStatsMutex; // the counters below that are wider than 32 bits

int sSocket = -1;
uint32_t sFrameId = 0;
uint32_t sSendFailures = 0;  // __atomic
uint32_t sFramesSent = 0;    // sStatsMutex
uint64_t sBytesSent = 0;     // sStatsMutex; frame payload only
uint64_t sWireBytesSent = 0; // sStatsMutex; payload + headers actually put on the socket
crc32_t sCrc;

// --- sender thread state ---
constexpr uint32_t SENDER_STACK_SIZE = 0x10000;
constexpr int WAKE_QUEUE_SIZE        = 4;

OSThread *sSenderThread = nullptr;
void *sSenderStack      = nullptr;
OSMessageQueue sWakeQueue;
OSMessage sWakeMessages[WAKE_QUEUE_SIZE];
volatile bool sSenderStop = false;

// The pending slot, written by Submit() (encoder thread) and taken by the sender.
OSFastMutex sPendingMutex;
uint8_t *sPendingBuf  = nullptr;
uint32_t sPendingCap  = 0;
uint32_t sPendingSize = 0;
FrameMeta sPendingMeta;
bool sPendingValid = false;

// Owned by the sender thread alone; swapped with the pending buffer under the lock.
uint8_t *sSendingBuf = nullptr;
uint32_t sSendingCap = 0;

uint32_t sSubmitDrops = 0; // __atomic
uint32_t sSendUsTotal = 0; // __atomic, wraps
uint32_t sSendCount   = 0; // __atomic, wraps

uint8_t sPacket[STREAM_HEADER_SIZE + STREAM_MAX_PAYLOAD];

/**
 * 4 MB of kernel-side send buffer. A 720p frame is ~100 datagrams pushed back
 * to back with no pacing; with the default buffer the stack starts refusing
 * them partway through every frame.
 */
constexpr int WANTED_SEND_BUFFER = 4 * 1024 * 1024;

/**
 * Total time SendFrame may spend pushing one frame out.
 *
 * The budget has to be per frame, not per datagram. A 720p frame is ~93
 * datagrams, so a 100ms-per-datagram allowance is a 9-second worst case - and
 * all of it is held under sMutex, which Close() and in turn ImageEncoder::Stop()
 * and the config menu all wait on. Abandoning a frame is cheap (the client just
 * never completes that frame id), so the right behaviour under real congestion
 * is to give up quickly and let the next frame try.
 */
constexpr uint32_t FRAME_SEND_BUDGET_MS = 200;

/** Secondary bound, so one wedged datagram cannot eat the whole frame budget. */
constexpr int MAX_SEND_RETRIES = 20;

void closeLocked() {
    if (sSocket >= 0) {
        close(sSocket);
        sSocket = -1;
    }
}

/** Sends one datagram whole, retrying while the stack is congested. */
bool sendDatagram(const uint8_t *data, uint32_t length, OSTime deadline) {
    for (int attempt = 0; attempt < MAX_SEND_RETRIES; attempt++) {
        const int ret = send(sSocket, data, (int) length, 0);
        if (ret == (int) length) {
            return true;
        }
        if (ret >= 0) {
            // A datagram socket either takes the whole message or none of it.
            // A short count means something is wrong with the socket, and
            // retrying would put a torn packet on the wire.
            DEBUG_FUNCTION_LINE_WARN("Short UDP send: %d of %u bytes", ret, length);
            return false;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) {
            return false;
        }
        if (OSGetSystemTime() >= deadline) {
            return false;
        }
        // Congested rather than broken - let the stack drain and try again.
        OSSleepTicks(OSMicrosecondsToTicks(500));
    }
    return false;
}

int senderEntry(int /*argc*/, const char ** /*argv*/) {
    for (;;) {
        OSMessage msg;
        OSReceiveMessage(&sWakeQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
        if (sSenderStop) {
            break;
        }

        // Take the pending frame by swapping buffers, so the lock is held only for
        // the swap and never for the send itself.
        OSFastMutex_Lock(&sPendingMutex);
        if (!sPendingValid) {
            OSFastMutex_Unlock(&sPendingMutex);
            continue;
        }
        uint8_t *const buf = sPendingBuf;
        const uint32_t cap = sPendingCap;
        sPendingBuf        = sSendingBuf;
        sPendingCap        = sSendingCap;
        sSendingBuf        = buf;
        sSendingCap        = cap;
        const uint32_t size  = sPendingSize;
        const FrameMeta meta = sPendingMeta;
        sPendingValid        = false;
        OSFastMutex_Unlock(&sPendingMutex);

        const OSTime t0 = OSGetSystemTime();
        SendFrame(sSendingBuf, size, meta);
        __atomic_add_fetch(&sSendUsTotal, (uint32_t) OSTicksToMicroseconds(OSGetSystemTime() - t0), __ATOMIC_RELAXED);
        __atomic_add_fetch(&sSendCount, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

} // namespace

void InitOnce() {
    OSFastMutex_Init(&sMutex, "StreamSender");
    OSFastMutex_Init(&sStatsMutex, "StreamSender stats");
    OSFastMutex_Init(&sPendingMutex, "StreamSender pending");
}

bool Open(uint32_t clientIp) {
    OSFastMutex_Lock(&sMutex);

    closeLocked();

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        DEBUG_FUNCTION_LINE_ERR("Failed to create the UDP socket (errno %d)", errno);
        OSFastMutex_Unlock(&sMutex);
        return false;
    }

    // Non-blocking, so a congested stack returns EAGAIN and the deadline in
    // SendFrame governs how long we spend here. On a blocking socket send() can
    // park indefinitely - and because ImageEncoder::Stop() joins this thread, that
    // hang propagates up to freezing the config menu on an encoder-core change.
    //
    // This is setsockopt, not fcntl: the Wii U socket layer does not implement
    // F_SETFL, so fcntl compiles fine and then silently does nothing at runtime.
    int nonBlocking = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &nonBlocking, sizeof(nonBlocking)) < 0) {
        DEBUG_FUNCTION_LINE_WARN("Could not set the socket non-blocking (errno %d); "
                                 "a stalled send may block the encoder",
                                 errno);
    }

    // Hardware rejected a flat 4 MB request with EINVAL and silently kept the
    // default, so walk down until one is accepted rather than giving up.
    static const int kSendBufferSizes[] = {WANTED_SEND_BUFFER, 1024 * 1024, 512 * 1024, 256 * 1024, 128 * 1024};
    for (int size : kSendBufferSizes) {
        int sndBuf = size;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf)) == 0) {
            DEBUG_FUNCTION_LINE("SO_SNDBUF set to %d bytes", size);
            break;
        }
        if (size == kSendBufferSizes[(sizeof(kSendBufferSizes) / sizeof(kSendBufferSizes[0])) - 1]) {
            // Not fatal, just lossier under load.
            DEBUG_FUNCTION_LINE_WARN("Could not raise SO_SNDBUF at any size (errno %d)", errno);
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(STREAM_UDP_PORT);
    addr.sin_addr.s_addr = clientIp;

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        DEBUG_FUNCTION_LINE_ERR("Failed to connect the UDP socket (errno %d)", errno);
        close(fd);
        OSFastMutex_Unlock(&sMutex);
        return false;
    }

    // Initialise everything the send path touches *before* publishing the
    // socket. Upstream returned early when socket() failed, leaving the CRC
    // table and the message queue uninitialised, and then used them anyway.
    crc32_init(&sCrc);
    // sFrameId deliberately keeps counting across reconnects. The client treats
    // frame ids as monotonic and ignores anything not newer than what it has
    // already completed, so restarting at 0 would make it discard every frame of
    // the new session until the counter climbed past the old one.
    __atomic_store_n(&sSendFailures, 0, __ATOMIC_RELAXED);
    sSocket       = fd;

    OSFastMutex_Unlock(&sMutex);
    DEBUG_FUNCTION_LINE("Streaming to %u.%u.%u.%u:%d",
                        (clientIp >> 24) & 0xFF, (clientIp >> 16) & 0xFF,
                        (clientIp >> 8) & 0xFF, clientIp & 0xFF, STREAM_UDP_PORT);
    return true;
}

void Close() {
    OSFastMutex_Lock(&sMutex);
    closeLocked();
    OSFastMutex_Unlock(&sMutex);
}

bool IsOpen() {
    OSFastMutex_Lock(&sMutex);
    const bool open = sSocket >= 0;
    OSFastMutex_Unlock(&sMutex);
    return open;
}

bool SendFrame(const uint8_t *payload, uint32_t size, const FrameMeta &meta) {
    if (payload == nullptr || size == 0) {
        return false;
    }

    OSFastMutex_Lock(&sMutex);

    if (sSocket < 0) {
        OSFastMutex_Unlock(&sMutex);
        return false;
    }

    const uint32_t frameCrc     = crc32_crc(&sCrc, payload, size);
    const uint32_t frameId      = sFrameId++;
    const uint64_t timestampUs  = OSTicksToMicroseconds(OSGetSystemTime());
    const OSTime deadline       = OSGetSystemTime() + OSMillisecondsToTicks(FRAME_SEND_BUDGET_MS);

    bool ok       = true;
    uint64_t wire = 0;
    for (uint32_t offset = 0; offset < size;) {
        const uint32_t remaining = size - offset;
        const uint16_t chunkLen  = (uint16_t) (remaining > STREAM_MAX_PAYLOAD ? STREAM_MAX_PAYLOAD : remaining);

        auto *header            = (StreamPacketHeader *) sPacket;
        header->magic           = STREAM_MAGIC;
        header->frameId         = frameId;
        header->timestampUs     = timestampUs;
        header->frameSize       = size;
        header->chunkOffset     = offset;
        header->frameCrc        = frameCrc;
        header->width           = meta.width;
        header->height          = meta.height;
        header->stride          = meta.stride;
        header->chunkLen        = chunkLen;
        header->flags           = STREAM_FLAG_KEYFRAME | ((offset + chunkLen >= size) ? STREAM_FLAG_LAST : 0);
        header->version         = STREAM_VERSION;
        header->compressionType = meta.compressionType;
        header->pixelFormat     = meta.pixelFormat;
        header->reserved        = 0;

        memcpy(sPacket + STREAM_HEADER_SIZE, payload + offset, chunkLen);

        if (!sendDatagram(sPacket, STREAM_HEADER_SIZE + chunkLen, deadline)) {
            // Abandon the rest of the frame. The client simply never completes
            // this frame id and moves on at the next one, so a partial send
            // costs one frame instead of desynchronising the stream.
            __atomic_add_fetch(&sSendFailures, 1, __ATOMIC_RELAXED);
            ok = false;
            break;
        }
        // Count only datagrams that actually reached the socket, so the bandwidth
        // figure reflects the wire and not what we intended to send.
        wire += STREAM_HEADER_SIZE + chunkLen;

        offset += chunkLen;
    }

    OSFastMutex_Unlock(&sMutex);

    // Statistics live under their own lock. sMutex is held for a whole frame's
    // worth of IPC, and the encoder reads these every few seconds - it must not
    // have to wait for a send to finish to do that.
    OSFastMutex_Lock(&sStatsMutex);
    sWireBytesSent += wire;
    if (ok) {
        sFramesSent++;
        sBytesSent += size;
    }
    OSFastMutex_Unlock(&sStatsMutex);
    return ok;
}

uint32_t GetSendFailures() {
    return __atomic_load_n(&sSendFailures, __ATOMIC_RELAXED);
}

uint32_t GetFramesSent() {
    OSFastMutex_Lock(&sStatsMutex);
    const uint32_t v = sFramesSent;
    OSFastMutex_Unlock(&sStatsMutex);
    return v;
}

uint64_t GetBytesSent() {
    // Locked because a 64-bit load is two 32-bit loads on this PPC; an unlocked
    // read could tear mid-update and print a nonsense Mbit/s figure.
    OSFastMutex_Lock(&sStatsMutex);
    const uint64_t v = sBytesSent;
    OSFastMutex_Unlock(&sStatsMutex);
    return v;
}

uint64_t GetWireBytesSent() {
    OSFastMutex_Lock(&sStatsMutex);
    const uint64_t v = sWireBytesSent;
    OSFastMutex_Unlock(&sStatsMutex);
    return v;
}

uint32_t GetSubmitDrops() {
    return __atomic_load_n(&sSubmitDrops, __ATOMIC_RELAXED);
}

uint32_t GetSendUsTotal() {
    return __atomic_load_n(&sSendUsTotal, __ATOMIC_RELAXED);
}

uint32_t GetSendCount() {
    return __atomic_load_n(&sSendCount, __ATOMIC_RELAXED);
}

// --- Sender thread ------------------------------------------------------------

bool Submit(const uint8_t *payload, uint32_t size, const FrameMeta &meta) {
    if (payload == nullptr || size == 0) {
        return false;
    }

    OSFastMutex_Lock(&sPendingMutex);
    if (sPendingCap < size) {
        // Grow in 64 KB steps: frames vary a little in size from one to the next,
        // and reallocating on every slightly-larger frame would be pointless churn.
        const uint32_t cap = (size + 0xFFFF) & ~0xFFFFu;
        auto *grown        = (uint8_t *) realloc(sPendingBuf, cap);
        if (grown == nullptr) {
            OSFastMutex_Unlock(&sPendingMutex);
            DEBUG_FUNCTION_LINE_ERR("Failed to grow the send buffer to %u bytes", cap);
            return false;
        }
        sPendingBuf = grown;
        sPendingCap = cap;
    }
    if (sPendingValid) {
        // Latest wins: the frame still waiting is now stale. Replacing it keeps
        // latency flat when the link cannot keep up with the encoder.
        __atomic_add_fetch(&sSubmitDrops, 1, __ATOMIC_RELAXED);
    }
    memcpy(sPendingBuf, payload, size);
    sPendingSize  = size;
    sPendingMeta  = meta;
    sPendingValid = true;
    OSFastMutex_Unlock(&sPendingMutex);

    // A full queue just means a wake-up is already pending; the sender will pick
    // up whatever is newest when it gets there.
    OSMessage wake;
    memset(&wake, 0, sizeof(wake));
    OSSendMessage(&sWakeQueue, &wake, OS_MESSAGE_FLAGS_NONE);
    return true;
}

bool StartThread(int core, int priority) {
    if (sSenderThread != nullptr) {
        return true;
    }
    if (core < 0 || core > 2) {
        core = 0;
    }

    OSInitMessageQueue(&sWakeQueue, sWakeMessages, WAKE_QUEUE_SIZE);
    sSenderStop = false;

    sSenderThread = (OSThread *) memalign(8, sizeof(OSThread));
    sSenderStack  = memalign(0x20, SENDER_STACK_SIZE);
    if (sSenderThread == nullptr || sSenderStack == nullptr) {
        free(sSenderThread);
        free(sSenderStack);
        sSenderThread = nullptr;
        sSenderStack  = nullptr;
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate the sender thread");
        return false;
    }
    memset(sSenderThread, 0, sizeof(OSThread));

    if (!OSCreateThread(sSenderThread, senderEntry, 0, nullptr,
                        (char *) sSenderStack + SENDER_STACK_SIZE, SENDER_STACK_SIZE,
                        priority, (OSThreadAttributes) (1 << core))) {
        free(sSenderThread);
        free(sSenderStack);
        sSenderThread = nullptr;
        sSenderStack  = nullptr;
        DEBUG_FUNCTION_LINE_ERR("Failed to create the sender thread");
        return false;
    }
    OSSetThreadName(sSenderThread, "WiiStreamUStream sender");
    OSResumeThread(sSenderThread);
    return true;
}

void StopThread() {
    if (sSenderThread == nullptr) {
        return;
    }
    sSenderStop = true;
    OSMemoryBarrier();
    // Blocking so the wake-up is guaranteed to arrive even if the queue is full;
    // the sender is the one draining it.
    OSMessage wake;
    memset(&wake, 0, sizeof(wake));
    OSSendMessage(&sWakeQueue, &wake, OS_MESSAGE_FLAGS_BLOCKING);

    int result = 0;
    OSJoinThread(sSenderThread, &result);
    free(sSenderStack);
    free(sSenderThread);
    sSenderStack  = nullptr;
    sSenderThread = nullptr;

    OSFastMutex_Lock(&sPendingMutex);
    free(sPendingBuf);
    free(sSendingBuf);
    sPendingBuf   = nullptr;
    sSendingBuf   = nullptr;
    sPendingCap   = 0;
    sSendingCap   = 0;
    sPendingSize  = 0;
    sPendingValid = false;
    OSFastMutex_Unlock(&sPendingMutex);
}

} // namespace StreamSender
