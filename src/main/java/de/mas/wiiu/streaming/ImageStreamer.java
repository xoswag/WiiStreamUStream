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
import java.util.logging.Level;
import java.util.logging.Logger;

import javax.swing.JOptionPane;
import javax.swing.SwingUtilities;

import de.mas.wiiu.streaming.gui.IImageProvider;
import de.mas.wiiu.streaming.gui.ImageProvider;
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
    private final BlockingQueue<byte[]> decodeQueue = new ArrayBlockingQueue<>(1);

    private long lastFramesCompleted = 0;
    private long lastBytesReceived = 0;
    // Incremented on the decoder thread, read-and-cleared on the stats thread, so
    // it needs the atomic read-modify-write rather than just a visible write.
    private final AtomicInteger decodedThisSecond = new AtomicInteger();

    public ImageStreamer(String ip) throws SocketException {
        tcpClient = new TCPClient(ip, StreamProtocol.TCP_PORT, 2000);
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

    private void onFrameAssembled(byte[] jpeg) {
        // Called on the UDP thread - never block it.
        if (!decodeQueue.offer(jpeg)) {
            decodeQueue.poll();
            decodeQueue.offer(jpeg);
        }
    }

    private void decodeLoop() {
        while (true) {
            final byte[] jpeg;
            try {
                jpeg = decodeQueue.take();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
            try {
                final BufferedImage image = Utilities.byteArrayToImage(jpeg);
                if (image != null) {
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

            log.info(String.format("%d fps (%d decoded)  %.2f Mbit/s  incomplete=%d crcfail=%d ignored=%d",
                    framesThisSecond, decoded, (bytesThisSecond * 8.0) / 1_000_000.0, assembler.getFramesIncomplete(),
                    assembler.getFramesCrcFailed(), assembler.getDatagramsIgnored()));

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
        if (sendTCP(new byte[] { StreamProtocol.PING })) {
            try {
                final byte pong = tcpClient.recvByte();
                if (pong != StreamProtocol.PONG) {
                    log.info("Got no valid response to a Ping. Disconnecting.");
                    tcpClient.abort();
                }
            } catch (IOException e) {
                log.info("Failed to get PONG. Disconnecting.");
                tcpClient.abort();
            }
        } else {
            log.info("Sending the PING failed. Disconnecting.");
            tcpClient.abort();
        }
    }

    public IImageProvider getImageProvider() {
        return imageProvider;
    }
}
