package org.lighttouch.android;

/** Run with javac/java; no Android runtime, device, or guest is involved. */
public final class FailureEvidenceContractTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        AssertionError original = new AssertionError("required concurrent interval absent");
        IllegalStateException preexisting = new IllegalStateException("earlier failure");
        original.addSuppressed(preexisting);
        AssertionError screenshot = new AssertionError("screenshot failed");
        IllegalStateException status = new IllegalStateException("status failed");
        StringBuilder order = new StringBuilder();
        try {
            try {
                throw original;
            } catch (Throwable failure) {
                FailureEvidence.capture(failure,
                        () -> { order.append('a'); throw screenshot; },
                        () -> { order.append('b'); throw failure; },
                        () -> { order.append('c'); throw status; },
                        () -> order.append('d'));
                throw failure;
            }
        } catch (Throwable caught) {
            check(caught == original, "diagnostics replaced the original assertion");
        }
        check(order.toString().equals("abcd"), "a failed capture skipped later evidence");
        Throwable[] suppressed = original.getSuppressed();
        check(suppressed.length == 3, "suppression count/self-suppression changed");
        check(suppressed[0] == preexisting && suppressed[1] == screenshot && suppressed[2] == status,
                "original and diagnostic exceptions lost their order/identity");

        AssertionError clean = new AssertionError("still a failed test");
        FailureEvidence.capture(clean, () -> order.append('e'));
        check(clean.getSuppressed().length == 0 && order.toString().equals("abcde"),
                "successful capture changed failure state");
        System.out.println("FailureEvidence contract PASS: original identity, all attempts, ordered suppression, self-suppression, success path");
    }
}
