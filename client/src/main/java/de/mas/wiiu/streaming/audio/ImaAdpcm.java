package de.mas.wiiu.streaming.audio;

/**
 * IMA/DVI ADPCM, 4 bits per sample - the decoder half. The console encodes with
 * the same tables (plugin/src/Adpcm.hpp); both are checked against one
 * reference vector so the two can never drift apart.
 */
public final class ImaAdpcm {
    private ImaAdpcm() {
    }

    static final int[] STEP = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
            60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
            494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
            3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
            15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

    private static final int[] INDEX = { -1, -1, -1, -1, 2, 4, 6, 8 };

    /** One channel's decoder state: the predicted sample and the step-table index. */
    public static final class State {
        public int predictor;
        public int index;

        public State(int predictor, int index) {
            this.predictor = clamp(predictor, -32768, 32767);
            this.index = clamp(index, 0, STEP.length - 1);
        }
    }

    /** Decodes one 4-bit code and returns the new sample. */
    public static int decode(State s, int code) {
        final int step = STEP[s.index];
        int vpdiff = step >> 3;
        if ((code & 4) != 0) {
            vpdiff += step;
        }
        if ((code & 2) != 0) {
            vpdiff += step >> 1;
        }
        if ((code & 1) != 0) {
            vpdiff += step >> 2;
        }
        s.predictor = clamp((code & 8) != 0 ? s.predictor - vpdiff : s.predictor + vpdiff, -32768, 32767);
        s.index = clamp(s.index + INDEX[code & 7], 0, STEP.length - 1);
        return s.predictor;
    }

    /**
     * Decodes {@code frames} stereo frames, one byte each (left code in the low
     * nibble, right in the high), into interleaved 16-bit little-endian PCM.
     *
     * @return bytes written to {@code out} ({@code frames * 4})
     */
    public static int decodeStereo(byte[] in, int inOffset, int frames, State left, State right, byte[] out,
            int outOffset) {
        int o = outOffset;
        for (int i = 0; i < frames; i++) {
            final int b = in[inOffset + i] & 0xFF;
            final int l = decode(left, b & 0x0F);
            final int r = decode(right, b >>> 4);
            out[o++] = (byte) l;
            out[o++] = (byte) (l >> 8);
            out[o++] = (byte) r;
            out[o++] = (byte) (r >> 8);
        }
        return o - outOffset;
    }

    private static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }
}
