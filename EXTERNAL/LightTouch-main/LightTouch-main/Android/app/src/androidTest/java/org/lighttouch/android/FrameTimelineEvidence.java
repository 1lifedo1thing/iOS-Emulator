package org.lighttouch.android;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.IOException;
import java.io.InputStream;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Fixed-size, read-only frame-timeline decoding; usable by host JVM contract tests. */
final class FrameTimelineEvidence {
    static final int MAX_LINE_BYTES = 262_144;
    static final int MAX_TIMELINE_BYTES = 196_608;
    static final int MAX_ENTRIES = 4_096;
    interface Clock { long millis(); }

    static void beforeDeadline(Clock clock, long deadline) throws IOException {
        if (clock.millis() >= deadline) throw new SocketTimeoutException("Frame diagnostic deadline expired");
    }

    // The caller supplies a buffered stream. The deadline never renews on
    // events, partial reads, or subsequent QMP requests in the same snapshot.
    static String readLine(InputStream input, Clock clock, long deadline) throws IOException {
        ByteArrayOutputStream line = new ByteArrayOutputStream();
        for (int i = 0; i < MAX_LINE_BYTES; i++) {
            if ((i & 31) == 0) beforeDeadline(clock, deadline);
            int value = input.read();
            if (value < 0) throw new IOException("Incomplete frame diagnostic response");
            if (value == '\n') {
                beforeDeadline(clock, deadline);
                return StandardCharsets.UTF_8.newDecoder()
                        .onMalformedInput(CodingErrorAction.REPORT)
                        .onUnmappableCharacter(CodingErrorAction.REPORT)
                        .decode(ByteBuffer.wrap(line.toByteArray())).toString();
            }
            line.write(value);
        }
        throw new IOException("Oversized frame diagnostic response");
    }

    /** Closes a socket at the same absolute deadline, including during connect/read. */
    static final class ClosingDeadline implements AutoCloseable {
        private final CountDownLatch completed = new CountDownLatch(1);
        private final Thread guardian;
        ClosingDeadline(Closeable socket, Clock clock, long deadline) {
            guardian = new Thread(() -> {
                try {
                    long remaining = deadline - clock.millis();
                    if (remaining > 0 && completed.await(remaining, TimeUnit.MILLISECONDS)) return;
                    socket.close();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                } catch (IOException ignored) {
                    // The caller's I/O failure and transcript remain authoritative.
                }
            }, "frame-diagnostic-deadline");
            guardian.setDaemon(true);
            guardian.start();
        }
        @Override public void close() throws InterruptedException, IOException {
            completed.countDown();
            guardian.join(100);
            if (guardian.isAlive()) throw new IOException("Frame diagnostic deadline thread did not retire");
        }
    }

    static final class Timeline {
        final long[] sequence, virtualNs, key;
        final boolean[] newBase;
        Timeline(int count) {
            sequence = new long[count]; virtualNs = new long[count];
            key = new long[count]; newBase = new boolean[count];
        }
        int size() { return sequence.length; }
    }

    private static long decimal(String value, long maximum) {
        if (value.isEmpty() || value.length() > 19) throw new IllegalArgumentException("Invalid timeline number");
        for (int i = 0; i < value.length(); i++)
            if (value.charAt(i) < '0' || value.charAt(i) > '9') throw new IllegalArgumentException("Invalid timeline number");
        long parsed = Long.parseLong(value);
        if (parsed > maximum) throw new IllegalArgumentException("Timeline number outside range");
        return parsed;
    }

    static Timeline parse(String text) {
        if (text.length() > MAX_TIMELINE_BYTES || (!text.isEmpty() && !text.endsWith("\n")))
            throw new IllegalArgumentException("Oversized or incomplete frame timeline");
        if (text.isEmpty()) return new Timeline(0);
        String[] lines = text.split("\n", -1);
        int count = lines.length - 1;
        if (count > MAX_ENTRIES) throw new IllegalArgumentException("Too many frame timeline entries");
        Timeline result = new Timeline(count);
        for (int i = 0; i < count; i++) {
            String[] fields = lines[i].split(" ", -1);
            if (fields.length != 4) throw new IllegalArgumentException("Invalid frame timeline row");
            result.sequence[i] = decimal(fields[0], 0xffff_ffffL);
            result.virtualNs[i] = decimal(fields[1], Long.MAX_VALUE);
            result.newBase[i] = decimal(fields[2], 1) == 1;
            result.key[i] = decimal(fields[3], 0xffff_ffffL);
            if (!result.newBase[i] && result.key[i] != 0) throw new IllegalArgumentException("Held timeline entry has a key");
            if (i > 0 && (((result.sequence[i] - result.sequence[i - 1]) & 0xffff_ffffL) != 1 ||
                    result.virtualNs[i] < result.virtualNs[i - 1]))
                throw new IllegalArgumentException("Frame timeline order changed");
        }
        return result;
    }

    static final class Delta {
        final boolean comparable;
        final String reason;
        final int periods, newBaseLatches;
        Delta(boolean comparable, String reason, int periods, int newBaseLatches) {
            this.comparable = comparable; this.reason = reason;
            this.periods = periods; this.newBaseLatches = newBaseLatches;
        }
    }

    static Delta compare(Timeline before, Timeline after) {
        if (before.size() == 0 || after.size() == 0) return new Delta(false, "empty timeline", 0, 0);
        int last = before.size() - 1;
        for (int i = after.size() - 1; i >= 0; i--) {
            if (after.sequence[i] != before.sequence[last] || after.virtualNs[i] != before.virtualNs[last]) continue;
            if (after.key[i] != before.key[last] || after.newBase[i] != before.newBase[last])
                return new Delta(false, "overlapping timeline entry changed", 0, 0);
            int changes = 0;
            for (int j = i + 1; j < after.size(); j++) if (after.newBase[j]) changes++;
            return new Delta(true, "exact overlapping entry", after.size() - i - 1, changes);
        }
        // A reset, ring overrun, or unmatched sample is not zero frame progress.
        return new Delta(false, "no exact overlapping entry", 0, 0);
    }
}
