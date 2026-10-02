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
 * The Wii U sends a frame's chunks back to back and never interleaves two frames,
 * but the network does not promise to deliver them in that order: on real
 * hardware the last chunk of a frame regularly arrived just after the first chunk
 * of the next one. Treating the first chunk of a newer frame as proof that the
 * current one was lost threw away about three of every four "incomplete" frames
 * in a measured session - their missing piece was already on its way.
 *
 * So two frames are kept open: the newest, and the one before it. A straggler
 * for the older one can still complete it, and it is shown as long as the newer
 * frame has not been shown yet - frames are never displayed out of order. The
 * older frame is given up when a third frame starts or the newer one completes.
 *
 * Not thread safe - drive it from the single UDP receive thread.
 */
public final class FrameAssembler {
    private final Consumer<Frame> onFrame;
    private final CRC32 crc = new CRC32();

    /** One frame being assembled. Two of these are reused forever, so steady state allocates nothing. */
    private static final class Slot {
        boolean active;
        int id;
        int size;
        int crc;
        long timestampUs;
        int width;
        int height;
        int stride;
        int comp;
        int pixfmt;
        byte[] buffer = new byte[0];
        int received;
        /** One bit per chunk slot, so a duplicated datagram cannot be counted twice. */
        final BitSet seen = new BitSet();

        void start(int id, int size, int crc, long timestampUs, int width, int height, int stride, int comp,
                int pixfmt) {
            this.id = id;
            this.size = size;
            this.crc = crc;
            this.timestampUs = timestampUs;
            this.width = width;
            this.height = height;
            this.stride = stride;
            this.comp = comp;
            this.pixfmt = pixfmt;
            received = 0;
            seen.clear();
            if (buffer.length < size) {
                buffer = new byte[size];
            }
            active = true;
        }

        boolean matches(int size, int crc, int comp, int pixfmt, int width, int height, int stride) {
            return this.size == size && this.crc == crc && this.comp == comp && this.pixfmt == pixfmt
                    && this.width == width && this.height == height && this.stride == stride;
        }
    }

    /** The newest frame being assembled. */
    private Slot current = new Slot();
    /** The frame before it, kept open briefly for late chunks. Only ever active while current is. */
    private Slot previous = new Slot();

    private boolean haveCompleted = false;
    private int lastCompletedFrameId;
    private volatile boolean sessionReset = false;

    // Statistics, read by the UI/logging thread.
    private volatile long framesCompleted = 0;
    private volatile long framesIncomplete = 0;
    private volatile long framesCrcFailed = 0;
    private volatile long datagramsIgnored = 0;
    private volatile long bytesReceived = 0;
    private volatile long framesCompletedLate = 0;

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
            current.active = false;
            previous.active = false;
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

        final Slot slot;
        if (current.active && pktFrameId == current.id) {
            slot = current;
        } else if (previous.active && pktFrameId == previous.id) {
            slot = previous;
        } else {
            // Not a frame in progress. A duplicate of a finished frame must not
            // restart it, and neither must a chunk of an older one: frame ids only
            // move forward, so anything not newer than the newest seen is stale.
            final int newestSeen = current.active ? current.id : lastCompletedFrameId;
            if ((current.active || haveCompleted) && !isNewer(pktFrameId, newestSeen)) {
                datagramsIgnored++;
                return;
            }
            // A newer frame starts. The frame before the current one has had its
            // chance; the current one stays open, as the previous, for stragglers.
            if (previous.active) {
                previous.active = false;
                framesIncomplete++;
            }
            if (current.active) {
                final Slot t = previous;
                previous = current;
                current = t;
            }
            current.start(pktFrameId, pktFrameSize, pktFrameCrc, pktTimestampUs, pktWidth, pktHeight, pktStride,
                    pktComp, pktPixfmt);
            slot = current;
        }

        if (!slot.matches(pktFrameSize, pktFrameCrc, pktComp, pktPixfmt, pktWidth, pktHeight, pktStride)) {
            // Same id but disagreeing metadata: one of the two is corrupt. Drop the
            // chunk rather than mixing them.
            datagramsIgnored++;
            return;
        }

        final int chunkIndex = pktChunkOffset / StreamProtocol.MAX_PAYLOAD;
        if (slot.seen.get(chunkIndex)) {
            datagramsIgnored++;
            return;
        }
        slot.seen.set(chunkIndex);

        System.arraycopy(data, StreamProtocol.HEADER_SIZE, slot.buffer, pktChunkOffset, pktChunkLen);
        slot.received += pktChunkLen;
        bytesReceived += pktChunkLen;

        if (slot.received >= slot.size) {
            completeFrame(slot);
        }
    }

    private static boolean rawGeometryValid(int pixfmt, int width, int height, int stride, int frameSize) {
        final int bpp = StreamProtocol.bytesPerPixel(pixfmt);
        if (bpp == 0 || width <= 0 || height <= 0 || stride < width * bpp) {
            return false;
        }
        return (long) height * stride == frameSize;
    }

    private void completeFrame(Slot slot) {
        slot.active = false;
        if (slot == previous) {
            // A straggler completed the older frame before the newer one finished,
            // so showing it now is still in order.
            framesCompletedLate++;
        } else if (previous.active) {
            // The newer frame finished first; the older one can no longer be shown in order.
            previous.active = false;
            framesIncomplete++;
        }
        haveCompleted = true;
        lastCompletedFrameId = slot.id;

        crc.reset();
        crc.update(slot.buffer, 0, slot.size);
        if ((int) crc.getValue() != slot.crc) {
            framesCrcFailed++;
            return;
        }

        framesCompleted++;
        // Hand out a right-sized copy: the caller decodes it on another thread and the
        // slot's buffer gets reused by a later frame.
        final byte[] payload = new byte[slot.size];
        System.arraycopy(slot.buffer, 0, payload, 0, slot.size);
        onFrame.accept(new Frame(payload, slot.comp, slot.pixfmt, slot.width, slot.height, slot.stride,
                slot.timestampUs));
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

    /** Frames saved by waiting for a chunk that arrived after the next frame had started. */
    public long getFramesCompletedLate() {
        return framesCompletedLate;
    }
}
