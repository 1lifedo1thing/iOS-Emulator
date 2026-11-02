# Candidate19 existing-session unlock diagnosis

`GuestUnlockTest` is test-only and opt-in. It requires the exact sealed candidate19
main APK and an already-started same-UID worker; its initial binding omits
`BIND_AUTO_CREATE`. It does not import, start, recover, enable tracing, invoke a
guest RPC, save a guest photograph or enable motion. Root must already have
inspected the n72 Lock layout; the test does not classify screens automatically.

After the new isolated test APK is built and installed by the device owner, keep
the existing main APK and guest state unchanged. Foreground MainActivity before
`--no-restart`, then use the matching runner:

```sh
adb -s "$ANDROID_SERIAL" shell am start -W -n org.lighttouch.android/.MainActivity
adb -s "$ANDROID_SERIAL" shell am instrument --no-restart -w -r \
  -e class org.lighttouch.android.GuestUnlockTest \
  -e exerciseGuestUnlock true -e observedN72LockLayout true \
  -e resumeExisting true \
  org.lighttouch.android.test/androidx.test.runner.AndroidJUnitRunner
```

This is a planned invocation, not a recorded unlock pass. A paused session needs
explicit `resumeExisting`; a stopped/failed/unused worker is rejected. If asleep,
the test admits balanced Power DOWN/UP with500ms host hold, then polls for at most
15s for running, native sleep0, positive backlight, producer growth and nonzero
RGB samples. UID/PID/session identity must remain unchanged. If already awake,
a separate gate has a500ms host deadline: statusA, actual raw capture and statusB
must agree on positive copied/producer sequence and timestamp, with running,
sleep0, positive backlight and nonzero RGB at both observations. A static Lock
need not repaint. Each capture attempt has a distinct retained filename; the
accepted path and whether new producer growth occurred are recorded. Neither
gate identifies the scene or proves guest input consumption. Binder calls still
need the owner's external deadline if they block beyond either polling budget.

While one ActivityScenario remains resumed, it captures the raw frame and sends
one stable pointer7 through the production GuestDisplay: guest-image fractions
(.20,.92) to(.85,.92),15 moves scheduled across600ms, then UP. Actual aspect-fit
viewport geometry is recorded. A changing View/viewport or rejected/cancelled
contact fails rather than silently continuing another gesture. Raw captures are
scheduled at UP+300ms and UP+1500ms before activity closure. Per-move evidence is
buffered in memory so file writes do not consume the short awake window.

Private `unlock-evidence-*` contains result, status timeline and MotionEvents;
captures stay under `files/screenshots/<evidence-folder>`. An already-configured
worker trace is copied through the bounded existing wire reader after the swipe;
it is never enabled late. Cleanup attempts UP if needed and always full input
cancellation. Inspect JUnit plus retained receipts and images. All guest-unlock,
visual/audible and guest-acceptance flags remain false pending root review.

BOOTTIME host deadlines, MotionEvent uptime and guest virtual trace time have
different epochs. Button/touch admission is not application or guest consumption.
The owner's external execution deadline is still required for Binder/platform
calls. Instrumentation completion may terminate the target package; retain the
evidence before interpreting subsequent lifecycle or dirty-state outcomes.

The isolated runner was compiled from Android/AIDL source
`4c9a54a57e13abe1cde34b52fa0e4b687debd76e`, plus only `GuestUnlockTest.kt`
from `463fe49e081dd9a01b36650aceca4c691e35b45d`. All81 base Android files
were verified unchanged. Native53ea and engine19/e67 inputs remain the sealed
main19 producers; this runner does not replace those binaries. Keep the helper
revision separate from the main APK's source identity.

Reproduce with a fresh Git archive of the base Android directory, add that single
test file from its pinned Git commit, and use main19's recorded dependency and
notice properties with an isolated build directory. The executed task selection
was:

```sh
./gradlew --offline --no-daemon --max-workers=1 \
  -Dorg.gradle.jvmargs='-Xmx768m -XX:MaxMetaspaceSize=384m' \
  -PnativeJobs=1 <main19-properties-with-isolated-buildRoot> \
  :app:assembleDebugAndroidTest \
  -x :app:externalNativeBuildDebug \
  -x :app:mergeDebugAndroidTestNativeLibs -x :app:packageDebug
```

The angle-bracket argument represents the recorded properties, not a literal
shell argument. The private producer receipt retains the complete executed argv,
archives, file hashes and log. Compilation passed in53.5s; no native or main APK
packaging task ran. The runner has zero native entries, its compiled test method
is present in DEX, and signature compatibility/alignment checks passed. Main19
and its original runner hashes were checked unchanged before and after. These
are build checks; no target unlock run was performed by the product lane.
