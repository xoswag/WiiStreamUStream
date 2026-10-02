package de.mas.wiiu.streaming.network;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.CRC32;

import org.junit.jupiter.api.Test;

class FrameAssemblerTest {

    private final List<Frame> shown = new ArrayList<>();
    private final FrameAssembler assembler = new FrameAssembler(shown::add);

    /** A frame's payload: distinct per id, and big enough to need several chunks. */
    private static byte[] payload(int id, int size) {
        final byte[] p = new byte[size];
        for (int i = 0; i < size; i++) {
            p[i] = (byte) (id * 31 + i);
        }
        return p;
    }

    /** All the datagrams for one frame, in send order, exactly as the console builds them. */
    private static List<byte[]> datagrams(int id, int size) {
        final byte[] p = payload(id, size);
        final CRC32 crc = new CRC32();
        crc.update(p);
        final List<byte[]> out = new ArrayList<>();
        for (int off = 0; off < size; off += StreamProtocol.MAX_PAYLOAD) {
            final int len = Math.min(StreamProtocol.MAX_PAYLOAD, size - off);
            final ByteBuffer b = ByteBuffer.allocate(StreamProtocol.HEADER_SIZE + len);
            b.putInt(StreamProtocol.MAGIC);
            b.putInt(id);
            b.putLong(1000L * id);
            b.putInt(size);
            b.putInt(off);
            b.putInt((int) crc.getValue());
            b.putShort((short) 640);
            b.putShort((short) 360);
            b.putInt(0);
            b.putShort((short) len);
            b.put((byte) (StreamProtocol.FLAG_KEYFRAME | (off + len >= size ? StreamProtocol.FLAG_LAST_CHUNK : 0)));
            b.put((byte) StreamProtocol.VERSION);
            b.put((byte) StreamProtocol.COMP_JPEG);
            b.put((byte) StreamProtocol.PIXFMT_JPEG);
            b.putShort((short) 0);
            b.put(p, off, len);
            out.add(b.array());
        }
        return out;
    }

    private void feed(byte[] d) {
        assembler.accept(d, d.length);
    }

    private void feedAll(List<byte[]> ds) {
        for (byte[] d : ds) {
            feed(d);
        }
    }

    private int shownId(int i) {
        return (int) (shown.get(i).timestampUs / 1000);
    }

    @Test
    void inOrderFramesAreAllShown() {
        for (int id = 1; id <= 5; id++) {
            feedAll(datagrams(id, 5000));
        }
        assertEquals(5, shown.size());
        assertEquals(0, assembler.getFramesIncomplete());
        assertArrayEquals(payload(3, 5000), shown.get(2).payload);
    }

    @Test
    void lastChunkArrivingAfterTheNextFrameStartsIsRecovered() {
        // The pattern measured on hardware: frame N's final datagram overtaken by
        // frame N+1's first.
        final List<byte[]> a = datagrams(10, 5000);
        final List<byte[]> b = datagrams(11, 5000);
        for (int i = 0; i < a.size() - 1; i++) {
            feed(a.get(i));
        }
        feed(b.get(0));
        feed(a.get(a.size() - 1)); // the straggler
        for (int i = 1; i < b.size(); i++) {
            feed(b.get(i));
        }

        assertEquals(2, shown.size(), "both frames shown");
        assertEquals(10, shownId(0), "in order");
        assertEquals(11, shownId(1));
        assertEquals(1, assembler.getFramesCompletedLate());
        assertEquals(0, assembler.getFramesIncomplete());
        assertEquals(0, assembler.getDatagramsIgnored());
        assertArrayEquals(payload(10, 5000), shown.get(0).payload);
    }

    @Test
    void anOlderFrameIsNeverShownAfterANewerOne() {
        final List<byte[]> a = datagrams(20, 5000);
        final List<byte[]> b = datagrams(21, 5000);
        for (int i = 0; i < a.size() - 1; i++) {
            feed(a.get(i));
        }
        feedAll(b); // the newer frame completes first
        feed(a.get(a.size() - 1)); // straggler arrives too late to be shown in order

        assertEquals(1, shown.size());
        assertEquals(21, shownId(0));
        assertEquals(1, assembler.getFramesIncomplete());
        assertEquals(1, assembler.getDatagramsIgnored(), "the late chunk is stale");
    }

    @Test
    void aReallyLostChunkCostsOnlyItsOwnFrame() {
        final List<byte[]> a = datagrams(30, 5000);
        for (int i = 0; i < a.size() - 1; i++) {
            feed(a.get(i)); // last chunk never arrives
        }
        feedAll(datagrams(31, 5000));
        feedAll(datagrams(32, 5000));

        assertEquals(2, shown.size());
        assertEquals(31, shownId(0));
        assertEquals(32, shownId(1));
        assertEquals(1, assembler.getFramesIncomplete());
    }

    @Test
    void aThirdFrameStartingGivesUpTheOldest() {
        final List<byte[]> a = datagrams(40, 5000);
        final List<byte[]> b = datagrams(41, 5000);
        feed(a.get(0));
        feed(b.get(0));
        feedAll(datagrams(42, 5000)); // 40 is evicted when 42 starts; 41 when 42 completes
        feed(a.get(1)); // stale now

        assertEquals(1, shown.size());
        assertEquals(42, shownId(0));
        assertEquals(2, assembler.getFramesIncomplete());
        assertEquals(1, assembler.getDatagramsIgnored());
    }

    @Test
    void duplicatesAreIgnoredAndDoNotRestartAFrame() {
        final List<byte[]> a = datagrams(50, 5000);
        feed(a.get(0));
        feed(a.get(0));
        feedAll(a.subList(1, a.size()));
        feed(a.get(2)); // duplicate after completion

        assertEquals(1, shown.size());
        assertEquals(2, assembler.getDatagramsIgnored());
        assertEquals(0, assembler.getFramesIncomplete());
    }

    @Test
    void aResetAcceptsACounterThatStartedAgain() {
        feedAll(datagrams(1000, 3000));
        assembler.requestReset(); // the console reloaded the plugin
        feedAll(datagrams(1, 3000));
        assertEquals(2, shown.size());
    }
}
