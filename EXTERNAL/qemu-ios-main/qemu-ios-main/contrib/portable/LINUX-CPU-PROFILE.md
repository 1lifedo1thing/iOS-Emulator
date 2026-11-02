# Finite Linux native CPU sampling

`linux-cpu-profile.py` records **40 wall seconds** on an already-owned Native14
worker and an explicit list of its existing threads. It sends no guest input,
does not start/stop the worker, and does not alter kernel settings. Collection
requires a matching external allocation; preflight alone never attaches perf.
Use a private directory outside Git. Native engine binaries remain unchanged.

The operator can derive a schema1 plan from the actual live worker immediately
before the allocated measurement. Supply only owner-verified existing TIDs;
preparation reads identity/mapping metadata and hashes but never attaches perf:

```sh
python3 contrib/portable/linux-cpu-profile.py prepare --pid ACTUAL_WORKER_PID \
  --thread ACTUAL_TCG_TID:TCG --thread ACTUAL_MAIN_TID:qemu-main \
  --engine /exact/frozen/libqemu-arm.so --plan /private/plan.json \
  --output /private/new-recording
```

The emitted fields are:

```json
{
  "schema": 1,
  "pid": 123,
  "uid": 1000,
  "host_boot_id": "actual-host-boot-id",
  "start_ticks": "actual-proc-stat-starttick",
  "argv": ["/exact/lighttouch-linux", "--worker", "actual-worker-arguments"],
  "threads": [{"tid": 124, "start_ticks": "actual-thread-starttick",
               "comm": "actual-comm", "role": "owner-verified-TCG"}],
  "engine": {"path": "/exact/frozen/libqemu-arm.so", "sha256": "actual-sha256"},
  "perf_sha256": "actual-sha256-of-/usr/bin/perf",
  "output": "/private/new-recording"
}
```

Record the actual full argv, not this illustrative list. Linux `/proc/*/stat`
start ticks are strings. Roles come from launch/thread evidence; inherited
Binder-like names are not proof of Binder IPC. The engine must be the actual
mapped, unstripped frozen engine. No old control/candidate substitution.

```sh
python3 contrib/portable/linux-cpu-profile.py collect --plan /private/plan.json
```

After reviewing that exact plan, Root writes its sibling `ROOT-GRANT.json` with
`plan_sha256` and `tool_sha256` (the SHA256 of this script), then allocates:

```sh
python3 contrib/portable/linux-cpu-profile.py collect --plan /private/plan.json \
  --execute-approved-sha256 ACTUAL_PLAN_SHA256
python3 contrib/portable/linux-cpu-profile.py report \
  --recording /private/new-recording --output /private/new-report
```

The fixed event is `cpu-clock:u`, period20,000,000ns (about50 samples per
user-CPU second), DWARF8192, eight mmap pages, explicit TIDs, no inheritance.
`--no-buildid-cache` preserves build IDs in the recording but suppresses perf's
default cache update. Without it perf can hardlink a frozen source DSO into its
build-ID cache, changing that DSO's link count and ctime despite identical bytes,
mode and mtime. Historical recordings made before this option retain their
original commands and metadata-check failures; do not relabel those runs.
Sudo is noninteractive; its own timeout is45s plus3s termination grace. The
collector checks worker/thread start identities and engine mapping every0.5s,
uses a worker pidfd for exit detection, and stops on identity changes. This
observer overhead is part of the measurement. A thread disappearing fails the
recording rather than silently changing scope. File size is capped at256MiB
per profiler/report file (RLIMIT_FSIZE plus polling). Cleanup signals only the
collector's own session/group members, with PID/start checks, never the worker.
Failure and cleanup failure remain explicit in `terminal.json`; no automatic
retry. Sudo returns only the newly created private `perf.data` file to the
ordinary user's ownership so offline reporting needs no privilege.

Offline reporting disables debuginfod downloads and retains raw stdout/stderr,
self and inclusive reports, samples and mmap/task/lost records. Each report
command has a30s timeout and the same per-file size limit. Engine, perf and
recording hashes are checked. Unknown output formats invalidate automatic
classification and remain in the report; the underlying reports are preserved.
Loss must be inspected explicitly before quantitative attribution.

Self samples distinguish native `tb_gen_code`/decoder, `tcg_optimize`, host
emission/register allocation, execution/MMU helpers, device/MMIO, graphics and
I/O wrappers. These are conservative symbol-name categories; optimized/inlined
functions may appear only in inclusive stacks. Inclusive counts overlap and
must not be added to self counts. Unknown addresses, including anonymous names
containing `tcg`, remain unresolved without recording-time mapping evidence.
No perfmap, jitdump, per-TB trace or guest-kernel symbols are enabled.

For cold/repeated menus, keep one finite40s recording with separately retained
host-monotonic input and actual frame/Surface endpoints, or two separately
allocated recordings. Use sample timestamps for phase bins; no automatic tap or
scene classification is performed. “First observed” is not a cleared-cache
claim. Statistical native code-generation CPU allocation can support a cost
hypothesis; net JIT-buffer growth alone cannot measure that CPU cost, and
neither establishes causality for file/guest initialization or visible latency.

Host-only safety/format tests (no perf, sudo or guest):

```sh
python3 tests/portable-linux-cpu-profile.py
```
