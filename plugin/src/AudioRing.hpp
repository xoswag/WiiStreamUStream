#pragma once

#include <stdint.h>

/**
 * Hand-off from the console's audio mixer to our audio sender: one producer, one
 * consumer, no locks.
 *
 * The producer is the AX final-mix callback, which runs every 3 ms inside the
 * console's audio processing - it must never block, take a lock or call into the
 * OS, so all it does here is copy samples and publish a new write position. The
 * consumer is our own thread, which reads whole blocks at its own pace.
 *
 * Each stereo frame is one 32-bit word (left in the high half), so a frame is
 * written and read with a single store/load. Positions are free-running 32-bit
 * counters; the difference of the two is the fill level even across a wrap.
 */
class AudioRing {
public:
    static constexpr uint32_t CAPACITY = 1u << 14; // frames: ~340 ms at 48 kHz

    // --- Producer -------------------------------------------------------------------
    // Either Write() a whole block, or check CanWrite(n), Put() frames 0..n-1 and
    // Commit(n) - the second form needs no buffer of the caller's, which matters
    // in the mixer callback, where stack space is not ours to spend.

    /** True when `frames` more frames fit. */
    bool CanWrite(uint32_t frames) const {
        return frames <= CAPACITY - (mWrite - __atomic_load_n(&mRead, __ATOMIC_ACQUIRE));
    }

    /** Stores frame `i` of the block being written (not visible until Commit). */
    void Put(uint32_t i, int16_t left, int16_t right) {
        mBuf[(mWrite + i) & (CAPACITY - 1)] = ((uint32_t) (uint16_t) left << 16) | (uint16_t) right;
    }

    /** Publishes the `frames` frames Put since the last Commit. */
    void Commit(uint32_t frames) {
        __atomic_store_n(&mWrite, mWrite + frames, __ATOMIC_RELEASE);
    }

    /**
     * Appends `frames` interleaved stereo frames, all or nothing. Returns false
     * (writing nothing) when there is no room - a stalled consumer costs the
     * newest audio, never corrupts the oldest.
     */
    bool Write(const int16_t *interleaved, uint32_t frames) {
        if (!CanWrite(frames)) {
            return false;
        }
        for (uint32_t i = 0; i < frames; i++) {
            Put(i, interleaved[2 * i], interleaved[2 * i + 1]);
        }
        Commit(frames);
        return true;
    }

    /** Consumer. Frames ready to read. */
    uint32_t Available() const {
        return __atomic_load_n(&mWrite, __ATOMIC_ACQUIRE) - mRead;
    }

    /** Consumer. Reads `frames` (which must be <= Available()) as interleaved stereo. */
    void Read(int16_t *interleaved, uint32_t frames) {
        const uint32_t r = mRead; // only the consumer stores this
        for (uint32_t i = 0; i < frames; i++) {
            const uint32_t v      = mBuf[(r + i) & (CAPACITY - 1)];
            interleaved[2 * i]     = (int16_t) (v >> 16);
            interleaved[2 * i + 1] = (int16_t) (v & 0xFFFF);
        }
        __atomic_store_n(&mRead, r + frames, __ATOMIC_RELEASE);
    }

    /** Consumer. Drops `frames` (<= Available()) unread - used to catch up after a stall. */
    void Skip(uint32_t frames) {
        __atomic_store_n(&mRead, mRead + frames, __ATOMIC_RELEASE);
    }

private:
    uint32_t mBuf[CAPACITY] = {};
    uint32_t mWrite         = 0;
    uint32_t mRead          = 0;
};
