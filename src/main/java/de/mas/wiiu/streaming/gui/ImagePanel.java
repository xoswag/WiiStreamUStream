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

package de.mas.wiiu.streaming.gui;

import java.awt.Color;
import java.awt.Dimension;
import java.awt.Graphics;
import java.awt.Graphics2D;
import java.awt.Image;
import java.awt.RenderingHints;

import javax.swing.JPanel;

public final class ImagePanel extends JPanel {
    private static final long serialVersionUID = -127096088663141229L;

    /** Written by the decoder thread, read by the EDT. */
    private volatile Image image;

    private final Dimension preferred;

    public ImagePanel(int width, int height) {
        super(true);
        preferred = new Dimension(width, height);
        setBackground(Color.BLACK);
        setOpaque(true);
    }

    public void setImage(Image image) {
        this.image = image;
        repaint();
    }

    @Override
    public Dimension getPreferredSize() {
        return new Dimension(preferred);
    }

    @Override
    protected void paintComponent(Graphics g) {
        // The upstream version overrode paint() and stretched the frame to fill the
        // window, which distorts the picture whenever the window is not exactly 16:9.
        super.paintComponent(g);

        final Image current = image;
        if (current == null) {
            return;
        }

        final int iw = current.getWidth(this);
        final int ih = current.getHeight(this);
        if (iw <= 0 || ih <= 0) {
            return;
        }

        final int pw = getWidth();
        final int ph = getHeight();

        // Letterbox: largest centred rectangle with the frame's aspect ratio.
        final double scale = Math.min(pw / (double) iw, ph / (double) ih);
        final int dw = Math.max(1, (int) Math.round(iw * scale));
        final int dh = Math.max(1, (int) Math.round(ih * scale));
        final int dx = (pw - dw) / 2;
        final int dy = (ph - dh) / 2;

        final Graphics2D g2 = (Graphics2D) g.create();
        try {
            g2.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BILINEAR);
            g2.setRenderingHint(RenderingHints.KEY_RENDERING, RenderingHints.VALUE_RENDER_SPEED);
            g2.drawImage(current, dx, dy, dw, dh, this);
        } finally {
            g2.dispose();
        }
    }
}
