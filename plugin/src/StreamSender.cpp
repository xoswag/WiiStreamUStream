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
#include <coreinit/fastmutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

namespace StreamSender {
namespace {

OSFastMutex sMutex;

int sSocket = -1;
uint32_t sFrameId = 0;
uint32_t sSendFailures = 0;
uint32_t sFramesSent = 0;
uint64_t sBytesSent = 0;     // frame payload only
uint64_t sWireBytesSent = 0; // payload + headers actually put on the socket
crc32_t sCrc;

uint8_t sPacket[STREAM_HEADER_SIZE + STREAM_MAX_PAYLOAD];

/**
 * 4 MB of kernel-side send buffer. A 720p frame is ~100 datagrams pushed back
 * to back with no pacing; with the default buffer the stack starts refusing
 * them partway through every frame.
 */
constexpr int WANTED_SEND_BUFFER = 4 * 1024 * 1024;

constexpr int MAX_SEND_RETRIES = 200;

void closeLocked() {
    if (sSocket >= 0) {
        close(sSocket);
        sSocket = -1;
    }
}

/** Sends one datagram whole, retrying while the stack is congested. */
bool sendDatagram(const uint8_t *data, uint32_t length) {
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
        // Congested rather than broken - let the stack drain and try again.
        OSSleepTicks(OSMicrosecondsToTicks(500));
    }
    return false;
}

} // namespace

void InitOnce() {
    OSFastMutex_Init(&sMutex, "StreamSender");
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
    sSendFailures = 0;
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

    bool ok = true;
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

        if (!sendDatagram(sPacket, STREAM_HEADER_SIZE + chunkLen)) {
            // Abandon the rest of the frame. The client simply never completes
            // this frame id and moves on at the next one, so a partial send
            // costs one frame instead of desynchronising the stream.
            sSendFailures++;
            ok = false;
            break;
        }
        // Count only datagrams that actually reached the socket, so the bandwidth
        // figure reflects the wire and not what we intended to send.
        sWireBytesSent += STREAM_HEADER_SIZE + chunkLen;

        offset += chunkLen;
    }

    if (ok) {
        sFramesSent++;
        sBytesSent += size;
    }

    OSFastMutex_Unlock(&sMutex);
    return ok;
}

uint32_t GetSendFailures() {
    return sSendFailures;
}

uint32_t GetFramesSent() {
    OSFastMutex_Lock(&sMutex);
    const uint32_t v = sFramesSent;
    OSFastMutex_Unlock(&sMutex);
    return v;
}

uint64_t GetBytesSent() {
    // Under the lock because a 64-bit load is two 32-bit loads on this PPC and
    // SendFrame writes this counter; an unlocked read could tear mid-update and
    // print a nonsense Mbit/s figure in the diagnostics.
    OSFastMutex_Lock(&sMutex);
    const uint64_t v = sBytesSent;
    OSFastMutex_Unlock(&sMutex);
    return v;
}

uint64_t GetWireBytesSent() {
    OSFastMutex_Lock(&sMutex);
    const uint64_t v = sWireBytesSent;
    OSFastMutex_Unlock(&sMutex);
    return v;
}

} // namespace StreamSender
