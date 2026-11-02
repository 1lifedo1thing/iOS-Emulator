package org.lighttouch.android;

/** Test-only bounded native observations; pixel predicates do not prove visible Lock content. */
final class GuestPowerTransition {
    static final long TIMEOUT_MS = 15_000;
    static long controlDeadline(long now, long sharedDeadline) {
        return Math.min(Math.addExact(now, 5_000), sharedDeadline);
    }
    private final long began, baselineFrames, expectedStart;
    final long deadline;
    private final int expectedPid, expectedUid;
    private final boolean waking;
    private long lastTime, lastFrames;

    GuestPowerTransition(long began, int pid, int uid, long start, long frames, boolean waking) {
        if (began < 0 || pid <= 0 || uid <= 0 || start <= 0 || frames < 0)
            throw new IllegalArgumentException("Invalid power transition identity/baseline");
        this.began = began;
        this.deadline = Math.addExact(began, TIMEOUT_MS);
        expectedPid = pid; expectedUid = uid; expectedStart = start;
        baselineFrames = frames; lastFrames = frames; lastTime = began; this.waking = waking;
    }

    boolean observe(long now, int pid, int uid, long start, String state, String error,
                    boolean nativeValid, boolean diagnostics, long sleeping, long backlight,
                    long frames, long samplePixels, long nonzeroPixels, long rgbSum) {
        if (now < lastTime || now < began || now >= deadline)
            throw new IllegalStateException("Power transition original deadline expired or clock regressed");
        lastTime = now;
        if (pid != expectedPid || uid != expectedUid || start != expectedStart)
            throw new IllegalStateException("Power transition worker identity changed");
        if (!"running".equals(state) || !"".equals(error) || !nativeValid || !diagnostics)
            throw new IllegalStateException("Power transition worker is not healthy/running with native diagnostics");
        if (frames < lastFrames || (sleeping != 0 && sleeping != 1) || backlight < 0 ||
                samplePixels <= 0 || nonzeroPixels < 0 || nonzeroPixels > samplePixels || rgbSum < 0)
            throw new IllegalStateException("Power transition counter reset or invalid observations");
        lastFrames = frames;
        if (frames <= baselineFrames) return false;
        return waking ? sleeping == 0 && backlight > 0 && nonzeroPixels > 0 && rgbSum > 0
                : sleeping == 1 && backlight == 0 && nonzeroPixels == 0 && rgbSum == 0;
    }
}
