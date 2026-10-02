#pragma once

#include <stdint.h>

/**
 * A few lines of plain text sent to the client about once a second, which it
 * prints in its own log:
 *
 *   settings  what the plugin menu is set to
 *   state     what is running (a game, the Wii U Menu...) and what is being streamed
 *   perf      the console's own frame rates, costs and bottleneck counters
 *
 * Unlike the DEBUG build's UDP log, this works in every build and needs nothing
 * running on the PC except the client.
 */
namespace StatusReport {

/** Forget the previous sample, so the next report measures from now. Call on client connect. */
void Reset();

/** Builds and sends one report. Called by the control server while a client is connected. */
void Send();

} // namespace StatusReport
