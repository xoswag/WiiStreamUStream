/*******************************************************************************
 * Copyright (c) 2018 Maschell
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *******************************************************************************/

package de.mas.wiiu.streaming.network;

import java.nio.ByteBuffer;
import java.util.BitSet;
import java.util.function.Consumer;
import java.util.zip.CRC32;

/**
 * Reassembles v2 datagrams into whole JPEG frames.
 *
 * Only ever one frame is in flight: the Wii U sends a frame's chunks back to back and
 * never interleaves two frames, so a datagram belonging to a newer frame is proof that
 * the frame in progress will never complete. Holding a single frame keeps this allocation
 * free in the steady state and bounds memory no matter what arrives on the wire.
 *
 * Not thread safe - drive it from the single UDP receive thread.
 */
public final class FrameAssembler {
    private final Consumer<byte[]> onFrame;
    private final CRC32 crc = new CRC32();

    private boolean haveFrame = false;
    private boolean haveCompleted = false;
    private int lastCompletedFrameId;
    private volatile boolean sessionReset = false;
    private int frameId;
    private int frameSize;
    private int frameCrc;
    private byte[] buffer = new byte[0];
    private int receivedBytes;
    /** One bit per chunk slot, so a duplicated datagram cannot be counted twice. */
    private final BitSet seenChunks = new BitSet();

    // Statistics, read by the UI/logging thread.
    private volatile long framesCompleted = 0;
    private volatile long framesIncomplete = 0;
    private volatile long framesCrcFailed = 0;
    private volatile long datagramsIgnored = 0;
    private volatile long bytesReceived = 0;

    public FrameAssembler(Consumer<byte[]> onFrame) {
        this.onFrame = onFrame;
    }

    /**
     * @param data one datagram exactly as it came off the wire
     * @param length valid bytes in {@code data}
     */
    /**
     * Forgets which frame ids have been seen. Call on every (re)connect: the
     * console restarts its frame counter whenever the plugin is reloaded — which
     * a title change does — and without this the staleness filter would reject
     * the whole new session until its counter climbed past the old one.
     */
    public void requestReset() {
        sessionReset = true;
    }

    public void accept(byte[] data, int length) {
        if (sessionReset) {
            sessionReset  = false;
            haveFrame     = false;
            haveCompleted = false;
        }

        if (length < StreamProtocol.HEADER_SIZE) {
            datagramsIgnored++;
            return;
        }

        final ByteBuffer h = ByteBuffer.wrap(data, 0, length); // big-endian by default

        if (h.getInt(StreamProtocol.OFF_MAGIC) != StreamProtocol.MAGIC
                || (data[StreamProtocol.OFF_VERSION] & 0xFF) != StreamProtocol.VERSION) {
            datagramsIgnored++;
            return;
        }

        final int pktFrameId = h.getInt(StreamProtocol.OFF_FRAME_ID);
        final int pktFrameSize = h.getInt(StreamProtocol.OFF_FRAME_SIZE);
        final int pktChunkOffset = h.getInt(StreamProtocol.OFF_CHUNK_OFFSET);
        final int pktChunkLen = h.getShort(StreamProtocol.OFF_CHUNK_LEN) & 0xFFFF;
        final int pktFrameCrc = h.getInt(StreamProtocol.OFF_FRAME_CRC);

        // Validate everything before it is allowed to size an allocation or an arraycopy.
        // A corrupt or spoofed datagram must not be able to do anything worse than be
        // ignored.
        if (pktFrameSize <= 0 || pktFrameSize > StreamProtocol.MAX_FRAME_BYTES) {
            datagramsIgnored++;
            return;
        }
        if (pktChunkLen == 0 || pktChunkLen > StreamProtocol.MAX_PAYLOAD) {
            datagramsIgnored++;
            return;
        }
        if (length != StreamProtocol.HEADER_SIZE + pktChunkLen) {
            datagramsIgnored++;
            return;
        }
        if (pktChunkOffset < 0 || pktChunkOffset > pktFrameSize - pktChunkLen) {
            datagramsIgnored++;
            return;
        }
        // Chunks are cut at a fixed stride, so a well-formed offset is always a multiple
        // of it. This is what lets us index the seen-set by chunk number.
        if (pktChunkOffset % StreamProtocol.MAX_PAYLOAD != 0) {
            datagramsIgnored++;
            return;
        }

        if (!haveFrame || pktFrameId != frameId) {
            // A duplicate of a frame we already finished must not restart it, and
            // neither must a chunk of an older frame. Frame ids only move forward,
            // so anything not newer than what we last saw is stale.
            final int newestSeen = haveFrame ? frameId : lastCompletedFrameId;
            if ((haveFrame || haveCompleted) && !isNewer(pktFrameId, newestSeen)) {
                datagramsIgnored++;
                return;
            }
            if (haveFrame) {
                framesIncomplete++;
            }
            startFrame(pktFrameId, pktFrameSize, pktFrameCrc);
        } else if (pktFrameSize != frameSize || pktFrameCrc != frameCrc) {
            // Same id but disagreeing metadata: one of the two is corrupt. Drop the chunk
            // rather than mixing them.
            datagramsIgnored++;
            return;
        }

        final int chunkIndex = pktChunkOffset / StreamProtocol.MAX_PAYLOAD;
        if (seenChunks.get(chunkIndex)) {
            datagramsIgnored++;
            return;
        }
        seenChunks.set(chunkIndex);

        System.arraycopy(data, StreamProtocol.HEADER_SIZE, buffer, pktChunkOffset, pktChunkLen);
        receivedBytes += pktChunkLen;
        bytesReceived += pktChunkLen;

        if (receivedBytes >= frameSize) {
            completeFrame();
        }
    }

    private void startFrame(int id, int size, int expectedCrc) {
        frameId = id;
        frameSize = size;
        frameCrc = expectedCrc;
        receivedBytes = 0;
        seenChunks.clear();
        if (buffer.length < size) {
            buffer = new byte[size];
        }
        haveFrame = true;
    }

    private void completeFrame() {
        haveFrame            = false;
        haveCompleted        = true;
        lastCompletedFrameId = frameId;

        crc.reset();
        crc.update(buffer, 0, frameSize);
        if ((int) crc.getValue() != frameCrc) {
            framesCrcFailed++;
            return;
        }

        framesCompleted++;
        // Hand out a right-sized copy: the caller decodes it on another thread and our
        // scratch buffer gets reused by the very next datagram.
        final byte[] frame = new byte[frameSize];
        System.arraycopy(buffer, 0, frame, 0, frameSize);
        onFrame.accept(frame);
    }

    /** Serial-number comparison, so the counter wrapping past 2^31 is not a discontinuity. */
    private static boolean isNewer(int candidate, int current) {
        return candidate - current > 0;
    }

    public long getFramesCompleted() {
        return framesCompleted;
    }

    public long getFramesIncomplete() {
        return framesIncomplete;
    }

    public long getFramesCrcFailed() {
        return framesCrcFailed;
    }

    public long getDatagramsIgnored() {
        return datagramsIgnored;
    }

    public long getBytesReceived() {
        return bytesReceived;
    }
}
