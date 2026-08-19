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
#include "ControlServer.hpp"
#include "StreamProtocol.h"
#include "StreamSender.hpp"
#include "retain_vars.hpp"
#include "utils/logger.h"

#include <arpa/inet.h> // htons
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ControlServer {
namespace {

constexpr uint32_t THREAD_STACK_SIZE = 0x8000;

/**
 * Deliberately above typical game threads, not just above the encoder.
 *
 * Lower number = higher priority on Cafe OS, and an application's main threads
 * usually sit around 16. The first attempt at fixing the reconnect loop used 18,
 * which beats our own encoder (25) but still loses to the game - so under a heavy
 * title (Minecraft) this thread went unscheduled for seconds at a time, the client
 * gave up waiting for a PONG, and the session died and relaunched in a loop while
 * the game itself ran perfectly.
 *
 * Running it above the game is safe precisely because it does almost nothing: it
 * wakes 4 times a second, reads one byte and writes one back. Microseconds of CPU
 * against a frame budget of 16 ms.
 */
constexpr int32_t CONTROL_THREAD_PRIORITY = 10;

/**
 * A client that has not pinged in this long is treated as gone.
 *
 * Generous on purpose: dropping a client is expensive (it tears down the video
 * socket and the client has to reconnect), and the cost of waiting a little
 * longer is nothing. The client pings once a second, so this tolerates ten
 * consecutive missed pings.
 */
constexpr int CLIENT_TIMEOUT_MS = 10000;

/**
 * How long the heartbeat blocks before re-checking sShouldExit.
 *
 * The client socket is deliberately owned by this thread alone - Stop() never
 * touches it. Reaching across to shutdown() another thread's descriptor means
 * racing its close(), and a file descriptor number is reusable the instant it is
 * freed, so the loser can end up tearing down whatever socket the process opened
 * next. Polling costs one wakeup every 250 ms and removes that hazard entirely.
 */
constexpr int POLL_INTERVAL_MS = 250;

OSThread *sThread = nullptr;
void *sThreadStack = nullptr;

volatile bool sShouldExit = false;
volatile bool sClientConnected = false;

// Claimed with __atomic_exchange_n before closing, so exactly one of the server
// thread and Stop() ever calls close() on it.
int sListenSocket = -1;

/**
 * Stops video and waits for a capture that is already running to finish.
 *
 * Ordering matters: the gate has to be cleared before the socket goes away, and
 * the GX2 hook may already be inside CaptureFrame on the game's render thread.
 * Upstream deleted the UDP server straight from this thread while the encoder
 * was still using it.
 */
void stopStreaming() {
    gClientConnected = false;
    OSMemoryBarrier();

    StreamWaitForCapturesToFinish();

    StreamSender::Close();
    sClientConnected = false;
}

/** Waits for the socket to become readable. Returns 1 readable, 0 timeout, -1 error. */
int waitReadable(int fd, int timeoutMs) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);

    struct timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    return select(fd + 1, &readSet, nullptr, nullptr, &tv);
}

void serveClient(int clientSocket, uint32_t clientIp) {
    if (!StreamSender::Open(clientIp)) {
        return;
    }

    sClientConnected = true;
    gClientConnected = true;
    OSMemoryBarrier();

    DEBUG_FUNCTION_LINE("Client connected");

    int idleMs = 0;
    while (!sShouldExit) {
        const int ready = waitReadable(clientSocket, POLL_INTERVAL_MS);
        if (ready < 0) {
            DEBUG_FUNCTION_LINE("select failed (errno %d), dropping the client", errno);
            break;
        }
        if (ready == 0) {
            idleMs += POLL_INTERVAL_MS;
            if (idleMs >= CLIENT_TIMEOUT_MS) {
                DEBUG_FUNCTION_LINE("No ping for %dms, dropping the client", CLIENT_TIMEOUT_MS);
                break;
            }
            continue;
        }
        idleMs = 0;

        uint8_t command = 0;
        const int got = recv(clientSocket, &command, 1, 0);
        if (got <= 0) {
            DEBUG_FUNCTION_LINE("Client disconnected");
            break;
        }

        if (command == STREAM_PING) {
            const uint8_t pong = STREAM_PONG;
            if (send(clientSocket, &pong, 1, 0) != 1) {
                DEBUG_FUNCTION_LINE("Failed to answer a ping, dropping the client");
                break;
            }
        }
    }

    stopStreaming();
}

int threadEntry(int /*argc*/, const char ** /*argv*/) {
    const int listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket < 0) {
        DEBUG_FUNCTION_LINE_ERR("Failed to create the listening socket (errno %d)", errno);
        return -1;
    }

    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_port        = htons(STREAM_TCP_PORT);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSocket, (struct sockaddr *) &bindAddr, sizeof(bindAddr)) < 0) {
        DEBUG_FUNCTION_LINE_ERR("Failed to bind port %d (errno %d)", STREAM_TCP_PORT, errno);
        close(listenSocket);
        return -1;
    }

    if (listen(listenSocket, 1) < 0) {
        DEBUG_FUNCTION_LINE_ERR("listen failed (errno %d)", errno);
        close(listenSocket);
        return -1;
    }

    __atomic_store_n(&sListenSocket, listenSocket, __ATOMIC_RELEASE);
    DEBUG_FUNCTION_LINE("Waiting for a client on TCP %d", STREAM_TCP_PORT);

    while (!sShouldExit) {
        struct sockaddr_in clientAddr;
        memset(&clientAddr, 0, sizeof(clientAddr));
        socklen_t addrLen = sizeof(clientAddr);

        const int clientSocket = accept(listenSocket, (struct sockaddr *) &clientAddr, &addrLen);
        if (clientSocket < 0) {
            if (sShouldExit) {
                break;
            }
            OSSleepTicks(OSMillisecondsToTicks(200));
            continue;
        }

        int noDelay = 1;
        setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

        serveClient(clientSocket, clientAddr.sin_addr.s_addr);
        close(clientSocket);
    }

    // Stop() may already have closed the listening socket. Claim it so exactly
    // one of us calls close() - the fd number is reusable the instant it is
    // freed, and a double close would take down whichever socket the system
    // handed the number to next.
    const int toClose = __atomic_exchange_n(&sListenSocket, -1, __ATOMIC_ACQ_REL);
    if (toClose >= 0) {
        close(toClose);
    }

    DEBUG_FUNCTION_LINE("Control server stopped");
    return 0;
}

} // namespace

bool Start() {
    if (sThread != nullptr) {
        return true;
    }

    sShouldExit = false;

    sThread = (OSThread *) memalign(8, sizeof(OSThread));
    if (sThread == nullptr) {
        return false;
    }
    memset(sThread, 0, sizeof(OSThread));

    sThreadStack = memalign(0x20, THREAD_STACK_SIZE);
    if (sThreadStack == nullptr) {
        free(sThread);
        sThread = nullptr;
        return false;
    }

    if (!OSCreateThread(sThread, threadEntry, 0, nullptr,
                        (char *) sThreadStack + THREAD_STACK_SIZE, THREAD_STACK_SIZE,
                        CONTROL_THREAD_PRIORITY, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        DEBUG_FUNCTION_LINE_ERR("Failed to create the control server thread");
        free(sThreadStack);
        free(sThread);
        sThreadStack = nullptr;
        sThread      = nullptr;
        return false;
    }

    OSSetThreadName(sThread, "ScreenStreaming control");
    OSResumeThread(sThread);
    return true;
}

void Stop() {
    if (sThread == nullptr) {
        return;
    }

    sShouldExit = true;
    OSMemoryBarrier();

    stopStreaming();

    // Closing the listening socket is what kicks the thread out of accept().
    // A connected client is left to the server thread, which notices sShouldExit
    // within POLL_INTERVAL_MS and closes its own descriptor.
    const int listenSocket = __atomic_exchange_n(&sListenSocket, -1, __ATOMIC_ACQ_REL);
    if (listenSocket >= 0) {
        shutdown(listenSocket, SHUT_RDWR);
        close(listenSocket);
    }

    int result = 0;
    OSJoinThread(sThread, &result);

    free(sThreadStack);
    free(sThread);
    sThreadStack = nullptr;
    sThread      = nullptr;
}

bool IsClientConnected() {
    return sClientConnected;
}

} // namespace ControlServer
