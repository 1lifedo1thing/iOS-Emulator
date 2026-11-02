# Persistent frontend IR cache: feasibility measurements

`ir-cache-feasibility.py` reads bounded retained native logs, their cache receipt
and its runtime-result hash binding. It never starts a guest, reopens a writable
store, repairs state or qualifies a run. Failed/dirty evidence remains labelled.
Use it alongside the independent owner's scene/shutdown/retirement reviews:

```
python3 contrib/portable/ir-cache-feasibility.py \
  --native-log /private/trial/exploration01/stderr.log \
  --cache-receipt /private/trial/exploration01/cache-trial.json \
  --runtime-result /private/trial/exploration01/RESULT.json
```

Existing hits+misses+rejected count key-eligible candidate lookups after the
store was enabled. Rejected means a record was found but could not be replayed;
it does not count all unsupported instructions/capture failures. Neither this
denominator nor stored/misses gives the fraction of all translated ARM TBs that
are A32 eligible. Legacy logs do not record Thumb, unsupported CPU/context or capture-rejection
counters. Translation attempts also differ from executed
instructions: repeated execution of an existing native TB does not enter the
persistent-cache lookup path. The report leaves unavailable quantities null.

The format limits are4096records/32MiB. The first recorded sample at the record
limit gives lookup/store yield at that sample, not its elapsed time. Final
inventory bytes distinguish count saturation from byte saturation. Every
successful new publication calls file fsync and directory fsync on the caller's
vCPU path;2*stored is a lower bound on fsync invocations including possible extra
failed attempts. Legacy logs do not record directory-fsync success. Durations cannot be
inferred from call counts or from untimestamped native log milestones.

The supported subset is deliberately narrow: ARM1176 little-endian A32 integer
instructions, one page and a closed symbolic IR set. Thumb, helper calls,
privileged/coproc/VFP/SIMD instructions and other guest CPUs are excluded. A hit
still hashes context/source bytes, validates/decodes/replays the retained IR,
optimizes it and emits fresh native code. No native machine code is persisted.

Keep the current engine frozen for actual matched OFF/COLD/REUSE correctness,
strict shutdown and fresh-worker hit coverage first. Do not change store limits,
durability semantics or enablement while comparing those arms. A feasible
speedup needs frontend decoding to consume a material fraction of workload CPU
and saved decoding to exceed lookup/replay/record-store overhead. GPU/execution,
IR optimization/emission, I/O and repeated already-translated execution remain.
No observed candidate hit ratio alone establishes those conditions.

If actual replay is useful, a later separately pinned diagnostic engine should
collect fixed aggregate counters only with explicit opt-in: total translated ARM
TB attempts, Thumb/other-context rejection, key-eligible attempts, unsupported
capture/IR rejection, hit/miss, capacity skips and first-capacity elapsed time.
Aggregate monotonic wall/thread-CPU spans should separately cover frontend
translation, key hashing, replay, optimization/emission and cache store, including
file/directory fsync counts and wall time. Preserve original final cache telemetry
and avoid per-TB logging. Include measurement overhead and emitted-sample bounds
in tests. Such an engine invalidates existing cache keys and therefore needs its
own COLD producer and matching REUSE. The source diagnostic subset described below is now implemented behind a second
explicit opt-in. It is not enabled in earlier engine artifacts or automatically
parsed by this offline report. Frontend translation, optimizer and native emitter
CPU timing remain unimplemented and unmeasured.

Tests are synthetic offline contracts:

```
python3 tests/portable-ir-cache-feasibility.py -v
python3 tests/portable-ir-cache-session-report.py -v
```

Default cache remains OFF and unqualified. The offline report makes no Android
qualification, actual iOS reuse or performance claim.


## Opt-in source diagnostics

With the experimental cache already enabled, exactly
`LT_TCG_IR_CACHE_DIAGNOSTICS=aggregate-v1` adds fixed aggregate counters and
monotonic wall microseconds. Diagnostics alone never enables the cache. Other
values do nothing. The default cache and diagnostics remain OFF. This is source
instrumentation, not a qualified diagnostic engine or a measured guest speedup.
The engine image digest changes after rebuilding, invalidating older cache keys.

The original cache summary line is unchanged. A separate version=1 diagnostic
line is emitted only at existing power-of-two hit/store milestones and terminal
finalization. There is no per-TB output or unbounded event collection. Fields are:

- `arm_attempts`, `a32_attempts`, `thumb_attempts`, `a64_attempts`: calls into
  the ARM translation cache hook after an actual store was enabled. The latter
  three partition the total. These are translations, not dynamic instructions.
- `key_eligible`, `key_rejected`: the initial key check partitions the total;
  eligible does not mean the decoded instruction/IR subset is capturable.
- `save_preflight_skips`: the cheap store preflight refused a save. This can
  mean an existing key or capacity; it is not a capacity-only denominator.
- `save_key_changed`, `capture_rejected`, `validation_rejected`, `store_failed`:
  later save-stage rejections. Legacy `rejected` instead counts found records
  that fail validation/replay admission.
- `key_us` combines initial and save-time key checks; `lookup_us`,
  `load_decode_us`, `replay_us`, `capture_us`, `encode_us`, `save_validate_us`
  and `put_us` cover the named cache stages. Source-instruction validation on
  load is outside `load_decode_us`; there is no timing of the ordinary ARM
  translator, optimizer or native emission here.
- `records` and `bytes` describe the current store including preexisting
  records. `elapsed_us` starts at diagnostic enable; `capacity_observed` and
  `first_capacity_us` record the first observed hard count/byte threshold.
  Enabling on an already-full store records capacity immediately. The first
  observation never resets and is not an absolute monotonic timestamp.
- `put_calls` includes rejected attempts, `put_success` counts publications.
  `file_fsync_calls/errors/us` and `directory_fsync_calls/errors/us` measure
  actual sync invocations and wall duration, including failed calls. Existing
  file-sync failure and ignored directory-sync failure behavior is unchanged.

These wall spans overlap: `put_us` includes both sync spans. Do not sum them as
independent CPU shares or call them guest CPU utilization. Scheduling, filesystem
latency, diagnostic clocks/locks and log output contribute overhead. Sub-
microsecond calls can report zero. The instrumentation's performance cost is
unqualified. Missing diagnostics in retained logs remain unmeasured; never infer
zero. Ordinary unit store tests use real fsync; the separate diagnostic harness
intercepts clock/fsync for deterministic accounting and failure semantics only.

## Embedded terminal reporting

The ARM embedded entry calls `arm_tcg_ir_cache_finish()` after `qemu_cleanup()`
has stopped vCPU and device activity, before returning to a host that may kill
its worker without running `atexit`. It emits the existing final summary and,
if requested, aggregate costs, then a terminal marker with
`reason=post-qemu-cleanup`, flushes stderr and releases only the cache lease.
It does not trigger shutdown, change shutdown deadlines, clear dirty guest
state or establish guest halt confirmation. All existing shutdown receipts
remain required. It does not flush or otherwise access guest writable state.

Finalization takes the existing cache lock before the diagnostic lock. The
caller must have stopped all translation; the lock alone does not make it safe
to finalize during active key generation. Repeated calls and the later
`atexit` fallback are silent after the store is closed. Cache-disabled and
unsupported builds remain silent and do not initialize a store. The process
cannot start a second VM after finalization.

Ordinary process exit retains a distinct `reason=process-exit` fallback. That
marker does not prove cleanup or clean guest shutdown. An Android timeout or
process death before native cleanup returns may retain milestones only; the
last milestone is not complete final totals. `_exit` unit checks establish log
retention without stdio/atexit cleanup, final counter accuracy, single reporting
and cache lease release; they do not establish an Android/iOS target pass.

## Possible future durability experiment

Per-record file and directory fsync are the existing policy and remain unchanged
in this diagnostic revision. A separate, explicitly experimental policy could
batch or delay sync for this disposable, checksummed cache if measurements show
significant cost. That policy must retain private exclusive ownership, complete
temporary-file writes before atomic rename, bounded startup validation, checksum
and engine/key rejection, and normal-translation fallback. It must never flush,
share or derive authority from the guest's writable state.

Without per-record sync, a crash may lose newly cached records or leave a partial
or stale directory entry. Current bounded reopen validation can reject the whole
cache, and normal translation must remain available. A later policy needs fresh
cross-process correctness and crash/fault-injection evidence for interrupted
write/rename, corrupt/truncated records, orphan temporary files and failures at
any batch/final flush. End-of-session flush alone cannot promise durability when
Android kills a worker without atexit. It could still provide within-process and
best-effort cross-session reuse; claims must match observed results. Keep limits,
workload, shutdown, engine and cache policy pinned for each comparison. No such
policy is implemented or approved by these source diagnostics.
