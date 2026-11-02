package org.lighttouch.android;

/** Executes the same test-only admission class used by GuestUnlockTest. */
final class UnlockReadinessHostTest {
    static final Object VIEW = new Object();
    static GuestUnlockReadiness.Viewport snapshot(Object view, int width, int height,
            long input, long surface, boolean awake, boolean usable) {
        return new GuestUnlockReadiness.Viewport(view, width, height, input, surface, awake, usable);
    }
    static GuestUnlockReadiness.Viewport ready() {
        return snapshot(VIEW, 996, 1374, 1, 14, true, true);
    }
    static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    static void refused(Runnable action) {
        try { action.run(); } catch (IllegalStateException expected) { return; }
        throw new AssertionError("Expected bounded refusal");
    }
    public static void main(String[] args) {
        GuestUnlockReadiness gate = new GuestUnlockReadiness(1000);
        GuestUnlockReadiness.Viewport first = ready();
        switch (args[0]) {
        case "poll-window":
            check(!gate.observe(1000, first));
            check(!gate.observe(1500, first)); // A single poll interval is insufficient.
            check(!gate.observe(1749, first));
            check(gate.observe(1750, first));
            break;
        case "sparse":
            check(!gate.observe(1000, first));
            check(!gate.observe(1750, first)); // Need at least three observations.
            check(gate.observe(1800, first));
            break;
        case "banner-reflow":
            check(!gate.observe(1000, first));
            check(!gate.observe(1500, snapshot(VIEW, 996, 1437, 1, 15, true, true)));
            check(!gate.observe(1750, snapshot(VIEW, 996, 1437, 1, 15, true, true)));
            check(!gate.observe(2249, snapshot(VIEW, 996, 1437, 1, 15, true, true)));
            check(gate.observe(2250, snapshot(VIEW, 996, 1437, 1, 15, true, true)));
            break;
        case "sleeping-ui":
        case "unusable":
            check(!gate.observe(1000, first));
            check(!gate.observe(1700, snapshot(VIEW, 996, 1374, 1, 14,
                !args[0].equals("sleeping-ui"), !args[0].equals("unusable"))));
            check(!gate.observe(1750, first));
            check(!gate.observe(2450, first));
            check(gate.observe(2500, first));
            break;
        case "identity":
        case "input-generation":
        case "surface-generation":
        case "width":
        case "height":
            GuestUnlockReadiness.Viewport changed = snapshot(
                args[0].equals("identity") ? new Object() : VIEW,
                args[0].equals("width") ? 995 : 996,
                args[0].equals("height") ? 1373 : 1374,
                args[0].equals("input-generation") ? 2 : 1,
                args[0].equals("surface-generation") ? 15 : 14, true, true);
            check(!first.sameViewport(changed)); // Same rule guards every continuation.
            check(!gate.observe(1000, first));
            check(!gate.observe(1700, changed));
            check(!gate.observe(1750, changed));
            check(gate.observe(2450, changed));
            break;
        case "invalid":
            for (GuestUnlockReadiness.Viewport value : new GuestUnlockReadiness.Viewport[] {
                snapshot(null,996,1374,1,14,true,true), snapshot(VIEW,0,1374,1,14,true,true),
                snapshot(VIEW,996,0,1,14,true,true), snapshot(VIEW,996,1374,0,14,true,true),
                snapshot(VIEW,996,1374,1,0,true,true) }) check(!gate.observe(1000, value));
            break;
        case "deadline":
            check(!gate.observe(1000, first));
            refused(() -> gate.observe(4000, first)); // Original bound never renews.
            break;
        case "clock":
            check(!gate.observe(1500, first));
            refused(() -> gate.observe(1499, first));
            break;
        default: throw new AssertionError("Unknown scenario");
        }
        System.out.println("PASS " + args[0]);
    }
}
