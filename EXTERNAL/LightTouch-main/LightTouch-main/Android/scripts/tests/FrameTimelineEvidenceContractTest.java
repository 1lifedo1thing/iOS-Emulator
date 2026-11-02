package org.lighttouch.android;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicInteger;

public final class FrameTimelineEvidenceContractTest {
    private static int checks;
    private interface Attempt { void run() throws Exception; }
    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }
    private static void rejected(Attempt attempt) throws Exception {
        boolean rejected = false;
        try { attempt.run(); } catch (IOException | IllegalArgumentException expected) { rejected = true; }
        check(rejected, "invalid or unbounded input was accepted");
    }
    private static ByteArrayInputStream bytes(String text) {
        return new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8));
    }

    public static void main(String[] args) throws Exception {
        FrameTimelineEvidence.Timeline before = FrameTimelineEvidence.parse(
                "4294967294 100 1 1000\n4294967295 200 0 0\n");
        FrameTimelineEvidence.Timeline after = FrameTimelineEvidence.parse(
                "4294967295 200 0 0\n0 300 1 2000\n1 400 0 0\n");
        FrameTimelineEvidence.Delta delta = FrameTimelineEvidence.compare(before, after);
        check(delta.comparable && delta.periods == 2 && delta.newBaseLatches == 1,
                "uint32 sequence wrap or overlapping-window attribution failed");
        delta = FrameTimelineEvidence.compare(after, after);
        check(delta.comparable && delta.periods == 0, "identical samples were not preserved as zero progress");
        check(!FrameTimelineEvidence.compare(after, FrameTimelineEvidence.parse("0 1 1 1000\n")).comparable,
                "a reset was reported as zero progress");
        check(!FrameTimelineEvidence.compare(before, FrameTimelineEvidence.parse("100 10000 0 0\n")).comparable,
                "a ring overrun was reported as zero progress");
        check(!FrameTimelineEvidence.compare(before, FrameTimelineEvidence.parse("4294967295 200 1 3000\n")).comparable,
                "a changed overlapping record was accepted");
        check(FrameTimelineEvidence.parse("").size() == 0, "valid empty timeline was rejected");
        check(!FrameTimelineEvidence.compare(before, FrameTimelineEvidence.parse("")).comparable,
                "unavailable history was reported as measured zero");

        for (String invalid : new String[] {
                "1 1 0 0", "1 1 0 0\n\n", "1 1 2 0\n", "1 1 0 8\n", "1 1 1 -1\n",
                "4294967296 1 0 0\n", "1 1 1 4294967296\n", "1 -1 0 0\n", "1 +1 0 0\n",
                "1 9223372036854775808 0 0\n", "1 1 0 0\n3 2 0 0\n", "1 2 0 0\n2 1 0 0\n",
                "1  1 0 0\n", "1\t1 0 0\n", "1 1 0 0 \n", "\u0661 1 0 0\n" }) {
            rejected(() -> FrameTimelineEvidence.parse(invalid));
        }
        StringBuilder fullRing = new StringBuilder();
        for (int i = 0; i < FrameTimelineEvidence.MAX_ENTRIES; i++) fullRing.append(i).append(' ').append(i).append(" 0 0\n");
        check(FrameTimelineEvidence.parse(fullRing.toString()).size() == 4096, "actual maximum ring does not fit");
        fullRing.append("4096 4096 0 0\n");
        rejected(() -> FrameTimelineEvidence.parse(fullRing.toString()));
        rejected(() -> FrameTimelineEvidence.parse("x".repeat(FrameTimelineEvidence.MAX_TIMELINE_BYTES + 1)));

        check(FrameTimelineEvidence.readLine(bytes("{\"QMP\":{}}\nunused\n"), () -> 1, 2).equals("{\"QMP\":{}}"),
                "line framing consumed or changed the response");
        rejected(() -> FrameTimelineEvidence.readLine(bytes("partial"), () -> 1, 2));
        rejected(() -> FrameTimelineEvidence.readLine(bytes("x".repeat(FrameTimelineEvidence.MAX_LINE_BYTES) + "\n"), () -> 1, 2));
        rejected(() -> FrameTimelineEvidence.readLine(new ByteArrayInputStream(new byte[]{(byte)0xc0, (byte)0xaf, '\n'}), () -> 1, 2));
        rejected(() -> FrameTimelineEvidence.readLine(bytes("{}\n"), () -> 2, 2));
        AtomicInteger clock = new AtomicInteger();
        rejected(() -> FrameTimelineEvidence.readLine(bytes("x".repeat(200) + "\n"), clock::incrementAndGet, 3));
        check(clock.get() == 3, "partial reads renewed the absolute deadline");

        FrameTimelineEvidence.Clock realClock = () -> System.nanoTime() / 1_000_000;
        AtomicInteger closes = new AtomicInteger();
        try (FrameTimelineEvidence.ClosingDeadline ignored = new FrameTimelineEvidence.ClosingDeadline(
                closes::incrementAndGet, realClock, realClock.millis() + 10_000)) { }
        check(closes.get() == 0, "completed observation closed a later resource");
        // Exercise a real blocked stream on loopback. No ADB, guest, Android,
        // remote host, or privileged operation is involved.
        try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getLoopbackAddress());
             Socket client = new Socket(InetAddress.getLoopbackAddress(), server.getLocalPort());
             Socket receiver = server.accept()) {
            long start = realClock.millis();
            try (FrameTimelineEvidence.ClosingDeadline ignored = new FrameTimelineEvidence.ClosingDeadline(
                    receiver, realClock, start + 120)) {
                rejected(() -> receiver.getInputStream().read());
            }
            check(realClock.millis() - start < 2_000, "blocked I/O did not terminate at its deadline");
        }
        check(Thread.getAllStackTraces().keySet().stream().noneMatch(thread ->
                        thread.isAlive() && thread.getName().equals("frame-diagnostic-deadline")),
                "diagnostic guardian survived cleanup");
        System.out.println("FrameTimelineEvidence contract PASS: " + checks + " assertions");
    }
}
