#pragma once

#include <stdint.h>

/**
 * IMA/DVI ADPCM, 4 bits per sample - the encoder half.
 *
 * Chosen for the audio stream because it is about a dozen integer operations per
 * sample (nothing next to video on this CPU) and cuts 48 kHz stereo from 1.5
 * Mbit/s to ~384 kbit/s, which matters on the Wii U's slow Wi-Fi. The client's
 * decoder (ImaAdpcm.java) uses the same tables; both are checked against one
 * reference vector in the tests.
 */
namespace Adpcm {

constexpr int16_t STEP[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

constexpr int8_t INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/** One channel's state, identical on encoder and decoder after every sample. */
struct State {
    int32_t predictor = 0;
    int32_t index     = 0;
};

/** Encodes one sample and advances the state exactly as the decoder will. */
inline uint8_t EncodeSample(State &s, int32_t sample) {
    int32_t step   = STEP[s.index];
    int32_t diff   = sample - s.predictor;
    uint8_t code   = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    // The decoder rebuilds the difference as step/8 plus step, step/2, step/4 for
    // each set bit; build it the same way here so the two predictors never part.
    int32_t vpdiff = step >> 3;
    if (diff >= step) {
        code |= 4;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 2;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 1;
        vpdiff += step;
    }

    int32_t p = (code & 8) ? s.predictor - vpdiff : s.predictor + vpdiff;
    if (p < -32768) p = -32768;
    if (p > 32767) p = 32767;
    s.predictor = p;

    int32_t i = s.index + INDEX[code & 7];
    if (i < 0) i = 0;
    if (i > 88) i = 88;
    s.index = i;
    return code;
}

/**
 * Encodes interleaved stereo (L, R, L, R...) into one byte per frame: the left
 * code in the low nibble, the right in the high.
 */
inline void EncodeStereo(const int16_t *interleaved, uint32_t frames, State &left, State &right, uint8_t *out) {
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t l = EncodeSample(left, interleaved[2 * i]);
        const uint8_t r = EncodeSample(right, interleaved[2 * i + 1]);
        out[i]          = (uint8_t) (l | (r << 4));
    }
}

} // namespace Adpcm
