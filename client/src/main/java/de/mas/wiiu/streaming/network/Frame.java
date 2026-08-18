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
 * A fully-received frame plus the self-describing metadata from its v3 header.
 *
 * The decoder switches on {@link #compressionType} / {@link #pixelFormat} rather
 * than assuming JPEG, which is what lets the same pipeline carry JPEG today and
 * RAW or a lightweight codec later without the client being told which to expect.
 */
public final class Frame {
    public final byte[] payload;
    public final int compressionType; // StreamProtocol.COMP_*
    public final int pixelFormat;     // StreamProtocol.PIXFMT_*
    public final int width;
    public final int height;
    public final int stride;          // bytes per row for RAW, 0 for JPEG
    public final long timestampUs;    // console clock, for jitter/ordering (not synced latency)

    public Frame(byte[] payload, int compressionType, int pixelFormat, int width, int height, int stride,
            long timestampUs) {
        this.payload = payload;
        this.compressionType = compressionType;
        this.pixelFormat = pixelFormat;
        this.width = width;
        this.height = height;
        this.stride = stride;
        this.timestampUs = timestampUs;
    }
}
