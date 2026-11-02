# Read-only shutdown evidence check

Run this metadata verifier against a private, retained Android evidence directory:

```sh
python3 -B Portable/firmware/tools/verify-shutdown-evidence.py /private/run-evidence
python3 -B Portable/firmware/tests/shutdown-evidence.py
```

The first command returns zero only when clean-shutdown evidence fields agree.
It writes nothing and operates no device, emulator, guest or firmware tool. The
tests use synthetic JSON only. Neither command authenticates a collector's facts,
checks guest pixels/media, or closes G6/G7. Keep all real inputs outside Git.

## Bounded Root-operated collection

`tools/collect-shutdown-evidence.py` is an opt-in ADB collector template. Running
it accesses the named device; only the allocated integrator may do that. This
source lane tests it offline with mocks, and has not run it on a target. It never
requests Stop, wake, touch, recovery, installation, process termination or marker
changes. It reads small metadata only, with a ten-second per-ADB timeout and a
sixty-second phase budget. Each phase requires a new private host directory;
failed/partial reads remain there and a retry must use another new directory.

Create a private `run-pins.json` with the expectation fields illustrated below,
omitting `clock` and the two acceptance-window values. Use installed artifact
hashes from the integrator's independent installation/producer check. This
collector does not sample the APK/native hashes or read NAND/ROM/package assets.
Then, while the current owning guest worker is still running:

```sh
python3 -B Portable/firmware/tools/collect-shutdown-evidence.py before \
  --serial DEVICE_SERIAL --expected /private/run-pins.json \
  --out /private/stop-before-UNIQUE
```

The integrator now requests production Stop separately and waits for the owning
worker to retire. Do not stop/recover/reimport from a second owner. Collect into
another unique directory and run the verifier:

```sh
python3 -B Portable/firmware/tools/collect-shutdown-evidence.py after \
  --serial DEVICE_SERIAL --before /private/stop-before-UNIQUE \
  --out /private/stop-after-UNIQUE
python3 -B Portable/firmware/tools/verify-shutdown-evidence.py /private/stop-after-UNIQUE
python3 -B Portable/firmware/tests/shutdown-collector.py
```

Before collection retains owning `/proc/PID/status`, UID, engine-process command
line and start token, selected record, original record/lock bytes and writable
state's base pin. After rechecks selected record, unchanged metadata/state pin,
original terminal/deadline files, owning PID absence and readable state/marker
facts. Readability failures abort rather than turning into absence. Raw ADB argv,
stdout/stderr, local exit and an explicit remote exit sentinel are retained.
The normalized terminal/record/lock files preserve original captured bytes.

Both phases capture the device boot ID and `/proc/uptime`; a changed or unreadable
boot ID fails. The acceptance window starts at the pre-Stop device uptime lower
bound and ends at the post-Stop upper bound. This brackets Stop admission; it is
not an invented exact UI-action timestamp. Two fractional digits in procfs mean
a ten-millisecond interval, not precise milliseconds. Host monotonic timestamps
bound collector I/O only and are never compared with the native receipt. Source
candidate21b passes `SystemClock::elapsedRealtime` to its shutdown coordinator.
The [Android contract](https://developer.android.com/reference/android/os/SystemClock#elapsedRealtime())
includes sleep, as does [Linux /proc/uptime](https://www.kernel.org/pub/linux/docs/man-pages/book/man-pages-6.12.pdf).
The collector uses those same-boot elapsed-time contracts and preserves their
precision; it does not measure a proprietary device clock implementation or
claim that Android elapsed time equals the Linux computer's clock.

`clock-binding.json` preserves boot ID, bracket precision and before-manifest
hash. Keep both complete phase directories and the independent artifact binding.
The collector does not itself authenticate installation or prove guest photo
persistence. A failed before phase must not be used to establish a Stop window.

The directory contains these files:

- `worker-shutdown-PID.json`: original terminal receipt from the actual candidate
  worker, copied without rewriting. Native13 must report a valid stopped snapshot,
  explicit confirmation, clean state, no storage failure and engine exit zero.
  The receipt must state native-terminal, no inferred confirmation/marker edits,
  no error and durable rather than best-effort persistence.
- `record.json` and `device.lock.json`: retained actual selected record and its
  immutable base lock. The lock byte hash must equal the expected base identity;
  board/build/direct-iBoot route and matched package must agree.
- `expected.json`: integrator's explicit run expectations, shown below. Bind them
  to installation/producer and Start/Stop evidence before running the checker.
  Do not derive expectations solely from the terminal receipt being checked.
- `post-stop.json`: normalized observations from the read-only collector, shown
  below. Retain raw commands, actual remote exit codes and their output separately;
  local `adb exec-out` zero can conceal a failed remote command.

Example shape (all IDs/hashes here are synthetic placeholders):

```json
{
  "schema": 1,
  "clock": "android-elapsed-realtime-ms",
  "baseKind": "generated-stock",
  "workerPid": 123,
  "workerUid": 10001,
  "recordId": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "baseSha256": "<actual lock SHA256>",
  "bootromSha256": "<actual matched record ROM SHA256>",
  "guestPackageSha256": "<actual matched record package SHA256>",
  "artifactPins": {
    "mainApkSha256": "<actual installed main APK SHA256>",
    "jniSha256": "<actual packaged native JNI SHA256>",
    "engineSha256": "<actual packaged engine SHA256>"
  },
  "shutdownGraceMs": 35000,
  "acceptanceWindowStartMs": 900,
  "acceptanceWindowEndMs": 1100
}
```

Use the actual compiled grace from the producer's BuildConfig/shared header.
The acceptance window identifies this Stop request, preventing a stale receipt
from a reused PID from passing. All these times and post-stop observation times
must share the same device boot's Android elapsed-realtime clock. A Linux host's
monotonic nanoseconds are a different clock and cannot be substituted or compared
without separate synchronization evidence.

```json
{
  "schema": 1,
  "clock": "android-elapsed-realtime-ms",
  "workerPid": 123,
  "workerUid": 10001,
  "recordId": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "baseSha256": "<same expected base SHA256>",
  "artifactPins": {"mainApkSha256": "<same APK>", "jniSha256": "<same JNI>", "engineSha256": "<same engine>"},
  "observedAtHostMs": 3100,
  "workerProbe": {"remoteExit": 0, "pids": [124]},
  "stateDirectoryProbe": {"remoteExit": 0, "isDirectory": true},
  "dirtyMarkerProbe": {"remoteExit": 0, "exists": false}
}
```

Probe booleans mean the command explicitly tested that fact successfully, not
that a failed `stat` or unreadable path was normalized to absence. Another idle
service PID may exist; the stopped owning PID must be absent. These bindings are
collector assertions: the production terminal receipt itself contains PID/UID
but no record ID, APK hash or clock boot ID. Correlate raw selected-record, worker,
installed-artifact and same-boot evidence. The checker rejects any same-PID
`worker-shutdown-deadline-PID.json`, including malformed or empty evidence; keep
the directory complete so conflicting evidence cannot be hidden by omission.

Keep current nondeferred acceptance distinctions:

- A reference-fixture demo import creates fresh state; it does not establish the
  generated-stock userspace preparation gate. Set `baseKind` honestly and bind
  its actual lock identity. Matching board/package does not make identities equal.
- Native13/source tests and kit correspondence require actual new-worker receipts
  on that compiled APK; they cannot retrofit missing receipts from older runs.
- Confirm Home on the selected generated-stock record, then show an identifiable
  guest photo after an ordinary same-state clean Stop/Start in a fresh worker.
  Preserve pre/post scene evidence and state/lease continuity. A photo count, host
  Screenshot, cancelled gesture, Lock screen, Mac test or Linux test is insufficient
  for Android saved-photo persistence.
- Retain terminal-confirmation and process-retirement durations separately. This
  verifier measures acceptance-to-terminal and acceptance-to-post-probe; it does
  not infer the exact process exit time or an OS scheduling guarantee.
- Sustained interactive/audio/render, required multi-touch/lifecycle, Linux/Android
  and 16KiB guest qualification remain separate G7 evidence. Gyro/motion is deferred
  and unqualified, with no initial acceptance dependency.
