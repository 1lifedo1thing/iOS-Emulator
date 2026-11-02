# Owned input after lifecycle cancellation

Full native input cancellation can occur without a View focus callback: audio
focus loss pauses the worker and releases input, then focus gain can finish
before the next UI status poll. Retaining the View's old pointer IDs in that
case permits a later MOVE/UP for a contact the guest no longer owns. Current
candidate22's historical batch356 rejection is not attributed to this path;
its retained observations report zero audio-focus losses. The second orientation
attempt failed its initial error guard before injecting any events.

The Android lease stores a positive cancellation generation. Every successful
full cancellation admission advances it; failed cancellation preserves it and
the existing release obligations. Normal `inputCompleted`/independent pointer
UP does not advance it. The owner-bound getter returns zero for a foreign or
retired lease and never grants display authority.

Only fresh DOWN reads the current generation. Every owned touch carries that
generation through Binder to JNI. Under the existing native control guard, JNI
checks both lease and generation before mapping or runtime admission. An old
generation returns the fixed expected-cancellation result, leaves current
owner/input/diagnostic untouched, and causes the View to drop local IDs and
suppress MOVE/UP until another fresh DOWN. No status polling is needed to
observe a loss/gain that finished between polls. Timestamp-failure cleanup also
checks generation under the guard before cancelling, so late old input cannot
release a newer gesture.

Expected generation/paused results increment the View's
`expectedCancellationBatches`; they do not invoke `onError`, write a sticky
service/native error, or erase a historical error. Genuine timestamp, mapping,
backend and cancellation failures keep their existing diagnostics. Owned touch
admission shares the existing lifecycle monitor with focus and host pause
control; paused/focus-paused/stopping hosts reject gestures. JNI also checks the
actual runtime paused state so a pending asynchronous Resume does not admit a
new DOWN prematurely. Legacy unowned touch stays unchanged.

The generation acknowledges **cancellation admission**, not guest execution.
The backend must preserve FIFO order between admitted cancellation and a fresh
BEGIN; JNI serialization cannot supply that ordering after queuing. The
independently audited b59 backend schedules each touch/cancel as a separate
one-shot bottom half, while `util/async.c` pushes and drains them at the list
head. A BEGIN followed by MOVE/UP before one poll can therefore execute the
continuation first. Backend owns that correction and its production queue
regressions; no target attribution is inferred from the source defect alone.

Source regressions cover the actual View with a fake Binder endpoint: a
successful global cancel plus complete focus gain without a status poll,
expected-cancellation handling, suppression of old continuations, and reuse
only after fresh DOWN. A second case blocks fresh DOWN while paused and still
requires fresh DOWN after Resume. Native helper cases cover failed cancellation,
normal independent UP, current-generation admission and foreign/stale-owner
protection. These new cases require compilation/execution; writing them is not
an Android or guest acceptance pass.

This changes Android AIDL/JNI touch arguments and requires matched main/test
APKs built from the new source. Do not install only this test runner alongside
candidate22. No Portable header, engine argument, gyro or guest recipe changes
are part of the Android generation contract.
