/*******************************************************************************
 * Copyright (c) 2017,2018 Ash (QuarkTheAwesome) & Maschell
 * Taken from the HID to VPAD Networkclient. Modified for the  StreamingPluginClient.
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

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.logging.Logger;

final public class TCPClient {
    private static final Logger log = Logger.getLogger(TCPClient.class.getName());

    private final Object lock = new Object();

    private Socket sock;
    private DataInputStream in;
    private DataOutputStream out;

    private final String ip;
    private final int port;
    private final int timeout;

    public TCPClient(String ip, int port, int timeout) {
        this.ip = ip;
        this.port = port;
        this.timeout = timeout;
    }

    public void connect() throws IOException {
        synchronized (lock) {
            final Socket s = new Socket();
            try {
                s.connect(new InetSocketAddress(ip, port), timeout);
                // Without a read timeout a Wii U that stops answering (crashed game,
                // plugin torn down) parks the heartbeat thread in readByte() forever
                // and the client never notices it is disconnected.
                s.setSoTimeout(Math.max(timeout, 2000));
                s.setTcpNoDelay(true);

                in = new DataInputStream(s.getInputStream());
                out = new DataOutputStream(s.getOutputStream());
                sock = s;
            } catch (IOException | RuntimeException e) {
                // The heartbeat thread retries once a second forever. Leaking the
                // socket on each failed attempt eventually exhausts the process's
                // file descriptors.
                try {
                    s.close();
                } catch (IOException ignored) {
                    // nothing useful to do
                }
                throw e;
            }
        }
    }

    /** The console's address, or null while disconnected. */
    public InetAddress getRemoteAddress() {
        synchronized (lock) {
            return sock != null ? sock.getInetAddress() : null;
        }
    }

    public boolean abort() {
        synchronized (lock) {
            final Socket s = sock;
            sock = null;
            in = null;
            out = null;
            if (s == null) {
                return true;
            }
            try {
                s.close();
            } catch (IOException e) {
                log.info("Failed to close socket: " + e.getMessage());
                return false;
            }
            return true;
        }
    }

    public void send(byte[] rawCommand) throws IOException {
        synchronized (lock) {
            if (out == null) {
                throw new IOException("not connected");
            }
            out.write(rawCommand);
            out.flush();
        }
    }

    void send(int value) throws IOException {
        send(ByteBuffer.allocate(4).putInt(value).array());
    }

    public void send(byte _byte) throws IOException {
        send(new byte[] { _byte });
    }

    public byte recvByte() throws IOException {
        synchronized (lock) {
            if (in == null) {
                throw new IOException("not connected");
            }
            return in.readByte();
        }
    }

    short recvShort() throws IOException {
        synchronized (lock) {
            if (in == null) {
                throw new IOException("not connected");
            }
            return in.readShort();
        }
    }

    int recvInt() throws IOException {
        synchronized (lock) {
            if (in == null) {
                throw new IOException("not connected");
            }
            return in.readInt();
        }
    }

    public boolean isConnected() {
        synchronized (lock) {
            return (sock != null && sock.isConnected() && !sock.isClosed());
        }
    }
}
