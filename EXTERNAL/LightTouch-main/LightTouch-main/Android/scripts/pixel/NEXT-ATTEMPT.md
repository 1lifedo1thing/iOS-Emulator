# Findings from virtual Pixel APK22 attempt01

Latest host-harness finding (APK24/8GiB normal04): Android reached the actual
service and clean-record gates at78.796s, but the retained tool session returned
exit143 before model-side original-clock capture or guest Start. No Python
failure/finally receipt was produced; the actual signal sender is unknown.
The emulator and observer remained owned and were explicitly retired using
their retained PID/start identities. All seven original parent file rows were
unchanged, with no open parent descriptors at terminal. Evidence:
`builds/pixel/attempt-apk24-api36-16k-8g-normal-04/terminal-summary.json` and
`host-supervisor-loss.json`. This is not an Android/native/iOS crash diagnosis.
The next harness persists its own immutable actual process clock before AVD
launch and unwinds observed parent TERM/HUP through bounded cleanup. Host OS
subprocess/fork tests cover that route; target execution remains required.

This is a failed virtual attempt, not physical Pixel qualification. API36/16KiB,
four KVM vCPUs,4GiB Android RAM, SwiftShader and the frozen APK22/test-only overlay
were used. Android startup, actual page size, signed APK installation and an
independently inspected unobstructed LightTouch UI passed. Reference import took
about242s and created an independent record. The iOS diagnosis failed after36.892s.

Android's retained log establishes the worker loss: LMKD killed PID4309/UID10215
at17:53:58.250, oom_score_adj100, to free502808KiB RSS and28416KiB swap, because
the minimum watermark remained breached. Zygote confirms signal9. This explains
this attempt's Binder DeadObjectException; it does not establish a native fault,
an iOS boot loop, or the user's physical Pixel10 failure cause. Stop found no bound
worker PID; the retained record was dirty and shutdown was unconfirmed. All owned
emulator/supervisor/observer/Xvfb processes subsequently retired. No automatic
restart or clean-shutdown pass was invented.

Private evidence: `builds/pixel/attempt-apk22-api36-16k-01/`, particularly
`worker4309-lmk-log.txt`, `android-all.log`, `angle-test.txt`,
`cpu-io-samples.jsonl`, `terminal-summary.json` and its evidence hash inventory.
The preserved AVD contains the scoped raw image/QMP files that were not retrieved
before immediate cleanup. A read-only4KiB block probe made no full disk copy;
the retained Android boot log explicitly identifies `/data` metadata encryption
and fscrypt. Direct ext4 inspection cannot recover those files with the currently
available offline tools. Preserve userdata/key images; do not reboot solely to
claim fresh readback or weaken encryption/security.

## Memory evidence and limits

Before iOS Start, the same worker performed compact import. At422.10s its Toybox
`top` row already displayed RES1.0G/SHR387M; the formatted RSS and percent-memory
fields do not supply canonical PSS or mapping residency. At483.47s, just before
diagnosis, Android MemFree was221888KiB with376432KiB swap used. The observer's
Android CPU/IO PSI reads were denied, not zero. Host MemAvailable at Android
readiness was about11GiB; host and the4GiB Android VM are separate memory domains.

The frozen native import implementation streams compressed payload using two
64KiB buffers. It retains the6,194,103-byte index string, a parsed JSON tree for
111319 entries, and a duplicate-name set during extraction, then releases their
C++ lifetimes on return. JNI persists a small library handle/root, not this
index/tree. This source review finds no intentional persistent archive tree;
allocator high-water retention, mapped libraries, framework memory and filesystem
cache are still possible. Existing samples cannot partition their contribution.
The import's index/tree/name-set lifetimes also span `Stage.finish()` and its base
validation, so validation work overlaps those allocations. Independently matched
APK22 sources eagerly size JNI's global pixel vector to the64MiB frame capacity
before guest Start; runtime creation sizes another64MiB scratch vector. The
shared64MiB memfd mapping is a separate reservation, whose residency cannot be
assumed. These source facts justify measuring frame-buffer residency and sizing;
they do not prove a particular share of LMK RSS or a tested memory fix.
Measure canonical worker VmRSS/VmSwap/PSS and `/proc/PID/smaps_rollup`/mapping
classes where permissions allow, before import, after import and before Start.
Record denied reads as unavailable. Avoid inferred heap leaks from one `top` row.

The n72 guest RAM argument is128MiB. The system-mode TCG default is bounded by
min(1GiB,host-physical-memory/8), also architecture-limited and host-page-aligned.
Here “host” means the Android process environment, giving approximately490MiB
of potential TCG code-buffer reservation from the measured4013824KiB Android
MemTotal. Reservation is not resident consumption, and the pre-Start import
memory cannot be assigned to active TCG translation. Do not blindly shrink the
cache or attribute LMK to this default. Compare actual mapping/PSS and generated
code usage first; changing Android RAM also changes the automatic TCG default.

## Reuse the import and separate its worker lifetime

The imported base remains useful. Preserve the failed dirty record and its state
unchanged. A next allocated attempt should reuse the imported read-only base,
without reimporting111319 files or copying the entire AVD/base. The current
production library pins each record to its own `base`/`state` paths; it has no
reviewed shared-base fresh-record operation. New small writable state therefore
needs an explicitly reviewed domain/tool route, or the existing dirty record
needs explicit recovery. Neither may silently erase the dirty marker or be
described as the same clean initial state.

For future fresh imports, a QEMU-unused worker is not necessarily a fresh native
or JVM heap. Retire only the verified idle import worker after successful import,
complete staging, no in-flight operation, `sessionStarted=false`, no guest/runtime
and clean retained state; then boot in a new PID. The current normal Stop path
expects a valid native runtime poll, which an unused worker lacks, so it can fall
through to the35s disposal deadline. An explicit idle-worker retirement contract
would avoid mislabeling that as guest shutdown and would keep UI/base/state
intact. No such API or process-kill experiment has been implemented here.

A controlled follow-up should first change the worker lifetime while keeping
Android RAM, APK, provider, base and admitted state route explicit. An8GiB VM is
an alternative capacity experiment, not a fix proven by this4GiB failure. Record
any recovery/new-state change and automatic TCG sizing as additional factors.
No additional AVD or memory-setting change is authorized by this document.

## Preserve evidence before disposal

The current controller requests Stop immediately after instrumentation failure;
the supervisor can dispose the AVD before interactive readback. Add a reviewed
precleanup hook while the emulator is still owned and alive. Capture only the
already bound diagnostic result/timeline/QMP directory, its raw screenshot
directory, that record's engine log and worker identity/exit/dirty receipts.
Use fixed validated relative paths, bounded byte totals and explicit remote exit
classification; never scan unrelated records, app data or the whole disk.

The hook needs an independent bounded readback deadline, at most10s and never
later than the original810s readback gate. It must finish or cancel before normal
Stop/cleanup and must not delay original840s Stop or900s disposal. Retain partial
files and errors when the device/worker is already gone. Hook completion is not
native shutdown confirmation. This attempt's missed readbacks remain failures;
offline recovered images would be historical captures, not current runtime proof.

## Repair the observation budget contract

The controller admitted the300s test at488.917s, but the test creates its deadline
after Activity/binding/provider setup and `worker.start`. Its initial native
MONOTONIC sample was512.002459055s in the Android epoch and its Android
elapsedRealtime deadline was812.002s. Android and supervisor clocks have different
origins and must not be subtracted directly. The488.9s admission guard alone
cannot prove that300s observation plus20s readback fits original810s.

Record host admission, actual pre/post-Start Android elapsedRealtime and native
MONOTONIC, and host receipt times for the matching worker/start handshake. The
host must verify the actual observation-start receipt plus300s and readback
reserve against its original fixed origin. Supply/check an explicit conservative
device deadline only through a bracketed clock correlation; never assume equal
epochs. If actual setup cost leaves insufficient budget, retain a diagnosis
admission failure and stop. Do not silently shorten the300s interval, reset the
origin or backdate a start. This attempt failed early, so no full interval or
successful timing contract is claimed.
