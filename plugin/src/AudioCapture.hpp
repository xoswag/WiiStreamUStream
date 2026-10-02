#pragma once

#include <stdint.h>

/**
 * Game audio, streamed alongside the video.
 *
 * The console's audio library (AX, sndcore2) mixes every voice down to one
 * buffer per output device every 3 ms and offers each finished mix to a
 * "final mix callback" before it goes to the speakers. We install our own
 * callback there - wrapping the game's, if it has one - copy the mix of the
 * screen being streamed into a lock-free ring, and a thread of ours compresses
 * it (IMA ADPCM) and sends it in 20 ms blocks.
 *
 * Only titles that use sndcore2 are covered (nearly all of them; a few launch
 * titles use the older snd_core library). "Audio: Off" in the menu installs
 * nothing at all at the next title start.
 */
namespace AudioCapture {

/** Starts the sender thread. Idempotent. */
bool Start();

/** Stops and joins the sender thread. */
void Stop();

struct Stats {
    uint32_t packetsSent;   // cumulative
    uint32_t bytesSent;     // cumulative, wraps
    uint32_t framesDropped; // ring full, or skipped to catch up; cumulative
    uint32_t sampleRate;    // 0 until audio has been captured
    bool hooked;            // our callback is installed in this title's mixer
};

void GetStats(Stats &out);

} // namespace AudioCapture
