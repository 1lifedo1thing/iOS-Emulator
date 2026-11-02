# Pixel boot diagnosis overlay

`GuestPixelBootDiagnosisTest.kt` is a test-only overlay outside normal source
sets. Export the selected frozen production Android source/catalog into a new
private directory, copy this file into its Android instrumentation source set,
and build only the matched test APK with the same native/dependency inputs and
independent outputs. Record source/overlay revisions, hashes, command, log,
main/test signature equality and absence of test native payloads. The integrator
provides `expectedMainApkSha256` from the sealed main APK receipt (including APK24
when selected); the test requires 64 lowercase hex digits and hashes the actual
installed APK. There is no default or previous-APK fallback. This is an identity
check, not a guest acceptance claim. Existing sealed artifacts remain untouched.

## Explicit graphics provider

New diagnostics accept only `pixelGraphicsProvider=platform` (default) or
`pixelGraphicsProvider=angle-gl` (explicit optional comparison). These map to the
existing debug API choices0/1; the production main APK and native ABI are unchanged.
The overlay selects once on a fresh unused worker, retains the selection error and
actual pre-Start status, and requires the observed provider to match with
`graphicsProviderFixed=true` before Start, after Start and throughout observation.
Selection failure or provider drift fails the diagnostic; it never tries another
provider. Requested/observed/fixed fields are published in result/ready/admission
receipts. No observed provider is claimed by the host before the actual selection.

`plan_manifest.build(..., pixel_graphics_provider='platform')` pins the choice and
generates the exact `-e pixelGraphicsProvider platform` instrumentation argument.
The builder defaults every new plan to platform, including plans seeded from an
old ANGLE run; ANGLE requires `pixel_graphics_provider='angle-gl'`. The controller
must pass the same manifest choice to preflight:

```python
bind_recovery.bind(adb, attempt, token, record, pins,
    cold_recovery=plan['coldRecovery'],
    pixel_graphics_provider=plan['pixelGraphicsProvider'])
```

That preflight records `graphicsProviderRequested`. Host admission requires exactly
one pinned record/provider preflight, then checks actual ready requested/observed/
fixed values before publishing a gate. The device checks these same fields in the
returned gate. Unknown, missing, duplicated or inconsistent manifest/argument
choices fail closed. Historical APK25/07 ANGLE results retain their original sealed
runner/source and remain scoped evidence; the new provider contract requires a
new matched test-only build, such as the prospective25d. It is not retroactively
compatible with the old diagnostic runner, which does not publish provider fields.

The platform provider exercises Android's installed EGL/GLES stack; packaged
ANGLE is an additional translation layer. Neither label establishes hardware
acceleration. On the current host, renderD128 advertises virtio3D_FEATURES=0,
CONTEXT_INIT=0 and no capsets, and host EGL reports llvmpipe. `-gpu host` cannot
produce a phone GPU through this host configuration. Preserve the running
SwiftShader AVD; changing the app provider does not require restarting Android,
but requires a fresh guest worker and explicit return of user UI ownership.
Virtual x86/software rendering cannot qualify physical Tensor/vendor GPU behavior.

## Original clock and after-Start admission

After a separate compiler/AVD allocation, use `pixel_supervisor.py`, the Pixel
adapter around the unchanged bounded supervisor, with the same allocation
arguments and a new Pixel10/API36/16KiB attempt. Reuse the encrypted imported base through
the reviewed external-COW route in COMPARISON.md. A retained dirty record requires
explicit `-e pixelColdRecovery true`; otherwise the overlay requires a clean
record. It checks actual selected metadata and records priorDirty/recovery mode.
Recovery does not claim clean initial admission. Capture the actual supervisor's original process clock once immediately
after launch:

```sh
python3 Android/scripts/pixel/capture_origin.py \
  --attempt /private/current-attempt --supervisor-pid ACTUAL_PID \
  --exec-session ACTUAL_SESSION --avd LightTouch_pixel_10_API36_16384
```

Schema2 `clock-context.json` derives `originNs` directly from process start ticks.
It cannot overwrite an existing context. Generate a unique32-digit lowercase hex
token and programmatically hash the unchanged clock-context bytes. Run this
instrumentation asynchronously on only the allocated emulator:

```sh
adb -s emulator-5554 shell am instrument -w -r --no-restart \
  -e class org.lighttouch.android.GuestPixelBootDiagnosisTest \
  -e diagnoseVirtualPixel true -e recordId VERIFIED_NEW_RECORD_ID \
  -e expectedMainApkSha256 SEALED_MAIN_APK_SHA256 \
  -e pixelGraphicsProvider platform \
  -e pixelAttemptToken UNIQUE_32_LOWERCASE_HEX \
  -e pixelOriginToken SHA256_OF_ORIGINAL_CLOCK_CONTEXT \
  -e pixelObserveSeconds 300 -e allowOneSleepWake true \
  org.lighttouch.android.test/androidx.test.runner.AndroidJUnitRunner
```

The diagnostic fixes the explicitly selected provider and arms private read-only
QMP before Start. It atomically publishes `ready.json` after Start, including UID/PID/start
ticks, device boot ID and pre/post-Start Android elapsed times. While the test
waits, invoke the host admission command:

```sh
python3 Android/scripts/pixel/admit_observation.py \
  --adb /path/to/selected/adb --attempt /private/current-attempt \
  --token SAME_UNIQUE_32_LOWERCASE_HEX
```

The host checks current UID/boot/record/worker identities and publishes
`pixel-bound.json` for scoped cleanup. It then brackets a fresh device BOOTTIME
read with host MONOTONIC times, allowing precision, delivery and a1s margin and
reserving20s for readback. The test waits at most20s and verifies the full300s
still fits its explicit device limit. Late setup/Start/admission refuses the
interval; it never shortens300s or backdates the origin. The original
UI240/readback810/Stop840/hard900 bounds remain. Distinct epochs are not directly
subtracted or treated as proof of equal future clock rates. Host completion and
readback must still be witnessed by original810; the device limit supplements
the independent original watchdog.

The Activity stays foreground for the full admitted interval, recording native
observations, raw images and CPU snapshots every10s. QMP can perturb timing and
is diagnosis, not a performance benchmark. One explicitly enabled balanced150ms
Home pulse follows observed sleep without a pre-wake agent-liveness requirement.
Inspect the actual subsequent image; admission/counters do not prove a response.
Eventual Scenario.close may pause the guest; the supervisor owns semantic Stop
and strict shutdown verification. Retain failures and writable state unchanged.

## Bounded evidence before disposal

The Pixel adapter runs a one-time readback hook before Stop/cleanup, including
early worker/instrumentation failures. Capture runs in a separate child capped
at10s and original810, with disposal reserve. Stalled ADB/local IO cannot hold
normal Stop/owned cleanup. The900s hard watchdog is unchanged. Any still-alive
capture child is explicitly recorded, not counted as retired.

`diagnostic_control.py` copies only the declared token's ready/result/timeline,
derived raw frame/QMP paths and bound record/PID engine/Stop logs. It rejects
scope/path/symlink, boot, UID, PID-start or process-role mismatches; caps one file
at4MiB and received remote data at32MiB; and records remote exits even when ADB
returns0. Permission failure is not absence. Proven worker absence permits only
historical bound files, never a live verdict. Partial reads/errors and forced
child disposal are retained in private receipts. Receipt failure does not
suppress Stop or owned cleanup.

The hook does not acquire an Android surface screenshot or classify a scene.
Take a separately bound surface image during live observation when required.
Normal scheduled Stop occurs at840, after the810 readback gate; its expired hook
does not override that gate. Collect successful/live diagnostic files before810
with a new private output directory:

```sh
python3 Android/scripts/pixel/diagnostic_control.py \
  --adb /path/to/selected/adb --attempt /private/current-attempt \
  --output /private/current-attempt/unique-live-readback
```

Reproducible host tests use synthetic ADB responses and do not prove target
behavior:

```sh
python3 -B -m unittest discover -s Android/scripts/pixel/tests -v
```

No helper/test receipt declares Home, physical Pixel equivalence, audible output,
persistence or a speedup. x86 SwiftShader cannot qualify Tensor ARM64/vendor GPU
compatibility. Gyro remains deferred.

## Memory comparison and observation

Use `--pixel-memory-mb 4096` or `8192` on the Pixel adapter (never combine with
`--memory-mb`). Both use the original fixed deadlines. `memory_monitor.py` records
host meminfo/vmstat/swap/PSI and zram provenance from launch through Stop and
owned cleanup; it samples guest memory/swap/PSI individually, with unavailable
reads explicitly null. Worker status/PSS is bounded by UID/start-ticks/role checks
before and after the read; denied PSS remains unavailable. Host swap counter
deltas include actual host page bytes. Requested cadence is2s; actual command
times and gaps are retained, so this is not a promise of gapless sampling.

Before owned cleanup, guest reads are disabled and acknowledged; host monitoring
continues. A stalled acknowledgement forces bounded monitor retirement and
records the resulting coverage gap. The sampler never retries/restarts an AVD,
changes swap/memory settings, or accepts a guest. A final host-only lifecycle
snapshot is recorded even after emulator disposal. Monitor failure cannot
suppress the original cleanup. COMPARISON.md defines the controlled experiment.

## Explicit existing dirty recovery binding

The retained dirty library uses `--fresh-library` only for the existing
supervisor's service/UI gate. It does not mean a fresh/clean library and does not
run import. Do not create an `import-record.json` receipt. Before Start use the
allocated attempt's token and actual original-record pins:

```sh
python3 Android/scripts/pixel/bind_recovery.py \
  --adb /path/to/selected/adb --attempt /private/current-attempt \
  --token SAME_UNIQUE_32_LOWERCASE_HEX --record DECLARED_RETAINED_RECORD_ID \
  --record-sha256 VERIFIED_RECORD_SHA256 --lock-sha256 VERIFIED_LOCK_SHA256 \
  --rom-sha256 VERIFIED_ROM_SHA256 --guest-sha256 VERIFIED_GUEST_SHA256
```

The readonly preflight caps10s and original490, verifies the original supervisor
and owned AVD process, current app UID/boot, unique engine PID/start/role, selected
record, metadata/base/bootrom/guest hashes and successful regular dirty-marker
read. It publishes `recovery-record-bound.json` once and writes no app state.
The sealed Kotlin still checks its actual worker is unused before recoverStart.
Run the test with `pixelColdRecovery=true`; after-Start admission rejects any
change from the preflight worker/record/clock/token or recovery mode.

Stop rechecks the original PID/start/UID/role/boot/selection and records that
record's dirty marker. Missing/mismatched identity produces unconfirmed Stop
admission, never a Stop sent to a replacement worker. Owned cleanup still runs.
No import, normal clean-record admission or lifecycle pass is inferred.

## Single identity and clean subsequent Start

`plan_manifest.build` constructs every stage from the same token, record, output,
mode and frozen main hash. Before any VM launch use the generated
`launchCheckedCommand`: the actual CLI checks all stage identities and actual
source/controller/launcher/APK hashes, requires Root's single-attempt allocation
and consumes it once. A copied stale admission token is rejected before exec.

The next same-child normal Start is supported by the actual03 native clean Stop
receipt, not a deleted marker. It uses the ordinary `--record-id` clean-record
supervisor gate and `coldRecovery=false`, and binds actual initialized state with
confirmed absent dirty marker. Permission denial is not absence. Historical03
files are for scoped later retrieval; they cannot prove a current guest scene.

Run generated `readyWatcherCommand` concurrently with diagnosis. It polls only
that attempt's token through original490, retains exact ready bytes immediately,
and publishes admission through the original after-Start clock/identity check.
The Kotlin20s wait and full300s are unchanged. Client completion is bounded by
original800, preserving10s finish plus10s scoped readback through810; setup time
is included and no fixed330s cap truncates the measured56s setup plus300s.

The supervisor itself now uses original process ticks, including CLI validation
and imports, for all gates/watchdog. Worker memory sampling and early-failure
readback can use the actual pre-Start binding when admission never publishes a
post-Start binding. Captures remain explicitly unconfirmed and never imply a
full interval, visible boot, physical Pixel or audible-output pass.

The immutable `clock-context.json` is written by the actual supervisor before
launching any AVD/helper. Read its `supervisorPid` directly; forked observer
command lines are not a way to identify the parent. The generated
`clockCaptureCommand` now uses `--attach-exec-session` to write a separate
`exec-session.json` after the tool returns its handle. This attachment rechecks
the live original PID/start/argv and preserves the clock bytes and binding hash.
An unknown tool handle is recorded as null in the original clock, never invented.

Normal Start's marker gate requires the exact positive `LT_MARKER_ABSENT` shell
status after proving the initialized state is readable. It rejects inaccessible,
missing or symlinked state directories and present/dangling markers. It retains
both actual streams and exit in `normal-marker-presence.json`; merged exec-out
diagnostics, permission errors and timeouts never mean clean state. The check
does not read, clear or rewrite a dirty marker.

Device clock reads use the public adb shell context, with one `cat` of boot ID,
uptime and boot ID. Both IDs must match the independently bound worker boot;
identity-sensitive app files remain under the app UID. The original host clock
brackets the complete read. Exact stdout/stderr bytes, status and bracket times
are retained before parsing in `pre-start-device-clock.json` and
`after-start-device-clock.json`. The same read is checked before Start, then
repeated freshly for full300s admission. A denied app-context proc read cannot
silently become a clock value, and no observation deadline is extended.

## Persistent Android and bounded guest attachment

User authorization now requires keeping the Android AVD running between guest
tests. `persistent_avd.py --plan /private/persistent-plan.json` requires a distinct
Root persistent allocation and retains its foreground handle. It writes the
actual wrapper/QEMU PID-start and Android boot binding to `owner.json`. Only an
explicit `REQUEST-STOP-AVD` file requests Android shutdown; guest completion,
failure and its900s deadline do not stop the AVD.

The existing supervisor accepts `--attach-avd-owner /private/owner.json`. Each
attached guest attempt still gets its own immutable original process clock,
actual owner/boot binding, fresh unused worker checks,240/810/840/900 bounds and
full300s admission. Normal cleanup uses the existing scoped guest Stop, retires
its logcat/monitor/watchdog, and leaves Android idle. Native clean Stop terminates
that worker; a subsequent guest must use a fresh unused worker. A hard fallback
can kill only the independently checked original app worker, preserves its dirty
state and never implies clean shutdown. It cannot signal the AVD/replacement PID.

For LAN viewing, verify the actual emulator adbd listener/protocol at host
loopback5555 after launch. Tunnel it through SSH instead of exposing the shared
ADB server5037. Example on the viewer's computer, using host192.168.1.45:

```sh
ssh -N -L15555:127.0.0.1:5555 blake@192.168.1.45
adb connect 127.0.0.1:15555
scrcpy -s 127.0.0.1:15555 --no-audio --no-control
```

Use read-only viewing while automatic measurements run; coordinate interactive
control after guest measurement ends. Endpoint availability/authentication must
be verified on the actual running AVD before reporting access as ready.

Parent SIGTERM/SIGHUP handlers perform no file IO: they retain signal/time fields
in memory and unwind into the existing cleanup. Cleanup records
`host-signals.json`, attempts the existing scoped Stop, then disposes owned
processes even if receipt/Stop fails. Forked helpers restore default handlers;
they cannot run the parent's cleanup. Repeated TERM/HUP cannot interrupt cleanup,
and the independent original hard-deadline SIGKILL remains enabled. Signal sender
is unknown. SIGKILL or external group disposal can still prevent graceful cleanup;
retain actual terminal/identity evidence rather than infer guest shutdown.


## Physical Pixel diagnosis

The virtual `GuestPixelBootDiagnosisTest` intentionally asserts x86_64. Do not run
it on an ARM phone or remove its assertion in a frozen test artifact. The sealed
25d runner also contains the ARM-compatible `GuestQmpSnapshotTest`, which can
start an explicitly selected existing clean record and retain fixed QMP
snapshots during a bounded foreground hold. It does not Stop the guest in
`finally`. A controller must retain its original clock, verify the current
record/assets/worker, collect the full observation and send ordinary semantic
Stop while the hold is still active. A late DeadObjectException after confirmed
Stop is a diagnostic test failure, separately recorded from the actual native
shutdown receipt. `am instrument` exit0 is not a JUnit pass.

The first physical Pixel10 diagnosis used unchanged main25 and test-only25d on
Android17/API37/arm64 with4096-byte pages. It retained304120ms of foreground QMP
samples and an independently viewed black→Apple logo→black sequence after one
production Home pulse, followed by confirmed native clean shutdown. The same
worker reached kernel/userspace with mixed alive/stale guest-agent samples.
The engine logged platform EGL/GLES, incomplete FBO and refused host context.
These observations do not establish a guest reboot loop, a16KiB cause or a fix.

A separate nonroot shell probe on the same PowerVR driver rejected packed
`GL_DEPTH24_STENCIL8` storage with `GL_INVALID_ENUM`; separate depth16/stencil8
attachments passed actual green and blue depth/stencil readbacks. Changing size,
name generation or wrapping alone retained the failure. App-UID execution was
denied before probe entry and no policy workaround was used. Shell UID2000 and
matching vendor mappings do not prove app GraphicsEnvironment equivalence.
These feature probes motivate a capability-based renderer fix; only a new real
guest run can qualify it. Private receipts and images remain in the workspace
under `builds/pixel/physical-pixel10-01/`; none are committed here.

The physical APK26 retest used the updated renderer on the same existing record,
without recopying assets. Its platform PowerVR path selected D24 with separate
stencil8. Model-inspected raw frames and Android surfaces showed the Lock Screen,
Home and Settings/About; the user navigated during observation. The actual
foreground QMP interval was 304026ms, followed by a strict native clean shutdown,
original-worker retirement, observed lease-free state and positive dirty-marker
absence. The engine logged zero incomplete-FBO or refused-context occurrences.
The late final status read still produced a separately retained JUnit
DeadObjectException after normal Stop. This run qualifies the observed rendering
fix and shutdown sequence; it does not establish a performance improvement,
deterministic input timing, Pixel audio/multi-touch/lifecycle acceptance or a
subsequent guest restart. Private evidence is under
`builds/pixel/physical-pixel10-apk26-02/`.

The restored API36/16KiB x86 AVD also tested APK26 with explicit `platform`
graphics. Its native Google SwiftShader GLES1 translator lacked required
`GL_OES_framebuffer_object`, and the engine refused the host context. Actual
frames showed the Apple logo/spinner, with no Home acceptance. The full300s
diagnostic completed, but ordinary Stop produced no native shutdown receipt;
the worker later sent itself SIG9 and writable state remained dirty. Retained
process absence and lease release do not make that a clean shutdown. Keep this
failed provider attempt and use a separately reviewed fresh-worker recovery
attempt for explicit `angle-gl`; never change providers silently or reuse a
pre-reboot clean receipt. Android remains running across these attempts.
