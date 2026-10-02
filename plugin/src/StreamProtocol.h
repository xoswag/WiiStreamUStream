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

// Protocol v3 makes every datagram fully self-describing: it now carries a
// timestamp, the frame's width/height/stride, the compression type and the
// pixel format, so the client renders whatever each frame declares instead of
// assuming JPEG at a fixed size. This is what lets the same wire format carry
// JPEG, RAW, or a future lightweight mode without the client having to be told
// which one to expect.
#define STREAM_MAGIC        0x57555333u // 'W','U','S','3'
#define STREAM_VERSION      3

#define STREAM_HEADER_SIZE  44

// 44 + 1376 = 1420 bytes on the wire, still under the 1472-byte limit for a
// 1500-byte-MTU link, so IP never fragments a datagram. Kept identical to v2 so
// the chunk stride and the client's per-chunk seen-set math are unchanged.
#define STREAM_MAX_PAYLOAD  1376

// flags
#define STREAM_FLAG_LAST      0x01 // last chunk of the frame
#define STREAM_FLAG_KEYFRAME  0x02 // frame is self-contained (always set for JPEG/RAW)

// compressionType
#define STREAM_COMP_RAW         0 // payload is raw pixels (see pixelFormat)
#define STREAM_COMP_LIGHTWEIGHT 1 // reserved for a future cheap codec
#define STREAM_COMP_JPEG        2 // payload is a complete JPEG/JFIF image

// pixelFormat (meaningful for RAW/LIGHTWEIGHT; JPEG carries its own)
#define STREAM_PIXFMT_JPEG      0 // not applicable - the JPEG describes itself
#define STREAM_PIXFMT_RGB888    1 // 3 bytes/pixel, tightly packed
#define STREAM_PIXFMT_RGBA8888  2 // 4 bytes/pixel

#define STREAM_TCP_PORT     8092
#define STREAM_UDP_PORT     9445

#define STREAM_PING         0x15
#define STREAM_PONG         0x16

// --- Side channel: audio and status --------------------------------------------
// Same UDP port as video, told apart by magic. A client that predates them sees
// an unknown magic and ignores the datagram.

#define STREAM_AUDIO_MAGIC           0x57555341u // 'W','U','S','A'
#define STREAM_AUDIO_VERSION         1
#define STREAM_AUDIO_HEADER_SIZE     32
#define STREAM_AUDIO_CODEC_IMA_ADPCM 1 // 4 bits/sample; one byte per stereo frame, left in the low nibble

#define STREAM_STATUS_MAGIC          0x57555353u // 'W','U','S','S'
#define STREAM_STATUS_VERSION        1
#define STREAM_STATUS_HEADER_SIZE    8
// Keeps a status datagram inside the client's receive buffer (header + 1376).
#define STREAM_STATUS_MAX_TEXT       1200

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The 44-byte header in front of every payload chunk.
 *
 * The Wii U is big-endian and Java's ByteBuffer defaults to big-endian, so this
 * struct goes onto the wire exactly as it sits in memory - no byte swapping on
 * either side. packed is deliberate: the natural alignment of the members
 * already produces the intended layout, but making it explicit means a compiler
 * change can never silently insert padding and shift every field.
 *
 * Fields that describe the whole frame (frameSize, frameCrc, timestampUs, width,
 * height, stride, compressionType, pixelFormat) are repeated identically in every
 * chunk, so a client that lost the first chunk can still validate and size what
 * it does receive.
 */
typedef struct __attribute__((packed)) StreamPacketHeader {
    uint32_t magic;           // 0  STREAM_MAGIC
    uint32_t frameId;         // 4  +1 per frame, free to wrap
    uint64_t timestampUs;     // 8  console clock in microseconds, frame-constant
    uint32_t frameSize;       // 16 total payload bytes for the whole frame
    uint32_t chunkOffset;     // 20 byte offset of this chunk within the frame
    uint32_t frameCrc;        // 24 CRC-32 of the whole frame payload
    uint16_t width;           // 28 frame width in pixels
    uint16_t height;          // 30 frame height in pixels
    uint32_t stride;          // 32 bytes per row for RAW, 0 for JPEG
    uint16_t chunkLen;        // 36 payload bytes in this datagram
    uint8_t  flags;           // 38 STREAM_FLAG_*
    uint8_t  version;         // 39 STREAM_VERSION
    uint8_t  compressionType; // 40 STREAM_COMP_*
    uint8_t  pixelFormat;     // 41 STREAM_PIXFMT_*
    uint16_t reserved;        // 42 must be 0
} StreamPacketHeader;

/**
 * One block of audio. Each block carries the ADPCM decoder state it starts from,
 * so it decodes on its own: a lost block costs its own 20 ms and nothing after.
 * firstFrame numbers the block by sample frame, which lets the client fill a
 * gap with exactly the silence it stands for.
 */
typedef struct __attribute__((packed)) StreamAudioHeader {
    uint32_t magic;        // 0  STREAM_AUDIO_MAGIC
    uint8_t  version;      // 4  STREAM_AUDIO_VERSION
    uint8_t  codec;        // 5  STREAM_AUDIO_CODEC_*
    uint8_t  channels;     // 6  always 2
    uint8_t  reserved0;    // 7
    uint32_t sampleRate;   // 8  Hz
    uint32_t sequence;     // 12 +1 per block, free to wrap
    uint32_t firstFrame;   // 16 sample-frame index of the block's first frame, free to wrap
    uint16_t frames;       // 20 sample frames in the block (= payload bytes for stereo ADPCM)
    uint16_t reserved1;    // 22
    int16_t  predictor[2]; // 24 left, right: decoder state at the first frame
    uint8_t  stepIndex[2]; // 28 left, right
    uint16_t reserved2;    // 30
} StreamAudioHeader;

/** A plain-text status report: lines starting "settings", "state" and "perf". */
typedef struct __attribute__((packed)) StreamStatusHeader {
    uint32_t magic;   // 0 STREAM_STATUS_MAGIC
    uint8_t  version; // 4 STREAM_STATUS_VERSION
    uint8_t  kind;    // 5 0 = text
    uint16_t length;  // 6 bytes of text that follow
} StreamStatusHeader;

#ifdef __cplusplus
static_assert(sizeof(StreamPacketHeader) == STREAM_HEADER_SIZE,
              "StreamPacketHeader must be exactly 44 bytes on the wire");
static_assert(sizeof(StreamAudioHeader) == STREAM_AUDIO_HEADER_SIZE, "StreamAudioHeader must be 32 bytes");
static_assert(sizeof(StreamStatusHeader) == STREAM_STATUS_HEADER_SIZE, "StreamStatusHeader must be 8 bytes");
#else
_Static_assert(sizeof(StreamPacketHeader) == STREAM_HEADER_SIZE,
               "StreamPacketHeader must be exactly 44 bytes on the wire");
_Static_assert(sizeof(StreamAudioHeader) == STREAM_AUDIO_HEADER_SIZE, "StreamAudioHeader must be 32 bytes");
_Static_assert(sizeof(StreamStatusHeader) == STREAM_STATUS_HEADER_SIZE, "StreamStatusHeader must be 8 bytes");
#endif

#ifdef __cplusplus
}
#endif
