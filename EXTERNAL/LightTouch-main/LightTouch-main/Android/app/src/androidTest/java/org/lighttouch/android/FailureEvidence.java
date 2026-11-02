package org.lighttouch.android;

/** Best-effort diagnostics must retain the original test failure and try every capture. */
final class FailureEvidence {
    private FailureEvidence() {}

    static void capture(Throwable original, Runnable... attempts) {
        for (Runnable attempt : attempts) {
            try {
                attempt.run();
            } catch (Throwable diagnosticFailure) {
                // Throwable forbids self-suppression. Never let diagnostics replace
                // the original assertion, including when a callback rethrows it.
                if (diagnosticFailure != original) original.addSuppressed(diagnosticFailure);
            }
        }
    }
}
