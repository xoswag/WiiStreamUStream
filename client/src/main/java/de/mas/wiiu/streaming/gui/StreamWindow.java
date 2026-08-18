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

import java.awt.BorderLayout;
import java.awt.Dimension;
import java.awt.GraphicsEnvironment;
import java.awt.Rectangle;

import javax.swing.JFrame;
import javax.swing.JMenu;
import javax.swing.JMenuBar;
import javax.swing.JMenuItem;
import javax.swing.WindowConstants;

public class StreamWindow {
    /** The stream tops out at 720p, so that is the natural 1:1 window size. */
    private static final int NATIVE_WIDTH = 1280;
    private static final int NATIVE_HEIGHT = 720;

    private final ImagePanel image;

    public StreamWindow(IImageProvider imageProvider) {
        final Dimension size = defaultSize();
        image = new ImagePanel(size.width, size.height);

        final JFrame frame = new JFrame("Wii U Streaming Client");
        frame.setDefaultCloseOperation(WindowConstants.EXIT_ON_CLOSE);

        imageProvider.setOnImageChange(image::setImage);

        frame.setJMenuBar(buildMenuBar());
        frame.getContentPane().setLayout(new BorderLayout());
        frame.getContentPane().add(image, BorderLayout.CENTER);

        frame.pack();
        frame.setMinimumSize(new Dimension(320, 180));
        frame.setLocationRelativeTo(null);
        frame.setVisible(true);
    }

    /**
     * 1:1 at 720p when the display has room for it, otherwise the largest 16:9 window that
     * fits in the usable screen area. Upstream sized the panel to the whole screen minus a
     * fixed margin, which produced a window taller than the desktop on laptops.
     */
    private static Dimension defaultSize() {
        final Rectangle usable = GraphicsEnvironment.getLocalGraphicsEnvironment().getMaximumWindowBounds();
        final int maxW = Math.max(320, usable.width - 80);
        final int maxH = Math.max(180, usable.height - 120);

        final double scale = Math.min(1.0, Math.min(maxW / (double) NATIVE_WIDTH, maxH / (double) NATIVE_HEIGHT));
        return new Dimension((int) Math.round(NATIVE_WIDTH * scale), (int) Math.round(NATIVE_HEIGHT * scale));
    }

    private static JMenuBar buildMenuBar() {
        final JMenuBar menuBar = new JMenuBar();
        final JMenu mnSettings = new JMenu("Settings");
        menuBar.add(mnSettings);

        final JMenuItem mntmConfig = new JMenuItem("Config (configure on the console)");
        mntmConfig.setEnabled(false);
        mnSettings.add(mntmConfig);

        final JMenuItem mntmExit = new JMenuItem("Exit");
        mntmExit.addActionListener(e -> System.exit(0));
        mnSettings.add(mntmExit);

        return menuBar;
    }
}
