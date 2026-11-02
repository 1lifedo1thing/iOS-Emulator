# Narrow Linux supporting-thread CPU observation

`linux-cpu-profile.py prepare --preset user-kernel-10s` is an optional diagnostic,
not a changed engine/default or a performance improvement. Existing plans without
`capture` retain the original 40-second, `cpu-clock:u`, 256 MiB, up-to-64-thread
capture. New plans bind the entire fixed preset; the exact plan and collector
hash remain required in the integrator's `ROOT-GRANT.json` before attachment.

## Question and allocation

An Android static-Home observation attributed substantial system CPU to a
supporting thread, whose inherited name did not establish its function. Existing
Linux action profiles already selected the main and vCPU threads, but used
`cpu-clock:u`. Observe whether a Linux production session reproduces that system
share and which resolved kernel/native paths account for it. This is not a
launch-speed benchmark, nor proof that translation dominates.

Only the runtime owner starts/controls a guest. Use the unchanged production
engine/frontend pair and normal cache geometry, with diagnostic counters off.
Review actual current worker and thread start ticks/maps. Select at most two
stable threads: the owner-identified main/supporting thread and vCPU. Label a
role unknown if not established; a name alone is insufficient. Inspect actual
Home before/after; run capture writers outside the recording and send no guest
input during it. Retain immutable base/state/source/binary and shutdown evidence.

The fixed preset uses `cpu-clock` (user plus kernel), 20 ms sampling period,
DWARF8192, no inheritance, no build-ID cache writes and mmap8. Sleep lasts 10s;
the wrapper sends INT after 15s and KILL after another 3s; collector polling has
an 18s deadline. Each output file is capped at 64 MiB. Existing identity-checked
collector-session retirement runs even after failure and is additional cleanup,
not part of the 18s recording deadline. Allocate at least 75s before the original
guest Stop edge, including shutdown margin; do not reset any guest clock. The
runtime owner must review this margin against its actual process/resource plan.
No guest, profiler, build or phone allocation is implied by these instructions.

## Exact command template

Run as the ordinary owner. Set these six variables from the new runtime's actual
identity/evidence; old worker/TID values are retired and must never be reused.
`profile_dir` must already exist privately outside Git, and its plan/output paths
must be fresh. `collector` names the reviewed, pinned source file.

```sh
: "${collector:?reviewed linux-cpu-profile.py path}"
: "${worker_pid:?actual live Native14 worker PID}"
: "${main_tid:?actual owner-identified main or supporting TID}"
: "${vcpu_tid:?actual CPU0/TCG TID}"
: "${engine_path:?exact executable-mapped production engine}"
: "${profile_dir:?fresh private parent outside Git}"
python3 -B "$collector" prepare --preset user-kernel-10s \
  --pid "$worker_pid" --thread "$main_tid:main-or-supporting-owner-verified" \
  --thread "$vcpu_tid:vcpu-owner-verified" --engine "$engine_path" \
  --plan "$profile_dir/plan.json" --output "$profile_dir/recording"
python3 -B "$collector" collect --plan "$profile_dir/plan.json"
```

The second command is preflight only and prints the exact argv without attaching.
The integrator reviews the plan, current resource admission and original Stop
margin, then writes `ROOT-GRANT.json` beside that plan, containing exactly the
reviewed `plan_sha256` and `tool_sha256`. After that lane allocation only:

```sh
: "${approved_plan_sha256:?integrator-reviewed exact plan SHA256}"
python3 -B "$collector" collect --plan "$profile_dir/plan.json" \
  --execute-approved-sha256 "$approved_plan_sha256"
```

After recording and guest shutdown, offline reporting is independent:

```sh
python3 -B "$collector" report --recording "$profile_dir/recording" \
  --output "$profile_dir/report"
```

Do not change paranoid/kptr/security settings if permissions or symbol resolution
fail. Retain the failure and stop; do not retry opportunistically.

## Review and decision

`before.json` / `terminal.json.after` retain actual `CLK_TCK`, worker/thread
utime/stime, schedstat and monotonic read brackets. For each unchanged TID, derive
user/system seconds from tick differences divided by CLK_TCK. Bound elapsed time
using the before/after read brackets; retain rounding/sampling uncertainty. Check
both selected thread deltas and process totals; unselected thread work may remain.

Offline `samples.txt` includes perf's `misc` CPU-mode field. Reports separate U/K
period totals per thread. K samples enter only kernel-resolved/kernel-unresolved
bins, never native QEMU/MMU/graphics categories. Missing/unknown modes fail parsing;
unresolved kernel symbols remain unresolved. Review raw lost events, actual event
header, permissions, callchains, snapshot deltas, thread lifetime and the original
terminal record independently; exit0 or classification_valid alone is no pass.
Kernel samples measure scheduled CPU, not off-CPU blocking or GPU elapsed time.

Choose an optimization only if a repeatable, attributable cost supports it. If
Linux does not reproduce Android system CPU, leave the Android cause open and
seek a target-specific observation. No invalidation weakening, larger cache,
QOM-build allocation, alternate backend promotion or speedup claim follows from
this collector change.

Source-only verification (no perf/sudo attachment):

```sh
python3 -B tests/portable-linux-cpu-profile.py
```
