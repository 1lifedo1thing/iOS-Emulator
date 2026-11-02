# Observe shutdown admission without JNI

`IEngine.debugShutdownProgress()` is available only in a debuggable app and on
the private worker Binder. It returns one immutable cached Java snapshot. It
does not call `Native.status`, observations, filesystem, audio, or lifecycle
admission code. It does not request Stop or change the shutdown budget.

The snapshot identifies the worker and uses Android
`SystemClock.elapsedRealtime` (BOOTTIME) milliseconds. Missing timestamps and
absent `lastReturnedNativeStatus` mean **unknown**, not zero work or a successful
native poll. `snapshotAgeMs` and the cached native result's `ageMs` identify old
information. They are not host-PC time or guest virtual time.
Diagnostic publication is best effort: an allocation or diagnostic-clock failure
may leave an older snapshot, and cannot prevent native admission or disposal.

Each callback has `entered`, `returned`, and `threw` counters and its last
entry/return timestamp. `returned` counts normal Java callback returns;
`threw` counts caught exceptions. Counters describe recorded callbacks and may
be incomplete if diagnostic publication fails. Neither establishes engine acknowledgement.
A cancel callback returns no cancellation result. The four phases distinguish
`shutdown-admission`, `contact-cancellation`, `terminal-poll`, and
`terminal-receipt`. A blocked second status call can therefore coexist with a
cached result from the first returned poll. Poll flags must be interpreted only
when that cached poll's `pollSnapshotValid` is true.

`disposalReason` records the coordinator's single winning disposal decision.
Even cached native clean flags cannot prove that terminal receipt persistence
completed. This accessor never publishes `cleanShutdownConfirmed`; verify the
independent PID-qualified durable terminal receipt and retained state using the
shutdown evidence tools. Missing deadline files remain unknown: their best
effort writer races process disposal.

## Optional existing-worker collector

Use a matched main/test pair that includes this method. With the foreground
worker identity freshly verified, run the collector using the existing session:

```sh
adb -s "$SERIAL" shell am instrument --no-restart -w \
  -e class org.lighttouch.android.GuestShutdownProgressTest#observeCachedProgressWithoutCallingNativeOrRequestingStop \
  -e observeShutdownProgress true \
  -e expectedMainApkSha256 "$MAIN_SHA256" \
  -e expectedWorkerPid "$WORKER_PID" \
  -e expectedWorkerStartTicks "$WORKER_START_TICKS" \
  -e expectedBootId "$ANDROID_BOOT_ID" \
  -e shutdownProgressSeconds 40 \
  org.lighttouch.android.test/androidx.test.runner.AndroidJUnitRunner
```

The collector binds with flags `0`, verifies APK/boot/PID/UID/start ticks, and invokes
only `debugShutdownProgress`. It creates no service or Activity and performs no
Start, recovery, Resume, input, or Stop. An authorized operator independently
uses the production Stop control while it observes. It samples at most four
times per second for up to 45 seconds. Private
`files/shutdown-progress-evidence-*/samples.jsonl` retains per-read BOOTTIME
bounds, including old cached results; `result.json` never qualifies guest or
clean-shutdown acceptance.

Individual Binder calls still require an external observer deadline: a fully
occupied Binder thread pool may prevent a response even though the accessor
itself takes no admission lock. Do not infer a native stage from a missing
sample, start a replacement worker, or repeat Stop to obtain different evidence.
This collector has not been executed on a target merely by compiling it.

The existing fresh-worker `debugArmQmp()` opt-in also enables the engine's
bounded shutdown-decision trace (`IT_SHUTDOWN_DIAG=1`). Ordinary starts explicitly
set it to `0`; there is no new provider, shutdown route or deadline. Matching
engine support is required. Engine trace timestamps use host monotonic time,
while these Java snapshots use BOOTTIME; do not subtract them without paired
clock calibration, especially across device suspend.

The shared publisher allows an agent it has previously observed up to five
seconds to refresh after resume or cancellation before asking the engine to
choose a shutdown route. It polls without blocking the publisher or BQL and
uses diagnostic phase `6` while waiting. This consumes the existing 35-second
shutdown budget. A board on which no agent has ever appeared retains immediate
fallback; an agent that never refreshes reaches fallback after the fixed wait.
Neither that fallback nor a successful request constitutes confirmed shutdown.

When this previously observed agent's guest display is asleep, the publisher
first sends one balanced Home pulse (150 ms). VM Resume alone does not wake
the iOS display or its sleeping daemon. Home release is submitted before route admission;
the pulse and agent polling share the same fixed five-second refresh window
and original 35-second shutdown deadline. An awake display and a board with no
observed agent receive no extra button input. This behavior still requires
target qualification; it does not turn a shutdown request into a clean receipt.
The engine button API is asynchronous; submission does not prove actual GPIO
release. Once Stop owns input, later external contact cancellations reset the
local ledger without enqueueing another full engine cancel. The publisher owns
the initial cancellation, wake pulse and final cleanup under that boundary.
