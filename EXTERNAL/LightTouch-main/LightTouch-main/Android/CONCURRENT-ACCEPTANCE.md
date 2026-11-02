# Production concurrent input/output acceptance

These are opt-in instrumentation recipes, not recorded target passes. Root owns the phone/AVD, private fixture provisioning, foreground readiness, visible/audible review and command timeout. Preserve every failed runner log and JSON receipt; `adb` exit zero alone is not a JUnit pass. Foreground `MainActivity` before every `am instrument --no-restart` invocation: retained target evidence showed Android's cached-process freezer can postpone a new runner until a normal foreground launch. Do not change system freezer or security settings.

Use the reviewed installed main APK with a matching test APK. The historical
first concurrent-test build used candidate16 and a test-only addition. That
recipe does not apply to the newer owner/generation AIDL and JNI changes:
those require a matched newly built main and test APK. Keep older candidates
immutable and bind each run to its actual source and payload hashes.

Root must boot the intended selected record to actual Home and open/inspect a real Photos image before gesture acceptance. Resume and allow the chosen workload to settle while the activity remains foreground. An agent status or boot logo is not readiness proof. The concurrent test requires an existing running/paused worker; it never starts, imports, selects or resets one. It intentionally sends the guest Home+Power screenshot chord and may create a photo in writable guest state.

Commands below target `org.lighttouch.android.test/androidx.test.runner.AndroidJUnitRunner`. Run each separately and inspect/extract its app-private evidence before the next operation. Instrumentation completion can force-stop the target; root must choose `--no-restart` and any longer active-test continuation deliberately rather than assuming a running worker survives completion.

| Exercise | Exact class/method and arguments |
| --- | --- |
| Concurrent touch/frame/nonzero playback | `-e class org.lighttouch.android.GuestConcurrentMediaTest#twoContactsOverlapGuestScreenshotSoundAndFramePresentation -e exerciseConcurrentMedia true -e concurrentSeconds 10 -e postGestureSettleSeconds 5` |
| Photos pinch with delayed capture | `-e class org.lighttouch.android.GuestInputTest#stableTwoPointerTrajectoriesUseProductionDisplayPath -e exerciseGuest true -e gesture pinch -e settleSeconds 5 -e postGestureSettleSeconds 5` |
| Recreate, rotate, contact cancel/reuse and balanced controls | `-e class org.lighttouch.android.GuestOrientationLifecycleTest#recreateRotateCancelReuseAndBalanceProductionControls -e exerciseOrientationLifecycle true` |
| 30-second media and focus/lifecycle check | `-e class org.lighttouch.android.GuestMediaLifecycleTest#productionMediaContinuesAfterPauseSurfaceAndAudioFocusChanges -e exerciseMedia true -e mediaSeconds 30 -e requireFrameProgress true -e requireGuestAudio true -e exerciseAudioFocus true` |
| Sustained 20-minute media and focus/lifecycle check | `-e class org.lighttouch.android.GuestMediaLifecycleTest#productionMediaContinuesAfterPauseSurfaceAndAudioFocusChanges -e exerciseMedia true -e mediaSeconds 1200 -e requireFrameProgress true -e requireGuestAudio true -e exerciseAudioFocus true` |
| Actual held contacts through audio-focus cancellation | Add `-e exerciseHeldAudioFocus true` to the media/focus recipe, using the new matched owner-generation APKs |
| Sustained declared keyboard workload with measured counter windows | Add `-e requireConcurrentMediaWindows true` to the 1200-second media recipe |

For media recipes root must first choose a guest workload that actually continues producing changing frames and nonzero sound for the measured duration. The test does not synthesize PCM or repeatedly trigger a sound. A silent/static workload correctly fails the required delta assertions; it does not establish a runtime regression by itself. No recipe enables gyro, motion or agent bundle launch. Omit `touchWireTrace` on an ANGLE-started worker: the current ANGLE start test has no pre-start trace hook. On an independently started already trace-configured worker, the existing concurrent/pinch/orientation recipes may add `-e touchWireTrace true`; configuration and actual retained guest wire reads remain separate facts.

`GuestConcurrentMediaTest` binds production `IEngine` and sends Android `MotionEvent`s through the actual `GuestDisplay`, IDs7/23, native aspect-fit mapping and Binder/JNI. It records source uptime, viewport, input ownership, full worker status and elapsed-time bounds for each read. After both contacts are admitted it performs the fixed Home/Power 150 ms chord once, moves both contacts for a bounded 3–30 seconds and records ~100 ms sampled counter intervals. A qualifying adjacent pair must increase actual producer and presenter counts, pulled and AudioTrack-written nonzero samples, and playback-head frames while both Android-owned contacts stay held. It rejects increases in write/native PCM/window failures, worker changes and actionable errors, and checks balanced button-admission deltas. These aggregate intervals establish measured overlap bounds, not exact sample timestamps, guest contact consumption or audible output. Optional `touchWireTrace` consumes the existing bounded own-worker collector, retains actual pending FRAME_READ/nonempty serialized contact records and host touch-index read bounds, and still does not infer per-batch or two-contact guest consumption. Cumulative RMS values and full counters are retained in each status; no subtraction across frame/PCM/input clock epochs is performed.

Finally requests both any still-owned button releases and contact cancellation, retaining failures. Captures use only allowed `files/screenshots/concurrent-evidence-*/{before,two-held-after-chord,after-settled}.png` paths; post-gesture settle and terminal measurements occur before `ActivityScenario.close` can background-pause the VM. Private `files/concurrent-evidence-*` retains `result.json`, full `samples.json`, `events.json` and interval deltas in `intervals.json`. All manual visual/audibility/photo/wire flags remain false. Root reviews actual before/after pixels, guest photo/gesture response and audible shutter separately. Individual Binder calls have no newly invented cancellation guarantee; use the root-owned external runner deadline and retain an ANR/hang if a call fails to return.

The new media opt-ins leave default stimulation unchanged. Held focus sends
real production MotionEvents with stable IDs7/23, retains the current worker,
display owner and generation, requests actual competing audio focus and waits
for pause/gain. It requires the same View to retain the old local IDs through
that unnotified cancellation. Its old MOVE must become expected cancellation,
subsequent UPs must not advance native input requests, and fresh DOWN/UP must
be admitted without a new worker error. Actual captures occur before scenario
close; finally releases contacts/focus even after failure. If View callbacks
already cancelled the gesture, that run fails this narrow isolation condition
rather than claiming generation-specific target proof.

Extended media tracing appends one-second status/input observations and deltas
to JSONL, then writes compatibility JSON arrays once at termination. Rewriting
the full growing trace every second would add unnecessary IO to a20-minute
measurement. A qualifying counter interval increases input requests, actual
producer/presenter counts, pulled/written nonzero PCM and playback head in the
same adjacent observations. Strict window mode requires at least one such
interval in each30-second workload block (and a final block of at least5s).
It fails new input/audio/presentation errors and worker/state changes. These
are interval bounds, not exact simultaneous sample timestamps or continuous
two-contact proof; retain the short concurrent test's separate held-two-contact
evidence. Manual guest/audibility flags remain false.

A legitimate stock workload is a **new scratch Notes note**, with its actual
on-screen keyboard inspected first. Leave the user's existing Test note
untouched. Verify real keyboard-click PCM and changing frames in30 seconds,
then let the phone owner perform a bounded letter/backspace pair every2 seconds
during the measurement, keeping the scratch text bounded and stopping if the
inspected layout changes. The test does not type text, toggle guest settings or
invent PCM. Stop that driver at measurement end, before the held-focus reuse
phase. Existing user-provided music may be used only if a real supported loop
is already available and inspected; do not assume the stock Music library has
a track. One concurrent screenshot chord may create one additional guest photo,
not a repeated photo workload. New source cases are not target passes until the
matched APKs are compiled, run and their raw evidence reviewed.

The source now also offers an in-test keyboard workload through the optional
`keyboardWorkloadBinding` JSON argument. This needs a newly compiled test APK;
sealed Test30/31 remain unchanged. Prepare the binding **before** measurement
with `scripts/prepare-media-keyboard-binding.py` using a current strict-collector
`before.json`, the personally inspected raw guest PNG, actual View dimensions
and rotation, normalized guest coordinates for a letter/backspace, and two to
eight stable native keyboard rectangles. The helper is offline, requires an
explicit new-scratch-note/preserve-existing-notes declaration, and writes only
to a new private file outside Git. Pick anchors that exclude the clock, caret,
edited text and letter popup but distinguish alphabetic keyboard geometry.
Keep the full reviewed image and its hash; the declaration is operator evidence,
not an automated claim that a picture depicts Notes.

Pass the resulting JSON as the `keyboardWorkloadBinding` instrumentation string,
with the existing strict media/window flags and `mediaSeconds` exactly30 or1200.
The driver checks exact worker PID/UID/start token, native display owner/input
generation, Surface generation, View identity/size, guest dimensions and Android
rotation. It taps through the actual GuestDisplay path, waits at least2seconds
after a completed pair, never catches up, and stops admitting pairs4.5seconds
before measurement end, with a3second event cutoff. Stable native RGBA anchors
are checked at start, at most about every30seconds while driving, and at end;
this is periodic image evidence, not continuous scene recognition. All key
events/coordinates/input-admission receipts are appended to
`keyboard-events.jsonl`; normal JSON arrays remain available at termination.
Anchor captures, observer reads and test IO have cost and are part of this
acceptance workload, not an uncontaminated CPU performance benchmark.

The driver runs synchronously inside measurement and cannot continue into the
pause, background or focus phases. Failure cancels an in-flight local contact;
it never guesses a compensating backspace on a changed scene. An unmatched letter
may remain in the private scratch note. Android admission, anchor agreement,
nonzero PCM and playback-head movement still do not establish guest text
consumption or physically audible playback. Retain actual before/end PNG review,
the real JUnit result and independent native clean shutdown/restoration evidence.
Five host binding/image tests and a standalone cached Kotlin type-check passed.
No new APK was built or run on a target; this establishes no new guest acceptance.
