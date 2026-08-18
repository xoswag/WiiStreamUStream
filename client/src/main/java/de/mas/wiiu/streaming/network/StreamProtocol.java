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
 * Constants shared with the Wii U plugin. Keep in sync with plugin/src/StreamProtocol.h
 * and with PROTOCOL.md.
 *
 * v3 makes every datagram fully self-describing: timestamp, width, height, stride,
 * compression type and pixel format now travel in the header, so the client renders
 * whatever each frame declares instead of assuming JPEG at a fixed size.
 */
public final class StreamProtocol {
    private StreamProtocol() {
    }

    /** 'W' 'U' 'S' '3' */
    public static final int MAGIC = 0x57555333;

    public static final int VERSION = 3;

    /** Bytes of header in front of every chunk payload. */
    public static final int HEADER_SIZE = 44;

    /** Largest payload the Wii U will put in one datagram. */
    public static final int MAX_PAYLOAD = 1376;

    /** Largest datagram we can ever receive. */
    public static final int MAX_DATAGRAM = HEADER_SIZE + MAX_PAYLOAD;

    // flags
    public static final int FLAG_LAST_CHUNK = 0x01;
    public static final int FLAG_KEYFRAME = 0x02;

    // compressionType
    public static final int COMP_RAW = 0;
    public static final int COMP_LIGHTWEIGHT = 1;
    public static final int COMP_JPEG = 2;

    // pixelFormat
    public static final int PIXFMT_JPEG = 0;
    public static final int PIXFMT_RGB888 = 1;
    public static final int PIXFMT_RGBA8888 = 2;

    /**
     * Hard ceiling on a single frame. A raw RGBA 1280x720 frame is 3.69 MB and a
     * 720p JPEG is well under 1 MB; 8 MB is generous and still small enough that a
     * corrupt length field can never make us allocate something dangerous.
     */
    public static final int MAX_FRAME_BYTES = 8 * 1024 * 1024;

    // Header field offsets (big-endian on the wire).
    public static final int OFF_MAGIC = 0;
    public static final int OFF_FRAME_ID = 4;
    public static final int OFF_TIMESTAMP_US = 8;
    public static final int OFF_FRAME_SIZE = 16;
    public static final int OFF_CHUNK_OFFSET = 20;
    public static final int OFF_FRAME_CRC = 24;
    public static final int OFF_WIDTH = 28;
    public static final int OFF_HEIGHT = 30;
    public static final int OFF_STRIDE = 32;
    public static final int OFF_CHUNK_LEN = 36;
    public static final int OFF_FLAGS = 38;
    public static final int OFF_VERSION = 39;
    public static final int OFF_COMPRESSION = 40;
    public static final int OFF_PIXEL_FORMAT = 41;

    public static final int TCP_PORT = 8092;
    public static final int UDP_PORT = 9445;

    public static final byte PING = 0x15;
    public static final byte PONG = 0x16;

    /** Bytes per pixel for a raw pixel format, or 0 if it is not a raw layout. */
    public static int bytesPerPixel(int pixelFormat) {
        switch (pixelFormat) {
            case PIXFMT_RGB888:
                return 3;
            case PIXFMT_RGBA8888:
                return 4;
            default:
                return 0;
        }
    }
}
