package org.lighttouch.android;

final class PowerTransitionHostTest {
    static GuestPowerTransition gate(boolean waking) { return new GuestPowerTransition(1000, 12872, 10179, 17001359, 939, waking); }
    static boolean sample(GuestPowerTransition g, long at, long sleeping, long backlight, long frames, long pixels, long sum) {
        return g.observe(at, 12872, 10179, 17001359, "running", "", true, true,
                sleeping, backlight, frames, 256, pixels, sum);
    }
    static void check(boolean ok) { if (!ok) throw new AssertionError("Unexpected admission"); }
    static void refuse(Runnable r) { try { r.run(); } catch (IllegalStateException expected) { return; } throw new AssertionError("Expected refusal"); }
    public static void main(String[] args) {
        GuestPowerTransition g = gate(true);
        switch (args[0]) {
        case "shared-control-budget":
            check(GuestPowerTransition.controlDeadline(15000, 16000) == 16000);
            check(GuestPowerTransition.controlDeadline(1000, 16000) == 6000);
            check(GuestPowerTransition.controlDeadline(16001, 16000) == 16000);
            check(GuestPowerTransition.controlDeadline(1000, Long.MAX_VALUE) == 6000);
            break;
        case "actual-black-wake":
            // Captured native sequence after old500ms wait never qualifies as awake.
            check(!sample(g, 1597, 1, 0, 940, 0, 0));
            check(!sample(g, 1649, 1, 0, 940, 0, 0));
            refuse(() -> sample(g, 16000, 1, 0, 940, 0, 0)); break;
        case "delayed-wake":
            check(!sample(g, 1600, 1, 0, 940, 0, 0));
            check(sample(g, 5000, 0, 245, 941, 129, 52578)); break;
        case "last-in-budget": check(sample(g, 15999, 0, 245, 941, 129, 52578)); break;
        case "deadline-no-renewal":
            check(!sample(g, 15999, 1, 0, 940, 0, 0));
            refuse(() -> sample(g, 16000, 0, 245, 941, 129, 52578)); break;
        case "lookup-consumes-budget": refuse(() -> sample(g, 16001, 0, 245, 941, 129, 52578)); break;
        case "stale-frame": check(!sample(g, 1200, 0, 245, 939, 129, 52578)); break;
        case "no-pixels": check(!sample(g, 1200, 0, 245, 940, 0, 0)); break;
        case "no-backlight": check(!sample(g, 1200, 0, 0, 940, 129, 52578)); break;
        case "sleep-fade":
            GuestPowerTransition sleeping = gate(false);
            check(!sample(sleeping, 1200, 1, 0, 940, 83, 208));
            check(sample(sleeping, 1400, 1, 0, 941, 0, 0)); break;
        case "frame-regression":
            check(!sample(g, 1100, 1, 0, 942, 0, 0));
            refuse(() -> sample(g, 1200, 0, 245, 941, 129, 52578)); break;
        case "clock-regression":
            check(!sample(g, 1100, 1, 0, 940, 0, 0));
            refuse(() -> sample(g, 1099, 0, 245, 941, 129, 52578)); break;
        case "identity-and-health":
            for (int n = 0; n < 8; n++) {
                final int k = n;
                refuse(() -> gate(true).observe(1200, k == 0 ? 5 : 12872, k == 1 ? 5 : 10179,
                        k == 2 ? 5 : 17001359, k == 3 ? "paused" : "running", k == 4 ? "failed" : "",
                        k != 5, k != 6, k == 7 ? 2 : 0, 245, 941, 256, 129, 52578));
            } break;
        case "missing-pixels": refuse(() -> sample(g, 1200, 0, 245, 941, 257, 52578)); break;
        default: throw new AssertionError(args[0]);
        }
        System.out.println("PASS " + args[0]);
    }
}
