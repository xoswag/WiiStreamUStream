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
#pragma once

#include <stdint.h>

// Keep in sync with the client's StreamProtocol.java and with PROTOCOL.md.

#define STREAM_MAGIC        0x57555332u // 'W','U','S','2'
#define STREAM_VERSION      2

#define STREAM_HEADER_SIZE  24

// 24 + 1376 = 1400 bytes on the wire, comfortably under the 1472-byte limit
// for a 1500-byte-MTU link, so IP never fragments a datagram.
#define STREAM_MAX_PAYLOAD  1376

#define STREAM_FLAG_LAST    0x01

#define STREAM_TCP_PORT     8092
#define STREAM_UDP_PORT     9445

#define STREAM_PING         0x15
#define STREAM_PONG         0x16

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The 24-byte header in front of every payload chunk.
 *
 * The Wii U is big-endian and Java's ByteBuffer defaults to big-endian, so this
 * struct goes onto the wire exactly as it sits in memory - no byte swapping on
 * either side. WUT_PACKED is deliberate: the natural alignment of the members
 * already produces the intended layout, but making it explicit means a compiler
 * change can never silently insert padding and shift every field.
 */
typedef struct __attribute__((packed)) StreamPacketHeader {
    uint32_t magic;       // STREAM_MAGIC
    uint32_t frameId;     // +1 per frame, free to wrap
    uint32_t frameSize;   // total JPEG bytes
    uint32_t chunkOffset; // byte offset of this chunk within the frame
    uint16_t chunkLen;    // payload bytes in this datagram
    uint8_t flags;        // STREAM_FLAG_*
    uint8_t version;      // STREAM_VERSION
    uint32_t frameCrc;    // CRC-32 of the whole frame, repeated in every chunk
} StreamPacketHeader;

#ifdef __cplusplus
static_assert(sizeof(StreamPacketHeader) == STREAM_HEADER_SIZE,
              "StreamPacketHeader must be exactly 24 bytes on the wire");
#else
_Static_assert(sizeof(StreamPacketHeader) == STREAM_HEADER_SIZE,
               "StreamPacketHeader must be exactly 24 bytes on the wire");
#endif

#ifdef __cplusplus
}
#endif
