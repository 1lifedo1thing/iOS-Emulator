package org.lighttouch.android;

/** Test-only admission before DOWN; never changes the production input contract. */
final class GuestUnlockReadiness {
    static final long STABLE_MS = 750; // Longer than MainActivity's 500ms status poll.
    static final long DEADLINE_MS = 3000;

    static final class Viewport {
        final Object view;
        final int width, height;
        final long inputGeneration, surfaceGeneration;
        final boolean awakeUi, usable;

        Viewport(Object view, int width, int height, long inputGeneration,
                 long surfaceGeneration, boolean awakeUi, boolean usable) {
            this.view = view;
            this.width = width;
            this.height = height;
            this.inputGeneration = inputGeneration;
            this.surfaceGeneration = surfaceGeneration;
            this.awakeUi = awakeUi;
            this.usable = usable;
        }

        boolean ready() {
            return view != null && width > 0 && height > 0 && inputGeneration > 0
                    && surfaceGeneration > 0 && awakeUi && usable;
        }

        boolean sameViewport(Viewport other) {
            return other != null && view == other.view && width == other.width
                    && height == other.height && inputGeneration == other.inputGeneration
                    && surfaceGeneration == other.surfaceGeneration;
        }
    }

    private final long began;
    private long lastTime, stableSince;
    private Viewport previous;
    private int samples;

    GuestUnlockReadiness(long began) {
        this.began = began;
        lastTime = began;
    }

    boolean observe(long now, Viewport viewport) {
        if (now < lastTime || now - began >= DEADLINE_MS)
            throw new IllegalStateException("Stable awake UI/viewport admission deadline expired or clock regressed");
        lastTime = now;
        if (!viewport.ready()) {
            previous = null;
            samples = 0;
            return false;
        }
        if (!viewport.sameViewport(previous)) {
            previous = viewport;
            stableSince = now;
            samples = 1;
        } else {
            samples++;
        }
        return samples >= 3 && now - stableSince >= STABLE_MS;
    }
}
