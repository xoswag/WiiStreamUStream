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

package de.mas.wiiu.streaming;

import java.awt.image.BufferedImage;
import java.io.IOException;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.net.UnknownHostException;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Level;
import java.util.logging.Logger;

import javax.swing.JOptionPane;
import javax.swing.SwingUtilities;

import de.mas.wiiu.streaming.gui.IImageProvider;
import de.mas.wiiu.streaming.gui.ImageProvider;
import de.mas.wiiu.streaming.network.Frame;
import de.mas.wiiu.streaming.network.FrameAssembler;
import de.mas.wiiu.streaming.network.StreamProtocol;
import de.mas.wiiu.streaming.network.TCPClient;
import de.mas.wiiu.streaming.network.UDPClient;
import de.mas.wiiu.streaming.utilities.Utilities;

public class ImageStreamer {
    private static final Logger log = Logger.getLogger(ImageStreamer.class.getName());

    private final ImageProvider imageProvider = new ImageProvider();
    private final TCPClient tcpClient;
    private final UDPClient udpClient;
    private final FrameAssembler assembler;

    /**
     * Decoding runs off the receive thread. A 720p JPEG takes long enough to decode that
     * doing it inline lets the socket buffer overflow and costs us the tail of the next
     * frame. Depth 1 with a drop-oldest policy: if we cannot keep up, showing the newest
     * frame beats building a latency backlog.
     */
    private final BlockingQueue<Frame> decodeQueue = new ArrayBlockingQueue<>(1);

    private long lastFramesCompleted = 0;
    private long lastBytesReceived = 0;
    // Incremented on the decoder thread, read-and-cleared on the stats thread, so
    // it needs the atomic read-modify-write rather than just a visible write.
    private final AtomicInteger decodedThisSecond = new AtomicInteger();
    private final AtomicLong decodeNanosThisSecond = new AtomicLong();
    // Frames that reached the decode queue but were evicted by a newer one because
    // the decoder could not keep up. This is the "we are dropping to stay live"
    // number and was invisible before.
    private final AtomicLong backlogDrops = new AtomicLong();

    /**
     * A single late PONG used to disconnect immediately, which tore down the whole
     * video stream. The console answers late whenever it is busy encoding a big
     * frame, so tolerate a few misses in a row and only give up when the console
     * has really gone quiet.
     */
    private static final int MAX_MISSED_PINGS = 3;
    private int missedPings = 0;

    /**
     * How long to wait for a PONG. Deliberately long: under a heavy title the
     * console's control thread can be descheduled for seconds, and treating that
     * as a disconnect tears down a working video stream for no reason. Combined
     * with MAX_MISSED_PINGS this tolerates ~15s of unresponsiveness before giving
     * up, which is still far quicker than a human would notice a real hang.
     */
    private static final int CONTROL_TIMEOUT_MS = 5000;

    public ImageStreamer(String ip) throws SocketException {
        tcpClient = new TCPClient(ip, StreamProtocol.TCP_PORT, CONTROL_TIMEOUT_MS);
        udpClient = new UDPClient(StreamProtocol.UDP_PORT);
        assembler = new FrameAssembler(this::onFrameAssembled);

        udpClient.setOnDataCallback(assembler::accept);
        startDaemon("UDPClient", udpClient);
        startDaemon("Decoder", this::decodeLoop);
        startDaemon("Heartbeat", this::heartbeatLoop);
        startDaemon("Stats", this::statsLoop);
    }

    private static void startDaemon(String name, Runnable body) {
        final Thread t = new Thread(body, name);
        // Daemon threads so closing the window actually exits the process.
        t.setDaemon(true);
        t.start();
    }

    private void onFrameAssembled(Frame frame) {
        // Called on the UDP thread - never block it. Latest-frame-wins: if the
        // decoder is behind, throw away the frame it has not started yet and keep
        // the newest, so latency never accumulates.
        if (!decodeQueue.offer(frame)) {
            if (decodeQueue.poll() != null) {
                backlogDrops.incrementAndGet();
            }
            decodeQueue.offer(frame);
        }
    }

    private void decodeLoop() {
        while (true) {
            final Frame frame;
            try {
                frame = decodeQueue.take();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
            try {
                final long t0 = System.nanoTime();
                final BufferedImage image = decode(frame);
                if (image != null) {
                    decodeNanosThisSecond.addAndGet(System.nanoTime() - t0);
                    decodedThisSecond.incrementAndGet();
                    imageProvider.updateImage(image);
                }
            } catch (RuntimeException | Error e) {
                // ImageIO's JPEG reader throws unchecked on malformed SOF/SOS data.
                // Letting that escape would end this thread for good and freeze the
                // stream with no error anywhere.
                log.log(Level.WARNING, "Failed to decode a frame", e);
            }
        }
    }

    /** Turns a received frame into an image based on what its header declares it to be. */
    private BufferedImage decode(Frame frame) {
        switch (frame.compressionType) {
            case StreamProtocol.COMP_JPEG:
                return Utilities.byteArrayToImage(frame.payload);
            case StreamProtocol.COMP_RAW:
                return decodeRaw(frame);
            default:
                logOncePerSecond("Unsupported compression type " + frame.compressionType
                        + " - client is older than the plugin.");
                return null;
        }
    }

    /** Unpacks a tightly-strided RGB888 / RGBA8888 frame into a BufferedImage. */
    private static BufferedImage decodeRaw(Frame frame) {
        final int bpp = StreamProtocol.bytesPerPixel(frame.pixelFormat);
        // FrameAssembler already validates COMP_RAW geometry, but decodeRaw indexes
        // the payload directly, so re-check here rather than trust the caller: a
        // bad stride/height would be an out-of-bounds read.
        if (bpp == 0 || frame.width <= 0 || frame.height <= 0 || frame.stride < frame.width * bpp
                || (long) frame.height * frame.stride > frame.payload.length) {
            return null;
        }
        final int w = frame.width;
        final int h = frame.height;
        final int stride = frame.stride;
        final byte[] src = frame.payload;
        final int[] argb = new int[w * h];
        for (int y = 0; y < h; y++) {
            int si = y * stride;
            int di = y * w;
            for (int x = 0; x < w; x++, si += bpp) {
                final int r = src[si] & 0xFF;
                final int g = src[si + 1] & 0xFF;
                final int b = src[si + 2] & 0xFF;
                argb[di + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
        final BufferedImage image = new BufferedImage(w, h, BufferedImage.TYPE_INT_RGB);
        image.setRGB(0, 0, w, h, argb, 0, w);
        return image;
    }

    private long lastUnsupportedLog = 0;

    private void logOncePerSecond(String message) {
        final long now = System.nanoTime();
        if (now - lastUnsupportedLog > 1_000_000_000L) {
            lastUnsupportedLog = now;
            log.warning(message);
        }
    }

    private void heartbeatLoop() {
        while (true) {
            if (!tcpClient.isConnected()) {
                log.info("Connecting to the Wii U...");
                try {
                    tcpClient.connect();
                    // Now that we know the console's address, refuse video from
                    // anywhere else.
                    udpClient.setExpectedSource(tcpClient.getRemoteAddress());
                    // The console's frame counter restarts whenever the plugin is
                    // reloaded, so a reconnect needs a fresh view of frame ids.
                    assembler.requestReset();
                    missedPings = 0;
                    log.info("Connected.");
                } catch (IllegalArgumentException | UnknownHostException e1) {
                    // Swing dialogs belong on the EDT; this runs on the heartbeat thread.
                    SwingUtilities.invokeLater(() -> {
                        JOptionPane.showMessageDialog(null, "Make sure to enter a valid ip address.",
                                e1.getClass().getName(), JOptionPane.WARNING_MESSAGE);
                        System.exit(-1);
                    });
                    return;
                } catch (SocketTimeoutException e) {
                    log.info("Timed out. Is the plugin running and the console awake?");
                } catch (IOException e) {
                    log.info("Connect failed: " + e.getMessage());
                }
            } else {
                sendPing();
            }
            sleep(1000);
        }
    }

    private void statsLoop() {
        while (true) {
            sleep(1000);
            if (!tcpClient.isConnected()) {
                continue;
            }

            final long completed = assembler.getFramesCompleted();
            final long bytes = assembler.getBytesReceived();
            final long framesThisSecond = completed - lastFramesCompleted;
            final long bytesThisSecond = bytes - lastBytesReceived;
            lastFramesCompleted = completed;
            lastBytesReceived = bytes;

            final int decoded = decodedThisSecond.getAndSet(0);
            final long decodeNanos = decodeNanosThisSecond.getAndSet(0);
            final double avgDecodeMs = decoded > 0 ? (decodeNanos / (double) decoded) / 1_000_000.0 : 0.0;

            log.info(String.format(
                    "recv %d fps | disp %d fps (decode %.1f ms) | %.2f Mbit/s | incomplete=%d crcfail=%d "
                            + "ignored=%d backlogdrop=%d wrongsrc=%d",
                    framesThisSecond, decoded, avgDecodeMs, (bytesThisSecond * 8.0) / 1_000_000.0,
                    assembler.getFramesIncomplete(), assembler.getFramesCrcFailed(), assembler.getDatagramsIgnored(),
                    backlogDrops.get(), udpClient.getWrongSourceDiscards()));

            if (framesThisSecond == 0 && assembler.getDatagramsIgnored() > 0 && completed == 0) {
                log.warning("Connected but no valid frames yet. If the Wii U is running the original "
                        + "plugin rather than this fork, the wire protocols do not match - see PROTOCOL.md.");
            }
        }
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private boolean sendTCP(byte[] rawCommand) {
        try {
            tcpClient.send(rawCommand);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    void sendPing() {
        if (!sendTCP(new byte[] { StreamProtocol.PING })) {
            // The socket itself is broken - no point tolerating that.
            log.info("Sending the PING failed. Disconnecting.");
            tcpClient.abort();
            return;
        }

        try {
            final byte pong = tcpClient.recvByte();
            if (pong == StreamProtocol.PONG) {
                missedPings = 0;
                return;
            }
            onMissedPong("got a non-PONG byte (0x" + Integer.toHexString(pong & 0xFF) + ")");
        } catch (SocketTimeoutException e) {
            // The usual case: the console is mid-encode and answered late. Keep the
            // video flowing and try again next second.
            onMissedPong("timed out waiting for PONG");
        } catch (IOException e) {
            // A real I/O error on the control socket is not just slowness.
            log.info("Control socket error, disconnecting: " + e.getMessage());
            tcpClient.abort();
        }
    }

    private void onMissedPong(String why) {
        missedPings++;
        if (missedPings >= MAX_MISSED_PINGS) {
            log.info("No PONG after " + missedPings + " tries (" + why + "). Disconnecting.");
            tcpClient.abort();
            missedPings = 0;
        }
    }

    public IImageProvider getImageProvider() {
        return imageProvider;
    }
}
