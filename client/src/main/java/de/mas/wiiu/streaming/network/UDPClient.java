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

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketException;
import java.util.logging.Level;
import java.util.logging.Logger;

public final class UDPClient implements Runnable {
    private static final Logger log = Logger.getLogger(UDPClient.class.getName());

    /**
     * A 720p frame is ~100 datagrams arriving back to back with no pacing. The default
     * receive buffer (commonly 64 KB) holds about 45 of them, so the tail of every frame
     * was being dropped by the kernel before this thread ever got a chance to run. 4 MB
     * covers several whole frames of jitter.
     */
    private static final int WANTED_RECEIVE_BUFFER = 4 * 1024 * 1024;

    private final DatagramSocket sock;

    public UDPClient(int port) throws SocketException {
        sock = new DatagramSocket(port);
        sock.setReceiveBufferSize(WANTED_RECEIVE_BUFFER);

        final int actual = sock.getReceiveBufferSize();
        if (actual < WANTED_RECEIVE_BUFFER) {
            // Not fatal, but worth saying out loud - it is the usual cause of a stream
            // that looks fine at 480p and falls apart at 720p.
            log.warning("UDP receive buffer is " + actual + " bytes, asked for " + WANTED_RECEIVE_BUFFER
                    + ". Expect dropped frames at high resolutions.");
        }
    }

    private volatile PacketHandler onDataCallback = null;

    /**
     * When set, datagrams from any other host are discarded. The socket is bound on
     * all interfaces, so without this any machine on the LAN could push frames into
     * the window, or pin memory by declaring a huge frame size.
     */
    private volatile InetAddress expectedSource = null;

    /** Datagrams dropped because they came from a host other than the connected console. */
    private final java.util.concurrent.atomic.AtomicLong wrongSourceDiscards = new java.util.concurrent.atomic.AtomicLong();

    public long getWrongSourceDiscards() {
        return wrongSourceDiscards.get();
    }

    public interface PacketHandler {
        void onPacket(byte[] data, int length);
    }

    public void setOnDataCallback(PacketHandler function) {
        onDataCallback = function;
    }

    public void setExpectedSource(InetAddress address) {
        expectedSource = address;
    }

    public void close() {
        sock.close();
    }

    @Override
    public void run() {
        log.info("UDPClient running.");
        // Reused across iterations: the handler copies out what it needs before returning.
        final byte[] receiveData = new byte[StreamProtocol.MAX_DATAGRAM];
        final DatagramPacket receivePacket = new DatagramPacket(receiveData, receiveData.length);

        while (!sock.isClosed()) {
            receivePacket.setData(receiveData, 0, receiveData.length);
            try {
                sock.receive(receivePacket);
            } catch (IOException e) {
                if (sock.isClosed()) {
                    break;
                }
                log.log(Level.FINE, "receive failed", e);
                continue;
            }

            final InetAddress expected = expectedSource;
            if (expected != null && !expected.equals(receivePacket.getAddress())) {
                wrongSourceDiscards.incrementAndGet();
                continue;
            }

            final PacketHandler cb = onDataCallback;
            if (cb != null) {
                cb.onPacket(receiveData, receivePacket.getLength());
            }
        }
        log.info("UDPClient stopped.");
    }
}
