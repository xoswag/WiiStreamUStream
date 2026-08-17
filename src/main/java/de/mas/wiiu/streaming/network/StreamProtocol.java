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

/**
 * Constants shared with the Wii U plugin. Keep in sync with src/StreamProtocol.h
 * in the StreamingPluginWiiU repository, and with PROTOCOL.md.
 */
public final class StreamProtocol {
    private StreamProtocol() {
    }

    /** 'W' 'U' 'S' '2' */
    public static final int MAGIC = 0x57555332;

    public static final int VERSION = 2;

    /** Bytes of header in front of every chunk payload. */
    public static final int HEADER_SIZE = 24;

    /** Largest payload the Wii U will put in one datagram. */
    public static final int MAX_PAYLOAD = 1376;

    /** Largest datagram we can ever receive. */
    public static final int MAX_DATAGRAM = HEADER_SIZE + MAX_PAYLOAD;

    public static final int FLAG_LAST_CHUNK = 1;

    /**
     * Hard ceiling on a single frame. A 1280x720 JPEG at quality 100 is well under 1 MB;
     * 8 MB is generous and still small enough that a corrupt length field can never make
     * us allocate something dangerous.
     */
    public static final int MAX_FRAME_BYTES = 8 * 1024 * 1024;

    // Header field offsets.
    public static final int OFF_MAGIC = 0;
    public static final int OFF_FRAME_ID = 4;
    public static final int OFF_FRAME_SIZE = 8;
    public static final int OFF_CHUNK_OFFSET = 12;
    public static final int OFF_CHUNK_LEN = 16;
    public static final int OFF_FLAGS = 18;
    public static final int OFF_VERSION = 19;
    public static final int OFF_FRAME_CRC = 20;

    public static final int TCP_PORT = 8092;
    public static final int UDP_PORT = 9445;

    public static final byte PING = 0x15;
    public static final byte PONG = 0x16;
}
