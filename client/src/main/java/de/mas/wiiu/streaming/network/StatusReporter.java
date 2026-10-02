package de.mas.wiiu.streaming.network;

import java.nio.charset.StandardCharsets;
import java.util.logging.Logger;

/**
 * Logs the console's status reports.
 *
 * The console sends a few lines of plain text about once a second:
 * <ul>
 * <li>{@code settings ...} - what the plugin menu is set to</li>
 * <li>{@code state ...} - what is running (a game, the Wii U Menu, ...) and whether it is streaming</li>
 * <li>{@code perf ...} - the console's own frame rates and costs</li>
 * </ul>
 * Settings and state are logged when they change, so the log always says what a
 * stretch of numbers was measured under; perf is logged every five seconds.
 */
public final class StatusReporter {
    private static final Logger log = Logger.getLogger(StatusReporter.class.getName());

    private static final long PERF_INTERVAL_NS = 5_000_000_000L;
    /** Repeat unchanged settings/state now and then, so a pasted excerpt still contains them. */
    private static final long REPEAT_INTERVAL_NS = 60_000_000_000L;

    private String lastSettings = "";
    private String lastState = "";
    private long lastSettingsLogNs = 0;
    private long lastStateLogNs = 0;
    private long lastPerfLogNs = 0;
    private volatile long reportsReceived = 0;

    /** Called on the UDP thread with a datagram whose magic says it is a status report. */
    public synchronized void accept(byte[] data, int length) {
        if (length < StreamProtocol.STATUS_HEADER_SIZE
                || (data[StreamProtocol.STATUS_OFF_VERSION] & 0xFF) != StreamProtocol.STATUS_VERSION) {
            return;
        }
        final int textLen = ((data[StreamProtocol.STATUS_OFF_LENGTH] & 0xFF) << 8)
                | (data[StreamProtocol.STATUS_OFF_LENGTH + 1] & 0xFF);
        if (textLen <= 0 || StreamProtocol.STATUS_HEADER_SIZE + textLen > length) {
            return;
        }
        reportsReceived++;
        final String text = new String(data, StreamProtocol.STATUS_HEADER_SIZE, textLen, StandardCharsets.US_ASCII);
        final long now = System.nanoTime();
        for (String line : text.split("\n")) {
            line = line.trim();
            if (line.startsWith("settings ")) {
                if (!line.equals(lastSettings) || now - lastSettingsLogNs > REPEAT_INTERVAL_NS) {
                    lastSettings = line;
                    lastSettingsLogNs = now;
                    log.info("Wii U " + line);
                }
            } else if (line.startsWith("state ")) {
                if (!line.equals(lastState) || now - lastStateLogNs > REPEAT_INTERVAL_NS) {
                    lastState = line;
                    lastStateLogNs = now;
                    log.info("Wii U " + line);
                }
            } else if (line.startsWith("perf ")) {
                if (now - lastPerfLogNs > PERF_INTERVAL_NS) {
                    lastPerfLogNs = now;
                    log.info("Wii U " + line);
                }
            }
        }
    }

    /** Forget what was last logged, so a new session prints its settings and state again. */
    public synchronized void reset() {
        lastSettings = "";
        lastState = "";
        lastPerfLogNs = 0;
    }

    public long getReportsReceived() {
        return reportsReceived;
    }
}
