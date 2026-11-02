# Candidate 26: matched build and retained-state acceptance

The app/native producer is `f80def4abadb2b1907f2da787b3623aa3f75dfac`.
The selected Android engine producer is `d9a00001ad050025b790e44be53e86f78b26ca5c`.
These are distinct source identities. Build both ABIs and the matched instrumentation
runner; an old test-only runner cannot validate the new accounting method.

The private reviewed build plan records the Git archive, every exported file,
same-revision Pixel test staging, engine/runtime/notice hashes, bridge copies and
exact Gradle arguments. Use a fresh CMake and Gradle output directory, offline,
one worker/native job, 768 MiB Java heap. The one allocated build runs
`:app:assembleDebug :app:assembleDebugAndroidTest :app:testDebugUnitTest --tests
org.lighttouch.android.*`. The 46 JVM cases are a source count until XML results
prove execution. Keep Debug symbols/assertions and verify effective JNI/Runtime
`-O2`, metadata entrypoint, fixed accounting setter and all AIDL consumers.

Before delivery verify the actual 17 native payloads, exact NDK stripping,
platform-only dependency closure, ELF and ZIP 16 KiB alignment, matching signer,
catalog, and all declared notice payload hashes. Build success is not a guest,
performance or distribution-clearance result.

## Retained record and first start

1. Bind the current worker PID, UID, process start ticks, Android boot, selected
   record and installed APK. Inspect its actual current Surface before input.
2. Press the production Stop control once. Retain the raw PID-qualified terminal
   receipt and metadata. Clean requires a valid same-poll stopped snapshot,
   shutdown confirmed, dirty/storage-failed false and engine exit zero, with no
   contradictory deadline. Require the old PID gone, state readable, dirty marker
   absent and a nonblocking exclusive lock on the existing lease opened read-only.
   Missing receipt or a timeout remains unconfirmed; never erase retained state.
3. Install the matched main/test pair without clearing data, only after that
   boundary. Verify actual installed hashes. Run fake-endpoint/View/Surface tests
   while the service is unused; these are framework checks, not guest acceptance.
4. If accounting/wire diagnosis is selected, use the existing QMP fresh-start test
   with `debugGuestQmp=true`, `debugVcpuAccounting=true`, `touchWireTrace=true`,
   the selected `recordId`, and a finite settle/sample count. It arms both fixed
   diagnostics before Start and records configured state. Do not first start the
   worker manually. Normal runs leave accounting off. Never use cold recovery for
   a record whose ordinary clean start should succeed.
5. Foreground the activity before each `am instrument --no-restart` invocation.
   SDK scenario closure pauses the guest. Resume semantically and wait for actual
   running status before a manual measurement. Bind every phase to the same
   worker/record, and recheck process identity before and after.

## Matched menu measurement

Close Performance for the baseline and record that choice. Inspect current raw
and Android Surface images, wake/unlock only from a freshly recognized scene,
then collect Home idle and Home → Settings → General → Back → Home twice.
Call the first launch “first measured launch”; do not call it cache-cold without
an actual cache definition/control. Keep action-command start/end and first/last
incomplete/complete image brackets. A title/frontmost response or a new producer
frame alone does not prove the complete scene is responsive.

Use the same event, verified existing TIDs, no-inherit, provider, workload and
observer settings for a profile comparison. The known working nonroot route is
platform `/system/bin/simpleperf` under the app UID, plain `task-clock`, count
period 50000000, 20 seconds, monotonic clock, bounded DWARF stack and output.
Report actual nonzero samples/event count, CPU/thread mapping and identity
stability. A zero-sample recording is inconclusive. Screen capture and profiling
cost are part of the observed workload; no priority/affinity/security changes.

## Input, concurrent media and lifecycle

Reuse the current compiled classes, with no gyro opt-in:

- `GuestInputTest#stableTwoPointerTrajectoriesUseProductionDisplayPath`: run the
  approved pinch/cross/independent-lift/cancel cases on an inspected appropriate
  scene, retain wire evidence and bounded post-gesture settlement before close.
- `GuestConcurrentMediaTest#twoContactsOverlapGuestScreenshotSoundAndFramePresentation`:
  `exerciseConcurrentMedia=true`, `concurrentSeconds=10`,
  `postGestureSettleSeconds=5`, `touchWireTrace=true`. This intentionally creates
  a guest screenshot/photo. Require simultaneous interval progress in input,
  produced/presented frames and nonzero PCM/playhead, with balanced contacts and
  buttons. Review actual images; counters do not establish semantic scene success.
- `GuestOrientationLifecycleTest#recreateRotateCancelReuseAndBalanceProductionControls`:
  `exerciseOrientationLifecycle=true`, `touchWireTrace=true`. Preserve production
  Pause/Resume clicks and all failure-tree/Android PNG evidence. No direct Binder
  substitute for a failed production-control lookup.
- `GuestMediaLifecycleTest#productionMediaContinuesAfterPauseSurfaceAndAudioFocusChanges`:
  `exerciseMedia=true`, `mediaSeconds=30`, `requireFrameProgress=true`,
  `requireGuestAudio=true`, `exerciseAudioFocus=true`,
  `exerciseHeldAudioFocus=true`, `requireConcurrentMediaWindows=true`.
  Only extend to 1200 seconds after the 30-second actual-workload trial succeeds.

The media measurement has no continuous input generator. Pair it with the bounded
`drive-media-keyboard.py` controller only after an operator creates a **new scratch
Note**, inspects its keyboard and approves fresh identity/viewport/ROI bindings.
Never edit the existing Test note. Its letter/backspace pairs end three seconds
before measurement completion, so the subsequent lifecycle controls cannot
receive stale keyboard coordinates. Every 30-second measured window needs actual
input, frame and nonzero PCM progress. Preserve any quiet/failing window rather
than replacing it with synthetic PCM or loosening assertions. Manual audible,
visual and guest-consumption qualification stays separate from JUnit.

Finally collect a new strict Stop, ordinary same-record restart and inspected
saved-photo/Test-note persistence. Recheck current Surface coherence and progress
across the worker replacement; fake-endpoint tests cannot prove BufferQueue recovery.
