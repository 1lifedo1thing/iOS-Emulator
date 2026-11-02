# Real keyboard workload for the media lifecycle test

This controller is a source candidate, not a device qualification. It never
starts, resumes, recovers or stops a guest, and its default invocation performs
no ADB operations. It uses the existing `GuestMediaLifecycleTest`, not a new
production API. Root/operator retains exclusive phone control.

Create a **new scratch Notes note** and inspect its actual guest keyboard. Do not
edit the user's existing `Test` note. Inspect the Notes list and note identity
before setup. Real keyboard clicks must supply the changing frames and guest
PCM; no host text injection, keyevents, tones or guest RPC substitutes are used.
A letter/backspace pair is attempted no sooner than two seconds after the prior
pair start, within the bounded cadence below. Successful Android input
admission does not establish guest consumption or an audible click.

## Bind this run before execution

Use a fresh strict collector `before.json` for the intended existing worker.
Pin actual installed main/test hashes, record, UID/PID/startticks and Android boot
ID. Old bindings must not survive a worker replacement or reboot.

Before starting the timed test, exercise every available non-mutating platform
reader against the current phone: identity, APK hashes, record metadata, focused
display/window, image dimensions, keyboard anchors and clock brackets. A retained
media sample can validate its format, but cannot establish current freshness.
Foreground MainActivity before `--no-restart`; a cached target can otherwise leave
the runner pending. Keep the host controller's source/hash separate from the
installed app/test producer pins.

Launch the existing media test with `--no-restart` after foregrounding the app:

```sh
adb -s "$SERIAL" shell am instrument --no-restart -w -r \
  -e class org.lighttouch.android.GuestMediaLifecycleTest#productionMediaContinuesAfterPauseSurfaceAndAudioFocusChanges \
  -e exerciseMedia true -e mediaSeconds 30 \
  -e requireFrameProgress true -e requireGuestAudio true \
  -e exerciseAudioFocus true -e exerciseHeldAudioFocus true \
  -e requireConcurrentMediaWindows true \
  org.lighttouch.android.test/androidx.test.runner.AndroidJUnitRunner
```

This test's binding can auto-create an unused service. Independently verify the
existing worker before launch; do not mistake an unsupported expected-PID argument
for an in-test guard. Never launch another instrumentation/UiAutomator session
while it runs. The controller uses read-only `screencap` and focused-window
metadata, which do not take over the instrumentation accessibility connection.

Discover the **new** `files/media-evidence-<timestamp>` directory by pre/post set
difference. Use its `measurement-start` sample and matching allowed-path raw
capture. Retain a fresh Android window PNG after this Activity has attached;
inspect the scratch note, keyboard and viewport yourself. Construct a private
keyboard JSON containing these required fields:

- `schema: 1`, `serial`, `recordId`, `mainApkSha256`, `testApkSha256`.
- `scratchNoteApproved: true`, `preservesExistingTestNote: true`, and a bounded
  `scratchNoteIdentity` chosen by the operator.
- `mediaFolder`, `durationSeconds` (30 or1200), and the exact
  `measurementStartDeviceBootTimeMs` from `measurement-start.hostElapsedMs`.
- `androidImageSha256`, `rawGuestImageSha256`, `androidDimensions: [width,height]`.
- `guestViewportBounds`, `letterKeyRegion`, `backspaceKeyRegion`, and
  `stableKeyboardRegions`: integer `[left,top,right,bottom]` rectangles in the
  **current Android screenshot**, never inherited coordinates. Keys must be
  distinct, nonoverlapping and strictly inside the inspected guest viewport.
- Two to eight independently inspected stable image anchors, totalling at most
  524288pixels. Include recognizable keyboard geometry and own-app chrome;
  exclude the caret, clock, edited text and temporary pressed-key highlights.
- The current sample's `surfaceGeneration` and
  `androidInput.cancellationRequests` as `inputCancellationRequests`.

There are deliberately no executable example coordinates or inferred scene
classifier. The binding must be prepared promptly: execution rejects a
measurement-start older than15seconds. If setup cannot fit the30-second dry run,
retain that failure and review timing instead of relaxing the phase guard.
The test publishes `measurement-start` before its raw capture/events record is
complete. Wait for that matching complete record within the original setup
deadline; do not reset the clock or backfill a late approval.

Inspect anchors during the actual letter popup as well as the resting keyboard.
Avoid transient popup areas and notification-prone app titles. Retain recognizable
own-app chrome outside the input area and a keyboard-mode glyph that distinguishes
alphabetic from numeric input. Replay proposed anchors against saved real popup,
notification and wrong-scene images before another timed trial. A production
screenshot notice may change the viewport; restore and remeasure the actual layout
before reusing any coordinates. Do not suppress notifications or change settings
to make an image guard pass.

The host image dependency is a prebuilt Pillow wheel. For example, in a private
host venv outside Git, install `Pillow==11.3.0` with `--only-binary=:all:`. No target
package or security-setting changes are required. Use that Python for:

```sh
python Android/scripts/drive-media-keyboard.py \
  --serial "$SERIAL" --binding "$PRIVATE/before.json" \
  --keyboard "$PRIVATE/keyboard.json" \
  --android-image "$PRIVATE/android-approved.png" \
  --raw-guest-image "$PRIVATE/guest-approved.png" --out "$PRIVATE/workload"
```

Review the plan, then add `--execute` only under exclusive device allocation.
The output directory must be new and outside Git. A command timeout is a failed
run: it cannot prove the remote input was cancelled. A failed second tap may
leave an unmatched letter in the scratch note; the controller never guesses a
cleanup tap on a changed screen.

## Bounds and evidence

Each key is preceded by worker/UID/start/boot/selected-record checks, a fresh
complete media sample, actual own focused window, unchanged Surface generation
and cancellation count, and exact current RGB image anchors. Stale/error/paused
samples, held contacts, rejected batches, changed image dimensions/anchors,
missing phase data or missed cadence abort. Device-side guards immediately
before each `input touchscreen tap` recheck process start, measurement label and
BOOTTIME cutoff. These checks are adjacent observations, **not** an atomic
window/image/input transaction; an independent operator must not interact.

After an owned tap command returns, the next guard requires a status read that
began after the upper bound of its immediate device-clock completion bracket.
Older per-second samples can contain that tap's in-flight contact. Retain those
samples and wait at most two seconds, without renewing the deadline or extending
the cutoff; then apply every normal check to the fresh sample. Fresh held contacts
still abort. Command completion and empty Android IDs do not establish guest UP
consumption. The reviewed controller revision allows pair starts two to 5.5
seconds apart and reserves five seconds before starting a pair, accommodating the
bounded fresh-sample wait. It requires a new actual 30-second qualification before
a long run; earlier failed runs retain their original controller contracts.

The conservative cutoff is `measurementStartDeviceBootTimeMs + duration -3000`.
The legacy sample field `hostElapsedMs` is Android `SystemClock.elapsedRealtime`
(BOOTTIME), not PC time or MotionEvent uptime. `/proc/uptime` is treated as a
precision bracket. No pair starts without five seconds remaining, no tap without
500ms, and both completion clocks must precede the cutoff. The actual test starts
its duration after the initial capture, so this cutoff precedes its earliest
lifecycle transition. At most15/600pairs are allowed; no catch-up bursts occur.

Raw per-command identity/status/clock receipts and image hashes remain private.
Real Android checkpoint PNGs are retained on the first pair and every15pairs
(roughly30seconds); matching intermediate payloads are removed after their hashes
are recorded. An image mismatch retains its raw payload. This bounds PNG
retention to at most40 checkpoints for1200seconds plus a possible failure image.
The instrumentation independently retains actual raw guest before/after images,
per-second observations and per-interval input/frame/nonzero-PCM/playhead deltas.
No ROM/NAND/full record export is collected.

Run30seconds first. Inspect the real images, JUnit result and every declared
30-second/final>=5-second block. The quiet tail may expose a workload gap; keep
that failure rather than weaken the test. Only then plan1200seconds. Repeated
keyboard taps are not continuous two-contact input; the already separate held
contact/render/audio test and the post-measurement focus-loss/fresh-DOWN phase
have different claims. Also verify fresh output after lifecycle resumes; the
existing test does not enforce new frame/PCM growth after every resume.

Manual audibility, image semantics and guest-consumption flags remain false
until separately reviewed. Finally inspect the original Test note unchanged and
collect strict shutdown/persistence evidence separately. No successful controller
result, cumulative audio count or synthetic host test establishes guest acceptance.

## Matched short launch/menu latency cases

Coordinate with Backend and the Mac operator before profiling. The selected
canonical case is **Home idle → Settings → General → Back**, with separate cold
and warm app-launch runs. Notes/editor timing is a different case and must not be
silently substituted into the comparison.

1. Pin fixture/state/guest tools/provider, main/engine/JNI hashes, UID/PID/start
   and actual initial raw+Surface images. Keep monitor closed, fixed viewport,
   no SDK scenario transition and no concurrent workload. Idle on inspected Home
   for five seconds. Record host-worker CPU ticks and clock tick frequency.
2. Inspect a fresh actual Settings icon ROI. Retain the before Android/raw image,
   device BOOTTIME brackets around the one production tap and the input receipt.
   Sample counters and real images at250ms cadence for at most20pairs/5seconds;
   stop after two inspected completed captures. If incomplete, retain one
   separately labelled10-second capture, then end the transition as censored.
   Keep actual achieved times and observer overhead. Do not retry the tap.
3. Manually inspect the earliest completed **Settings list** image. A new frame,
   process admission, title/frontmost label or boot logo is not that endpoint.
   Report an interval: last image not yet complete to first inspected complete
   image. Raw-copy completion and actual Android Surface completion are separate
   measurements; host ADB roundtrip is not guest CPU time.
4. On that completed list, separately inspect/tap **General**, then its actual
   Settings/back control. Record each completed General/list image and the same
   latency brackets. The working real follow-up establishes responsiveness; a
   single static completed image alone does not. Do not use fixed old coordinates.
5. Return to inspected Home using a balanced production control; after five
   seconds repeat the launch as a distinct warm case. Keep same worker/record.
   Never claim cache-cold or force an engine restart merely from closing an app.
6. Compute process CPU tick deltas over each measured operation, retaining both
   wall-time bounds and whole-worker/support-thread scope. Android and Mac clocks
   are independent; compare durations, not absolute timestamps. If sampler reports
   zero samples, retain it as inconclusive and use the valid tick/time/image data
   without inventing function attribution.

These are operator steps pending target allocation and inspected current ROIs,
not a remote-control script or a benchmark result. No guest setting writes,
existing note edits, host audio substitution or device/security changes are part
of this comparison.

For the subsequent matched CPU-only window, Backend's reviewed proposal is20s
with intended Settings/General/Back/Home actions near2/7/12/17s. Each remains
gated by the actually completed prior scene; skip/censor late actions rather
than blindly catching up. Capture full images only before/after that profile.
The preceding250ms image series is a separate, perturbed visual-latency run.
Use the independently reviewed platform simpleperf command and validated live
TIDs; this keyboard controller does not launch profiling or add trace events.
