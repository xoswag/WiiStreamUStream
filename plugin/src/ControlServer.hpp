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
 ****************************************************************************/
#pragma once

#include <stdint.h>

/**
 * Listens on TCP 8092 for the PC client.
 *
 * The client connects, the console learns its address from the accepted socket
 * and starts sending UDP video there. The client then pings once a second; when
 * the pings stop, video stops.
 *
 * Upstream got this from libutilswut's TCPServer, which no longer exists, so it
 * is implemented here directly.
 */
namespace ControlServer {

bool Start();

void Stop();

bool IsClientConnected();

} // namespace ControlServer
