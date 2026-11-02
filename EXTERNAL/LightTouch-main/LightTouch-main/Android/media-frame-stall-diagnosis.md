# Poco concurrent-media stall: diagnosis and proposed successor

Test32's 30-second keyboard pilot failed the required concurrent interval
assertion. Forty input requests and nonzero PCM continued, while produced and
presented frame counters advanced four times and then stayed constant. Initial
native captures showed the scratch Note and keyboard. No native end frame was
captured because the assertion preceded the normal end capture. A confirmed
clean Stop followed. This is a retained failure, not a concurrency pass.

The failure handler now attempts status, the latest published native frame, and
another status before the worker binding and ActivityScenario close. Each
attempt runs even if another fails; diagnostic exceptions are suppressed onto
the original failure. These captures do not force a fresh LCD scanout or prove
which pixels the guest compositor most recently produced. No extra input,
retry, pause, resume, or deadline extension is introduced.

## What the source establishes

The installed engine revision is `96f2d08edd1439ba2b41624190ee68b8a7851625`.

* `Portable/src/Runtime.cpp` counts produced frames after acquisition of a new
  engine frame serial. Android's presenter subsequently counts presentations.
  Both stopping places the fault at or before engine acquisition, rather than
  establishing a Surface posting failure.
* `contrib/ios-app/qemu-ios-ui.c:ios_capture` compares every visible pixel with
  the last published buffer and retains the serial for identical pixels.
  `ios_gfx_update` marks a pending paint and schedules deferred publication on
  portable hosts. `ios_refresh` also drives the LCD and publishes pending work.
* `hw/arm/ipod_touch_lcd.c:lcd_refresh` reads the latched scanout, tracks dirty
  memory and GLES generations, and emits dirty updates. Vblank drives a scan
  when the console is visible; the display refresh is a further scan path.
* The retained SPI trace shows touch frames were read. It does not establish
  that UIKit consumed the keys or that Notes rendered the expected characters.
  Coordinates match the earlier manual Q, and actual DOWN/UP dwell was about
  78–87 ms; a missing hold is not demonstrated.

Identical-pixel suppression can hide a transient Q followed by backspace if
both paints complete before the next capture/publication and the final pixels
match the old buffer. A 250 ms host delay does not guarantee an intermediate
guest paint on a busy emulator. Conversely, a changed completed panel paint
that reaches `ios_capture` cannot be discarded as identical. Deferred portable
publication is intended to retain complete paints. Suppression therefore
remains a hypothesis; it does not explain away 27 seconds of stalled counters
or justify aggregating wider intervals into a pass.

## Visible-phase pilot source (not compiled or authorized to run)

Set `keyboardVisiblePhases=true` for a 30-second pilot. The opt-in requires the
reviewed keyboard binding and `requireConcurrentMediaWindows=true`; longer
diagnostic runs and arbitrary phase durations are rejected. Keep the same
reviewed scratch Note/worker/image binding,
balanced 60 ms DOWN/UP events, absolute cutoffs, and the existing required
input + produced frame + presented frame + nonzero pull/write + playback
overlap within each declared block. Keep lifecycle and strict clean Stop.
Do not reuse the spent worker or mutate the protected original.

1. Capture the initial native Note, keyboard anchors and status. Send one Q
   DOWN/UP. Leave the letter visible for a fixed 1,500 ms without backspace.
2. Capture native pixels and status **before** compensation, including input,
   serial/frame and audio deltas. Recheck the same worker, Surface, keyboard
   anchors and no held contacts. Retain each phase's raw times and images.
   Every capture rechecks the reviewed keyboard anchors. The receipt explicitly
   leaves expected text visually unverified until the images are inspected;
   counter progress alone does not establish Note content.
3. On the unchanged bound keyboard scene, send balanced backspace, wait another fixed
   1,500 ms, and capture the restored Note. Do not catch up missed phases or
   extend the original cutoff. A changed/unverified scene fails and stops;
   no extra compensating backspace is allowed. Reserve nine seconds before
   starting a pair, retain the original three-second no-input tail, and reject
   any visible phase/capture crossing that tail.
4. Record the original overlap predicate on the same phase samples. A visible
   phase does not relax the requirement or qualify sustained 20-minute media.

## Additional engine diagnosis needed for a precise stall attribution

`captureFrameTimeline=true` now opts the visible-phase pilot into existing
read-only QMP evidence. Arm the fixed debug QMP endpoint on the fresh worker
before Start. Instrumentation accepts no arbitrary QMP command or QOM path:
it locates the single n72 LCD under `/machine/unattached`, verifies the socket
peer's exact worker PID/UID, then requests its existing `frame-timeline` and the two fixed read-only
`frame-scan-counters` / `portable-frame-counters` properties. A diagnostic
engine containing QEMU `59f4c953cd` is required; an older engine fails explicitly
on missing properties, recording `unsupportedEngine=true` and the exact missing
property list. QOM getter types must be strings. A separate tested allowlist
rejects all writable commands, other properties, paths and extra arguments
before any socket write. This is source support, not a deployed or qualified fix.
It retains raw responses, raw counter JSON, timeline text and current native producer/presenter
sequences, counts and times beside each native image. History deltas require
an exact overlapping record; reset, overrun and missing history remain
unavailable rather than measured zero. Counter JSON uses a fixed flat schema,
rejects missing/unknown/duplicate fields and nested or malformed values, and
is bounded to 4,096 characters. Unsigned 64-bit values and deltas use exact
integers. Saturation or a decreasing counter yields an unavailable delta; a
changed or saturated guest-reset count suppresses scan interval attribution and portable
interval deltas. Queries occur sequentially and do not claim an atomic snapshot
with native counters or pixels. A changed scanout base is explicitly
not treated as changed pixels.

Each QMP snapshot has one absolute 1,000 ms deadline including socket connect,
plus a joined socket-closing guardian and read timeout. Limits are 256 KiB per
response, 1 MiB per transcript, eight intervening events per response, 4,096
timeline entries and sixteen snapshots. A diagnostic failure is retained and
ends the opt-in test; it never reconnects automatically. Raw evidence stays in
the private media-evidence directory. Full APK and actual target exercise are
still unrun. The earlier frozen Test33 packet is unchanged and does not contain
this later source checkpoint.

The earlier image/status-only evidence cannot distinguish all actual layers. Longer
visible phases can test transient coalescing, but another unchanged published
image would leave guest scanout, dirty tracking, publication and acquisition
indistinguishable. Add bounded counters before another diagnostic runtime if
the objective is to locate the stall in one run, rather than repeat the same
ambiguity. The engine addition is committed separately; the instrumentation successor
does not contain a replacement engine or qualify a product fix.

Engine source checkpoint `59f4c953cd` implements the following file scope:
`136b14ea63` adapts the three existing slice harnesses for the added state.
Neither source checkpoint is target acceptance. Production file list:

* `include/hw/arm/ipod_touch_lcd.h`: per-device cumulative scan/dirty/reset counters;
* `hw/arm/ipod_touch_lcd.c`: increment at existing scan/update boundaries and
  expose read-only `frame-scan-counters` JSON beside `frame-timeline`;
* `contrib/ios-app/qemu-ios-ui.c`: count existing capture/dedup/copy branches;
  attach read-only `portable-frame-counters` to the LCD when the UI attaches;
* `tests/slice/ipod-frame-pipeline.c`: production-function ASan/UBSan slice,
  invoked through the existing `tests/slice/run.sh` without build-system edits.

Using two existing-object properties keeps normal headless LCD builds free of
a new dependency on the optional portable UI bridge. No public ABI/header,
frame-copy layout, JNI or product behavior needs to change. Snapshot fields:

* LCD scan attempts, dirty updates, last latched scanout base and GLES generation;
* `ios_capture` attempts, identical-pixel suppressions and published serial;
* portable frame-copy calls, NO_FRAME returns, successful copies and last
  successfully copied serial.

The existing read-only `frame-timeline` LCD property supplies vblank/latched-base
history already; reuse it instead of inventing another frame scheduler. The
new getters must read LCD state under the BQL and publication/copy state under
the existing frame lock, respecting the existing BQL-to-frame-lock order.
Keep the counters fixed in size and lifetime, with no per-frame logging,
dynamic history, forced invalidation, or publication side effect. Count actual
pixel copies separately from size-only metadata queries and NO_FRAME returns.
Fixed QMP requests from instrumentation can collect the properties at start, visible
phases and failure; the existing debug QMP endpoint must be armed on the fresh
worker before Start. The current fixed JNI QMP snapshot does not request these
properties, so it cannot supply these counters unchanged.

Upstream portable publication adaptation `5c96b49196` is already present in
the installed source. Do not duplicate upstream `97860b36` or remove its
deferred complete-paint boundary. Counters observe that boundary as it stands.

Comparing these stages separates unchanged guest scanout, missed dirty
notification, deferred publication and consumer acquisition. Latest-published
screenshots alone cannot make that distinction. Engine/QMP getter source,
semantic source tests are available; a new pinned engine and native APK allocation
require separate review. Engine counters are implemented in source only; no new
native build or target run is claimed here. No
rendering bypass or forced duplicate frames should manufacture progress.

The new exception-preservation helper has a host-JVM contract test. Full APK
compilation and actual target exercise remain pending separate allocation.
