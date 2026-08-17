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
#include "ControlServer.hpp"
#include "ImageEncoder.hpp"
#include "ScreenCapture.hpp"
#include "StreamProtocol.h"
#include "StreamSender.hpp"
#include "config.hpp"
#include "retain_vars.hpp"
#include "utils/logger.h"

#include <coreinit/cache.h>
#include <wups.h>

WUPS_PLUGIN_NAME("Screen Streaming");
WUPS_PLUGIN_DESCRIPTION("Streams the TV or GamePad screen to a PC over the network. "
                        "Run the StreamingPluginClient on your computer and enter this console's IP.");
WUPS_PLUGIN_VERSION("v0.2");
WUPS_PLUGIN_AUTHOR("Maschell");
WUPS_PLUGIN_LICENSE("GPL");

WUPS_USE_STORAGE("screenstreaming");

namespace {

bool sPipelineRunning = false;

void startPipeline() {
    if (sPipelineRunning) {
        return;
    }

    if (!ScreenCapture::Init()) {
        DEBUG_FUNCTION_LINE_ERR("Failed to set up the capture buffers");
        return;
    }
    if (!ImageEncoder::Start()) {
        DEBUG_FUNCTION_LINE_ERR("Failed to start the encoder");
        ScreenCapture::Shutdown();
        return;
    }
    if (!ControlServer::Start()) {
        DEBUG_FUNCTION_LINE_ERR("Failed to start the control server");
        ImageEncoder::Stop();
        ScreenCapture::Shutdown();
        return;
    }

    sPipelineRunning = true;
    DEBUG_FUNCTION_LINE("Ready - connect the client to TCP %d", STREAM_TCP_PORT);
}

/**
 * Teardown order is the whole point of this function.
 *
 * Close the gates, wait for a capture that is already running on the game's
 * render thread, stop the threads, and only then release the buffers those
 * threads were reading. Upstream freed as it went and raced itself.
 */
void stopPipeline() {
    if (!sPipelineRunning) {
        return;
    }
    sPipelineRunning = false;

    gClientConnected = false;
    gHasForeground   = false;
    OSMemoryBarrier();
    StreamWaitForCapturesToFinish();

    ControlServer::Stop();
    ImageEncoder::Stop();
    ScreenCapture::Shutdown();

    DEBUG_FUNCTION_LINE("Pipeline stopped");
}

} // namespace

INITIALIZE_PLUGIN() {
    initLogging();
    StreamSender::InitOnce();
    StreamConfig::Init();
    DEBUG_FUNCTION_LINE("Screen Streaming plugin initialised");
}

DEINITIALIZE_PLUGIN() {
    deinitLogging();
}

ON_APPLICATION_START() {
    initLogging();

    gHasForeground   = true;
    gClientConnected = false;
    OSMemoryBarrier();

    startPipeline();
}

ON_APPLICATION_ENDS() {
    stopPipeline();
    deinitLogging();
}

ON_APPLICATION_REQUESTS_EXIT() {
    // Stop capturing before the title starts tearing its own GX2 state down.
    gHasForeground = false;
    OSMemoryBarrier();
    StreamWaitForCapturesToFinish();
}

ON_ACQUIRED_FOREGROUND() {
    gHasForeground = true;
    OSMemoryBarrier();
}

ON_RELEASE_FOREGROUND() {
    // The HOME menu is taking over; the GX2 state we capture from is not ours
    // to touch while we are in the background.
    gHasForeground = false;
    OSMemoryBarrier();
    StreamWaitForCapturesToFinish();
}
