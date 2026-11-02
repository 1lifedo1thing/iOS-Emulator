# Sparse cold/warm translation diagnostics

Existing QMP `x-query-jit` (or HMP `info jit`) returns the generated code size and
capacity, live TB count, TB flush/invalidation and TLB flush counters. These
describe the code buffer and physical lookup structures, separate from the
4096/16384-entry jump cache. System-mode code capacity defaults to the smaller
of host physical RAM/8 and1GiB; capacity is not resident RAM. Obtain actual
capacity from the query, rather than assuming it from Android available RAM.

An owner may collect three snapshots on a fresh matched worker: inspected Home
before the first Settings→General→Back cycle, Home after that cycle, and Home
after the repeated cycle. Take each query outside the tap/capture timing burst,
retain its request/response time bounds and raw response, and use identical
instrumentation in both arms. "First" means first observed in this session,
not that host/guest file or shader caches were cleared. The query traverses TB
and QHT metadata; its overhead is not zero. Do not poll it per frame or enable
per-TB translation/execution traces for the latency comparison.

`jit-snapshot-delta.py` compares retained snapshots **offline**. Transport and
live worker verification belong to the runtime owner. Its input envelope is:

```json
{
  "schema": 1,
  "label": "home-before-cold",
  "identity": {
    "host_boot_id": "owner-verified-host-boot",
    "pid": 123,
    "start_ticks": "456",
    "engine_sha256": "64-lowercase-hex-digits"
  },
  "begin_monotonic_ns": 100,
  "end_monotonic_ns": 110,
  "response": {"return": {"human-readable-text": "complete x-query-jit text"}}
}
```

```sh
python3 contrib/portable/jit-snapshot-delta.py before.json after.json > delta.json
python3 tests/portable-jit-snapshot.py
```

The tool requires one worker/engine epoch, ordered observation bounds and stable
capacity; it refuses missing/duplicate fields, QMP errors and decreasing
cumulative counters. Live TB/code-size deltas may decrease after flushing.
These are net retained state, not a cumulative count of successful translations
or time spent compiling. Statistics are not one atomic snapshot; invalidation
and flushing can hide generated work. A first-only increase supports new
translation activity but cannot establish the cause of a slower first action.
Correlate a separate sampled CPU profile with `tb_gen_code`/`tcg_gen_code`,
guest execution, MMU and MMIO handlers. File/library/texture initialization
needs its own evidence. Existing accounting's run/halt/io fields are wall-time
categories, and `io` is not disk I/O. No causal attribution or speedup follows
from these counter deltas alone.


This offline tool changes no engine or CPU backend. The larger jump-cache experiment remains on its separate experimental branch and is not included by this documentation change. Actual matched Linux and Android guest measurements remain required before claiming a performance improvement.
