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

/**
 * The UDP side of the stream.
 *
 * This is a module rather than an object on purpose. Upstream handed a raw
 * MJPEGStreamServerUDP* to the encoder thread and then deleted it from the TCP
 * thread the moment the client disconnected, so a frame that was mid-send used
 * a freed socket object. Here every entry point takes the same mutex, so a
 * close can never land in the middle of a send.
 */
namespace StreamSender {

/**
 * Everything the header needs that is not the payload itself. Kept as a small
 * value so the encoder can describe a JPEG, a RAW frame, or a future mode
 * without SendFrame growing a new argument each time.
 */
struct FrameMeta {
    uint16_t width;
    uint16_t height;
    uint32_t stride;          // bytes per row for RAW, 0 for JPEG
    uint8_t compressionType;  // STREAM_COMP_*
    uint8_t pixelFormat;      // STREAM_PIXFMT_*
};

/**
 * Initialises the module's lock. Call once from INITIALIZE_PLUGIN, before any
 * other thread exists - lazily initialising it inside Open()/Close() means two
 * first callers can re-initialise a mutex one of them already holds.
 */
void InitOnce();

/** Opens the UDP socket aimed at the connected client. Replaces any existing one. */
bool Open(uint32_t clientIp);

/** Closes the socket. Blocks until an in-flight SendFrame has finished. */
void Close();

bool IsOpen();

/** Splits one frame into v3 datagrams. Returns false if it was not fully sent. */
bool SendFrame(const uint8_t *payload, uint32_t size, const FrameMeta &meta);

uint32_t GetSendFailures();

// --- Instrumentation. Monotonic counters, read on the encoder's report tick. --
uint32_t GetFramesSent();
uint64_t GetBytesSent();

} // namespace StreamSender
