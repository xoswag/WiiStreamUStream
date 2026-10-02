// Hooks the host tests use to steer the stubbed console and the fakes.
#pragma once

#include "StreamSender.hpp"

#include <stdint.h>
#include <string>
#include <vector>

namespace testhooks {

/**
 * Makes a "core" behave like one the running game keeps busy: a blocking
 * receive on a thread pinned there is followed, `percent`% of the time, by a
 * sleep of [minMs, maxMs] - the time the thread would spend descheduled.
 */
void setStarve(int core, int percent, int minMs, int maxMs);
void clearStarve();

/** How many log lines so far contained `needle`. */
int logCount(const std::string &needle);
void resetLogCounts();
/** Cache-maintenance calls that were not whole, in-bounds 32-byte lines. */
int badCacheOps();

} // namespace testhooks

namespace fakes {

// --- capture -------------------------------------------------------------------
void captureSetup(uint32_t width, uint32_t height, bool srgb);
/** Frees the slots; reports a slot still held by the encoder as an error. */
void captureTeardown();
/** Feeds frames as fast as slots free up (or every pacingUs). */
void producerStart(uint32_t pacingUs);
void producerStop();
void failGpu(int count);
uint32_t framesProduced();
uint32_t lastReleasedFrame();
int captureErrors();

uint32_t sourceWidth();
uint32_t sourceHeight();
bool sourceSrgb();
/** The deterministic content of frame `id`, as 0xRRGGBBAA words. */
void fillFrame(uint32_t *pixels, uint32_t width, uint32_t height, uint32_t pitch, uint32_t id);

// --- sender --------------------------------------------------------------------
struct Submitted {
    uint32_t frameId;
    std::vector<uint8_t> jpeg;
    StreamSender::FrameMeta meta;
    int32_t path;
    int32_t quality;
    int32_t captureSize;
    bool srgb;
};
/** Called for every frame the encoder hands to the sender. */
using SubmitHandler = void (*)(Submitted &&frame);
void setSubmitHandler(SubmitHandler handler);
uint32_t submittedCount();

} // namespace fakes
