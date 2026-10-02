package de.mas.wiiu.streaming.audio;

import java.util.Arrays;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Level;
import java.util.logging.Logger;

import javax.sound.sampled.AudioFormat;
import javax.sound.sampled.AudioSystem;
import javax.sound.sampled.LineUnavailableException;
import javax.sound.sampled.SourceDataLine;

import de.mas.wiiu.streaming.network.StreamProtocol;

/**
 * Plays the console's audio datagrams.
 *
 * Each datagram is a self-contained block of IMA ADPCM (it carries the decoder
 * state it starts from), so a lost one costs exactly its own 20 ms and nothing
 * after it. Blocks are numbered by the sample frame they start at, which lets a
 * gap be filled with exactly the silence it stands for - so the sound stays in
 * step with the picture instead of drifting ahead after every loss.
 *
 * Latency is kept bounded both ways: playback waits for a small cushion before
 * starting (so normal network jitter does not stutter it), and blocks are
 * dropped whenever the sound card is holding more than it should (the console's
 * clock and the PC's never run at exactly the same speed).
 */
public final class AudioPlayer {
    private static final Logger log = Logger.getLogger(AudioPlayer.class.getName());

    /** Collected before playback (re)starts, so ordinary jitter does not starve it. */
    private static final int PREBUFFER_MS = 60;
    /** More than this waiting in the sound card and blocks are dropped to stay live. */
    private static final int MAX_QUEUED_MS = 150;
    private static final int LINE_BUFFER_MS = 250;
    /** A forward gap up to this long is lost audio and is filled with silence; longer is a restart. */
    private static final int MAX_FILL_MS = 200;

    private final BlockingQueue<byte[]> queue = new ArrayBlockingQueue<>(32);

    private volatile boolean muted = false;
    private volatile boolean resetRequested = false;
    private volatile boolean unavailable = false;

    private final AtomicLong packetsReceived = new AtomicLong();
    private final AtomicLong packetsDropped = new AtomicLong();
    private final AtomicLong framesFilled = new AtomicLong();
    private volatile int sampleRate = 0;

    public AudioPlayer() {
        final Thread t = new Thread(this::run, "Audio");
        t.setDaemon(true);
        t.start();
    }

    /** Called on the UDP thread with a datagram whose magic says it is audio. Never blocks. */
    public void accept(byte[] data, int length) {
        if (length < StreamProtocol.AUDIO_HEADER_SIZE
                || (data[StreamProtocol.AUDIO_OFF_VERSION] & 0xFF) != StreamProtocol.AUDIO_VERSION
                || (data[StreamProtocol.AUDIO_OFF_CODEC] & 0xFF) != StreamProtocol.AUDIO_CODEC_IMA_ADPCM
                || (data[StreamProtocol.AUDIO_OFF_CHANNELS] & 0xFF) != 2) {
            packetsDropped.incrementAndGet();
            return;
        }
        final int frames = u16(data, StreamProtocol.AUDIO_OFF_FRAMES);
        final int rate = s32(data, StreamProtocol.AUDIO_OFF_SAMPLE_RATE);
        if (frames == 0 || StreamProtocol.AUDIO_HEADER_SIZE + frames > length || rate < 8000 || rate > 96000) {
            packetsDropped.incrementAndGet();
            return;
        }
        packetsReceived.incrementAndGet();
        final byte[] copy = Arrays.copyOf(data, StreamProtocol.AUDIO_HEADER_SIZE + frames);
        if (!queue.offer(copy)) {
            // The player is behind: the oldest block is the one to lose.
            queue.poll();
            packetsDropped.incrementAndGet();
            queue.offer(copy);
        }
    }

    /** Forget the running sample position, e.g. after a reconnect (the console's counter restarts). */
    public void reset() {
        resetRequested = true;
    }

    public void setMuted(boolean muted) {
        this.muted = muted;
    }

    public long getPacketsReceived() {
        return packetsReceived.get();
    }

    public long getPacketsDropped() {
        return packetsDropped.get();
    }

    /** Sample frames that never arrived and were played as silence instead. */
    public long getFramesFilled() {
        return framesFilled.get();
    }

    public int getSampleRate() {
        return sampleRate;
    }

    private void run() {
        SourceDataLine line = null;
        int lineRate = 0;
        boolean started = false;
        boolean haveExpected = false;
        int expected = 0;
        byte[] pcm = new byte[0];
        final byte[] silence = new byte[4096];

        while (true) {
            final byte[] pkt;
            try {
                pkt = queue.poll(250, TimeUnit.MILLISECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }

            if (resetRequested) {
                resetRequested = false;
                haveExpected = false;
                if (line != null) {
                    line.stop();
                    line.flush();
                    started = false;
                }
            }
            if (pkt == null) {
                continue;
            }
            if (muted || unavailable) {
                haveExpected = false;
                if (line != null && started) {
                    line.stop();
                    line.flush();
                    started = false;
                }
                continue;
            }

            final int rate = s32(pkt, StreamProtocol.AUDIO_OFF_SAMPLE_RATE);
            final int firstFrame = s32(pkt, StreamProtocol.AUDIO_OFF_FIRST_FRAME);
            final int frames = u16(pkt, StreamProtocol.AUDIO_OFF_FRAMES);

            if (line == null || rate != lineRate) {
                if (line != null) {
                    line.close();
                }
                line = openLine(rate);
                if (line == null) {
                    unavailable = true;
                    continue;
                }
                lineRate = rate;
                sampleRate = rate;
                started = false;
                haveExpected = false;
            }
            final int bytesPerMs = Math.max(1, rate * 4 / 1000);

            // The line ran dry (the stream paused, or the network starved it):
            // collect a fresh cushion before playing again rather than stutter.
            if (started && line.getBufferSize() - line.available() == 0) {
                line.stop();
                started = false;
            }

            if (haveExpected) {
                final int delta = firstFrame - expected; // wrap-safe
                if (delta < 0) {
                    if (delta > -rate) {
                        packetsDropped.incrementAndGet(); // late or duplicated
                        continue;
                    }
                    haveExpected = false; // far behind: the console restarted its counter
                } else if (delta > 0 && delta <= (long) rate * MAX_FILL_MS / 1000) {
                    // Lost blocks: play their length in silence, so what follows
                    // stays in step with the picture - but never more than the
                    // sound card has room for, or the write would block and the
                    // silence would turn into added latency.
                    framesFilled.addAndGet(delta);
                    int bytes = Math.min(delta * 4, Math.max(0, line.available() - frames * 4));
                    bytes -= bytes % 4;
                    while (bytes > 0) {
                        final int n = Math.min(bytes, silence.length);
                        line.write(silence, 0, n);
                        bytes -= n;
                    }
                }
            }
            expected = firstFrame + frames;
            haveExpected = true;

            final int queued = line.getBufferSize() - line.available();
            if (queued > MAX_QUEUED_MS * bytesPerMs) {
                packetsDropped.incrementAndGet(); // holding too much: catch up
                continue;
            }

            if (pcm.length < frames * 4) {
                pcm = new byte[frames * 4];
            }
            final ImaAdpcm.State left = new ImaAdpcm.State(s16(pkt, StreamProtocol.AUDIO_OFF_PREDICTOR_L),
                    pkt[StreamProtocol.AUDIO_OFF_INDEX_L] & 0xFF);
            final ImaAdpcm.State right = new ImaAdpcm.State(s16(pkt, StreamProtocol.AUDIO_OFF_PREDICTOR_R),
                    pkt[StreamProtocol.AUDIO_OFF_INDEX_R] & 0xFF);
            final int n = ImaAdpcm.decodeStereo(pkt, StreamProtocol.AUDIO_HEADER_SIZE, frames, left, right, pcm, 0);
            line.write(pcm, 0, n);

            if (!started && line.getBufferSize() - line.available() >= PREBUFFER_MS * bytesPerMs) {
                line.start();
                started = true;
            }
        }
    }

    private static SourceDataLine openLine(int rate) {
        final AudioFormat format = new AudioFormat(rate, 16, 2, true, false);
        try {
            final SourceDataLine line = AudioSystem.getSourceDataLine(format);
            line.open(format, rate * 4 * LINE_BUFFER_MS / 1000);
            log.info("Audio: playing " + rate + " Hz stereo");
            return line;
        } catch (LineUnavailableException | IllegalArgumentException e) {
            log.log(Level.WARNING, "Audio: no usable output device (" + e.getMessage() + "), audio disabled", e);
            return null;
        }
    }

    private static int u16(byte[] b, int off) {
        return ((b[off] & 0xFF) << 8) | (b[off + 1] & 0xFF);
    }

    private static int s16(byte[] b, int off) {
        return (short) u16(b, off);
    }

    private static int s32(byte[] b, int off) {
        return ((b[off] & 0xFF) << 24) | ((b[off + 1] & 0xFF) << 16) | ((b[off + 2] & 0xFF) << 8) | (b[off + 3] & 0xFF);
    }
}
