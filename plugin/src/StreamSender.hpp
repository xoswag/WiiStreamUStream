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

/** Splits one JPEG into v2 datagrams. Returns false if the frame was not fully sent. */
bool SendFrame(const uint8_t *jpeg, uint32_t size);

uint32_t GetSendFailures();

} // namespace StreamSender
