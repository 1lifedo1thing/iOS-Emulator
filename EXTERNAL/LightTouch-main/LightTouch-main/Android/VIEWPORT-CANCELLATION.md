# Expected viewport cancellation

Source followup to the retained Poco24 error, not a target result. The earlier
concurrent-media attempt failed its initial error-free precondition before media
input; that failure and its already-visible viewport error remain unchanged.
No existing worker's error is cleared by this change.

A finite point outside the guest's fitted viewport is LT_AGAIN. Android still
returns a nonempty rejection and requests the existing scoped release before
classifying it. If View-owned contacts/release debt exist, native cancellation
must return LT_OK. If none are owed after terminal-success refresh, the gesture
is already balanced and an unrelated Home/Power pulse is preserved. Only this
successful scoped LT_AGAIN case returns the closed expected-cancellation reason
`Display input outside guest viewport; gesture cancelled`.

Invalid/nonfinite coordinates or viewport geometry (LT_INVALID), unavailable
frames/runtime and failed contact release remain real nonempty errors. A failed
release keeps its actionable cause and pending cleanup obligation. The legacy
ambiguous viewport string is not whitelisted. Native and service public ABI
shapes are unchanged.

The existing View expected-cancellation path drops local IDs/input generation,
awaits a fresh DOWN, and suppresses old MOVE/UP. The existing service error
assignment excludes closed expected reasons; it still retains unrelated fatal
errors and never blindly clears them after an admitted touch. Native mapping
continues to use the same viewport math and accepted coordinate bounds.

Worker observations add `displayInputCancellations`: total host cancellation
count, lastReason and the last8 closed reasons as an indexed nested Bundle,
plus historyLimit and guestConsumptionInferred false. Nested strings retain
readable reasons in both status and the existing durable JSON encoder. The snapshot survives View recreation. Readers take a volatile immutable
snapshot without waiting on native/touch admission; record serializes only the
small bounded history, after control admission. This is cancellation history,
not proof that a contact batch reached or was released by the guest.

Staged regressions cover the production native classifier's mapping/release
matrix, exact expected-reason classification, bounded immutable history and
production View balanced viewport cancel -> suppressed continuation -> fresh DOWN,
plus release failure remaining actionable. These tests are UNRUN until an
explicit compiler/target allocation. No acceptance assertion has been relaxed.
