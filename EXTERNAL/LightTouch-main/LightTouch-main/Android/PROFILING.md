# Profile an existing Android guest

Use the installed Debug candidate with a running foreground guest and its
current dedicated `:engine` PID. The helper attaches; it does not start an
Activity, restart/stop the worker, compile Java, invoke adb root/unroot, or write
system properties. Keep recordings private. Results do not establish guest
rendering, audible output or lifecycle acceptance.

## Working Poco APK22 route: platform recorder, plain task-clock

The first usable APK22 recording ran `/system/bin/simpleperf` directly under
`run-as org.lighttouch.android`, without `--app` redispatch. Actual platform
version was `1.build.1790963387`, SHA-256
`a258fcfcfa04b6336ab52f328bcd6a9ae6f3dff8e0aeb5f6950006ac6362e148`.
This is platform-recorder evidence, not an NDK-recorder claim. Read back and retain
the version/hash, existing kernel limits and target identity on each device;
Pixel kernels and profiler versions may differ.

Before recording, verify the existing worker UID/PID/start ticks, installed APK,
and every selected `/proc/PID/task/TID` UID/Tgid/Pid/start tick/comm. Keep the
Activity resumed and declare the workload. The successful attempt selected all
22 verified existing TIDs with `-t` and `--no-inherit`; newly created threads were
outside this scope. Recheck the same identities afterward and reject a replaced
or exited worker/thread. Do not use stale TIDs from a previous attempt.

The following recording recipe assumes those checks have passed and that
`LT_VERIFIED_TIDS` contains only the verified comma-separated decimal IDs. Choose
a new private host directory and a new app-private output directory, both outside
Git; never overwrite earlier evidence. The existing
`/proc/sys/kernel/perf_cpu_time_max_percent` was25 and
`/proc/sys/kernel/perf_event_mlock_kb` was516 during the successful run. Passing
`--cpu-percent 25` retained that existing value. Do not copy it onto a device with
a different existing limit or change settings to make recording succeed.

```sh
# LT_ADB, LT_SERIAL, LT_VERIFIED_TIDS and LT_PROFILE are operator-supplied.
# LT_PROFILE is an existing fresh host evidence directory, outside Git.
"$LT_ADB" -s "$LT_SERIAL" shell run-as org.lighttouch.android \
  mkdir files/lt-perf-private-01
"$LT_ADB" -s "$LT_SERIAL" shell run-as org.lighttouch.android \
  /system/bin/simpleperf record -t "$LT_VERIFIED_TIDS" --no-inherit \
  -c 50000000 -e task-clock --duration 10 --clockid monotonic \
  --call-graph dwarf,8192 -m 8 --user-buffer-size 16M --size-limit 32M \
  --cpu-percent 25 --no-dump-kernel-symbols \
  -o files/lt-perf-private-01/perf.data \
  > "$LT_PROFILE/record.stdout" 2> "$LT_PROFILE/record.stderr"
# Retain the actual command and exit status before any further command.
"$LT_ADB" -s "$LT_SERIAL" exec-out run-as org.lighttouch.android \
  cat files/lt-perf-private-01/perf.data > "$LT_PROFILE/perf.data"
```

The event is plain `task-clock` (user plus kernel), sampled every50ms of counted
task CPU time with `-c 50000000`. This is a period, not a fixed20 samples per wall
second across all selected threads. `-m 8` requests eight ring pages per CPU
(32KiB on the measured4KiB Poco kernel); DWARF stack size is8192 bytes. Retain the
actual page size and memory limits before adapting this recipe elsewhere. No
root, sysctl, property, security or privilege-mode changes were made.

Earlier platform `task-clock:u` failed because the kernel did not support its
user/kernel modifier. A one-page4KiB ring with8KiB DWARF stacks recorded zero
samples and29 kernel-lost samples. The eight-page attempt recorded228 samples,
zero lost, event count11,400,000,000 and exit0 over10s. It also warned that kernel
symbol addresses were restricted; retain the warning without applying its
suggested `kptr_restrict` change. A zero-sample result never identifies a
bottleneck, even if command exit is0.

Private reproducible evidence is under
`evidence/product/poco22-system-taskclock-buffer8-profile-01`: `command.json`,
`terminal.json`, `receipt.json`, recorder output, `perf.data` and reports.
The recording SHA-256 is
`305705441458e4776a24ba9b5e6b52ca610dd553755b104e9db761176aafa4ca`.
The workload was current guest Notes with no scripted touches, not a controlled
app launch. Sample shares were62.28% CPU0/TCG,33.33% combined across two
Binder-named native threads (guest main/presenter), and4.39% guest audio. These
are shares of sampled task CPU, not wall-time latency, whole-device utilization,
or proof of a menu/loading bottleneck. No measured speedup or causal optimization
claim follows from this steady-state profile.

## Historical NDK helper and retained inconclusive attempts

The selected NDK is28.2.13676358, simpleperf build12821384. The helper verifies
the exact Android and Linux report binary hashes before any device command.
Android's `--app` route can delegate to an app-context/platform recorder; retain
the actual recording cmdline/version instead of claiming every recording ran
the NDK binary. [AOSP app profiling](https://android.googlesource.com/platform/system/extras/+/main/simpleperf/doc/android_application_profiling.md)
describes the supported non-root debuggable-app route.

```sh
LT_NDK=/path/to/Android/Sdk/ndk/28.2.13676358
LT_SERIAL=your_selected_device
LT_ENGINE_PID=current_existing_worker_pid
LT_PROFILE=/path/to/private/new-profile-directory
python3 Android/scripts/profile-existing-guest.py \
  --ndk "$LT_NDK" --serial "$LT_SERIAL" --engine-pid "$LT_ENGINE_PID" \
  --scope engine --event cpu-cycles:u --out "$LT_PROFILE"
# Review the plan, then add --execute to the same command.
```

Use `--scope app` for a separate whole-app recording of the exact running app,
engine and firmware process identities. These process modes select `-p` and
collect threads of the selected processes. The separate `--scope engine-threads`
mode enumerates `/proc/ENGINE_PID/task` under the app UID, verifies each task's
Uid/Tgid/Pid, start ticks and actual comm, and passes **only `-t VERIFIED_TIDS`**.
It adds `--no-inherit`, does not add `-p`, and does not collect system-wide. The same selected TIDs must retain
their owner/start ticks/comm afterward; an exiting or reused TID invalidates the
recording. Threads created later are outside that explicit selection. Recording is fixed
at10s/99Hz, with an8KiB DWARF stack,16MiB user buffer and32MiB file limit. The
binary/output directory is a fresh `/data/local/tmp/lt-perf-<uuid>`; unrelated
profiler outputs are untouched. `receipt.json` retains all commands, selected
UID/PID/start ticks and scoped thread mappings before/after. `ps NAME` can show
the process name for every thread, so the helper also reads each owned thread's
actual `/proc/PID/task/TID/comm`.

The pinned profiler can internally adjust kernel limits. The helper instead
requires existing paranoid<=1, sample-rate>=4000 (its internal default even
with requested99Hz), an unchanged valid cpu-percent and enough existing mlock
budget for tiny rings. If that conservative read-only preflight, app-context
permission or recording fails, retain the failure and use the counters below.
Do not apply the profiler's suggested settings/security workaround. The helper
avoids NDK wrappers that switch ADB privilege mode or use shared output paths.
[AOSP command reference](https://android.googlesource.com/platform/system/extras/+/main/simpleperf/doc/executable_commands_reference.md)
documents process/thread selection, sample frequency and reporting.

The helper runs the pinned host report and requires **both** nonzero Samples and
reported sample event count. Exit0 and a perf.data file are insufficient. A
zero-sample recording returns2 with `INCONCLUSIVE_ZERO_SAMPLES_NO_CPU_ATTRIBUTION`;
unknown/missing/ambiguous reports remain unavailable. Positive samples still
need symbol and workload review. Use exact unstripped engine/JNI inputs from the
sealed producer's input-to-packaged mappings; do not substitute a newer build.
Use build IDs only where they actually exist. The APK22 engine has no GNU build
ID: its unstripped input SHA-256
`bc5694ab7f2d6bf390186f4b2ce9fadf5b1511a50d7d022d1096729fc80efb65`
was matched by the integrator's NDK-strip/SHA verification to packaged engine
`b09f134a50fe2274d14e58e1ed70f80ec96904cfdea8af735bb5727f92872ca1`.
The report's `--symdir` lookup did not symbolicate that engine. Retained
DSO-relative PCs did resolve through the matching input's DWARF with
`llvm-addr2line -Cfipe /path/to/exact-unstripped/libqemu-arm.so 0xRELATIVE_PC`.
Keep the report's DSO/PC association and the verified mapping; a raw absolute
address or a guessed GNU build ID is not a valid replacement. Exact commands and
resolved engine locations are retained in the successful attempt's
`engine-symbol-command.json` and `engine-pc-symbols.txt`.

The first actual21b recording used `task-clock:u` while a static Photo was
selected. Recording/report commands succeeded but produced0 samples/event
periods. The engine's process CPU counters advanced across the wider
preparation/record bracket, so this means inconclusive sampling, not CPU idle.
The next `cpu-cycles:u` process recording also produced zero samples, but that
guest was paused after ActivityScenario.close; it is not an active benchmark.
A controlled explicit-thread variant is:

```sh
python3 Android/scripts/profile-existing-guest.py \
  --ndk "$LT_NDK" --serial "$LT_SERIAL" --engine-pid "$LT_ENGINE_PID" \
  --scope engine-threads --event cpu-cycles:u --out "$LT_PROFILE"
# Review the plan, then add --execute. Use a new evidence directory each time.
```

The operator must explicitly resume the same guest after any SDK scenario
closes, then perform the declared live menu/Photos workload during recording.
Neither zero-sample result identifies a kernel or profiler defect. Unsupported
hardware events remain a retained permission/event failure; never change
settings to make them work.

## Paired counters and responsiveness

Keep the same worker UID/PID/start ticks and resumed Activity throughout the
observed interval. Obtain actual `IEngine.status()` snapshots through an already
attached app-UID observer; this host helper does not start instrumentation to
collect them. Instrumentation completion/ActivityScenario.close can pause or
stop the target and confound the profile. Retain observations and raw captures
before, during and after the operator's declared workload.

- Compare `producedFrames`, `presentedFrames`, frame serial/timestamps,
  presentation-gap buckets and window/copy failures. A static frame can be
  correct; copied/presented counters alone do not prove a new guest response.
- Compare pulled PCM/nonzero sample energy, written frames/playhead,
  write failures and underruns, with the same audio observation baseline.
  Cumulative nonzero PCM is not simultaneous touch/render/audio proof.
- Preserve existing and new input errors. A retained historical rejection is
  not cleared by profiling or by successful queued admission. Do not bypass
  acceptance test guards without a reviewed fault/baseline contract.

For a future normal launch measurement, capture the fresh worker's
`observations.hostMonotonicNs` immediately before start admission and retain the
same-worker first produced/copy/present sequence. Frame/publish/presenter times
share the device native monotonic epoch. A later balanced interaction followed
by a fresh coherent changed frame and inspected guest response establishes
responsiveness more strongly than a boot-logo frame. Keep the Activity resumed
until after the settle/capture boundary. Record both earliest nonzero frame and
first inspected responsive scene separately; no automatic scene classification.

Host ADB command times, Android elapsedRealtime (BOOTTIME), MotionEvent uptime,
PCM capture-relative timestamps and native MONOTONIC are distinct domains.
Do not subtract across them. An existing worker cannot provide retrospective
launch latency, and an ADB `am start -W` timing is not guest boot latency.
Candidate21b keeps candidate20's QEMU engine, but also changes JNI/Runtime
optimization, ownership, telemetry and Stop receipts; a20→21b comparison cannot
attribute an improvement solely to `-O2`.
