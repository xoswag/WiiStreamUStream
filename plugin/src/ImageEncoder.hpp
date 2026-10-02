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
 * Owns the encoder thread: pulls captured frames, turns them into JPEGs and
 * hands them to StreamSender.
 */
namespace ImageEncoder {

bool Start();

/** Signals the thread, waits for it to exit and releases everything it owned. */
void Stop();

bool IsRunning();

constexpr int MAX_ENCODER_CORES = 3;

/** Cumulative counters (free to wrap - take differences), readable from any thread in any build. */
struct Stats {
    uint32_t framesEncoded;
    uint32_t framesDropped; // encoded but given up (a follower missed the deadline, a failure)
    uint32_t gpuTimeouts;
    uint32_t frameUsTotal;  // wall time per encoded frame, summed
    uint32_t bandsTotal;    // bands per encoded frame, summed
    uint32_t width;         // of the last frame
    uint32_t height;
    int32_t coreCount;
    int32_t core[MAX_ENCODER_CORES];          // [0] leads
    uint32_t benched[MAX_ENCODER_CORES];      // followers only
    uint32_t bandLatencyUs[MAX_ENCODER_CORES]; // followers only: last band, dispatch to done
};

void GetStats(Stats &out);

} // namespace ImageEncoder
