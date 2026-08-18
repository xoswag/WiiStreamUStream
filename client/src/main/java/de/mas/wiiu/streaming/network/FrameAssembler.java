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
 * Reassembles v3 datagrams into whole frames.
 *
 * Only ever one frame is in flight: the Wii U sends a frame's chunks back to back and
 * never interleaves two frames, so a datagram belonging to a newer frame is proof that
 * the frame in progress will never complete. Holding a single frame keeps this allocation
 * free in the steady state and bounds memory no matter what arrives on the wire.
 *
 * Not thread safe - drive it from the single UDP receive thread.
 */
public final class FrameAssembler {
    private final Consumer<Frame> onFrame;
    private final CRC32 crc = new CRC32();

    private boolean haveFrame = false;
    private boolean haveCompleted = false;
    private int lastCompletedFrameId;
    private volatile boolean sessionReset = false;

    // Metadata of the frame currently being assembled.
    private int frameId;
    private int frameSize;
    private int frameCrc;
    private long frameTimestampUs;
    private int frameWidth;
    private int frameHeight;
    private int frameStride;
    private int frameComp;
    private int framePixfmt;

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

    public FrameAssembler(Consumer<Frame> onFrame) {
        this.onFrame = onFrame;
    }

    /**
     * Forgets which frame ids have been seen. Call on every (re)connect: the
     * console restarts its frame counter whenever the plugin is reloaded - which
     * a title change does - and without this the staleness filter would reject
     * the whole new session until its counter climbed past the old one.
     */
    public void requestReset() {
        sessionReset = true;
    }

    /**
     * @param data one datagram exactly as it came off the wire
     * @param length valid bytes in {@code data}
     */
    public void accept(byte[] data, int length) {
        if (sessionReset) {
            sessionReset = false;
            haveFrame = false;
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
        final long pktTimestampUs = h.getLong(StreamProtocol.OFF_TIMESTAMP_US);
        final int pktFrameSize = h.getInt(StreamProtocol.OFF_FRAME_SIZE);
        final int pktChunkOffset = h.getInt(StreamProtocol.OFF_CHUNK_OFFSET);
        final int pktFrameCrc = h.getInt(StreamProtocol.OFF_FRAME_CRC);
        final int pktWidth = h.getShort(StreamProtocol.OFF_WIDTH) & 0xFFFF;
        final int pktHeight = h.getShort(StreamProtocol.OFF_HEIGHT) & 0xFFFF;
        final int pktStride = h.getInt(StreamProtocol.OFF_STRIDE);
        final int pktChunkLen = h.getShort(StreamProtocol.OFF_CHUNK_LEN) & 0xFFFF;
        final int pktComp = data[StreamProtocol.OFF_COMPRESSION] & 0xFF;
        final int pktPixfmt = data[StreamProtocol.OFF_PIXEL_FORMAT] & 0xFF;

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
        // For a raw layout, the declared geometry must exactly account for the frame
        // size, otherwise a decoder would read past the row it was handed. JPEG carries
        // its own dimensions so its header width/height/stride are advisory only.
        if (pktComp == StreamProtocol.COMP_RAW && !rawGeometryValid(pktPixfmt, pktWidth, pktHeight, pktStride, pktFrameSize)) {
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
            startFrame(pktFrameId, pktFrameSize, pktFrameCrc, pktTimestampUs, pktWidth, pktHeight, pktStride, pktComp,
                    pktPixfmt);
        } else if (pktFrameSize != frameSize || pktFrameCrc != frameCrc || pktComp != frameComp
                || pktPixfmt != framePixfmt || pktWidth != frameWidth || pktHeight != frameHeight
                || pktStride != frameStride) {
            // Same id but disagreeing metadata: one of the two is corrupt. Drop the
            // chunk rather than mixing them.
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

    private static boolean rawGeometryValid(int pixfmt, int width, int height, int stride, int frameSize) {
        final int bpp = StreamProtocol.bytesPerPixel(pixfmt);
        if (bpp == 0 || width <= 0 || height <= 0 || stride < width * bpp) {
            return false;
        }
        return (long) height * stride == frameSize;
    }

    private void startFrame(int id, int size, int expectedCrc, long timestampUs, int width, int height, int stride,
            int comp, int pixfmt) {
        frameId = id;
        frameSize = size;
        frameCrc = expectedCrc;
        frameTimestampUs = timestampUs;
        frameWidth = width;
        frameHeight = height;
        frameStride = stride;
        frameComp = comp;
        framePixfmt = pixfmt;
        receivedBytes = 0;
        seenChunks.clear();
        if (buffer.length < size) {
            buffer = new byte[size];
        }
        haveFrame = true;
    }

    private void completeFrame() {
        haveFrame = false;
        haveCompleted = true;
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
        final byte[] payload = new byte[frameSize];
        System.arraycopy(buffer, 0, payload, 0, frameSize);
        onFrame.accept(new Frame(payload, frameComp, framePixfmt, frameWidth, frameHeight, frameStride,
                frameTimestampUs));
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
