# Experimental jump-cache clear accounting

This diagnostic does not change cache geometry, validity checks, MMU flushes,
chaining, or the default 4096-entry jump cache. It is absent unless the engine
is explicitly compiled with `QEMU_EXPERIMENTAL_TB_CACHE_STATS=1`. This is the
same experimental compilation gate used by the lookup-reason experiment;
this source revision alone adds clear accounting, not lookup-reason counters.
Combining the two patches requires a separately reviewed producer.

The explicit `build-engine.py --experimental-tb-cache-clear` selector enables
only this compile gate and requires an absent fresh Linux output. Android,
source-update, existing directories and dangling symlinks are refused before
output creation. The exact macro is recorded in the normal recipe argv;
no existing production cache may be reused with changed flags. Omit the flag
to retain production behavior. This experimental build needs its own actual
engine/configuration validation before any guest allocation.

The motivation is to distinguish clearing from collisions when a lookup sees
an empty cache slot but usually finds an existing translation in the slow
table. An empty slot does not itself prove invalidation: it can also be cold,
or reflect a previously invalidated translation. A larger table cannot avoid
full clears, and makes each full clear larger. No performance improvement or
dominant cause follows from this source patch.

## Clear paths

The reviewed baseline matches upstream `samhenrigold/qemu-ios` commit
`cb3595e8add3d9a3a55807186c6b8ff0ba338abe` for these operations.

| Counter | Actual operation counted |
| --- | --- |
| `full_total` | Non-NULL cache entering `tcg_flush_jmp_cache`; the original loop stores NULL to every slot. |
| `full_tlb` | Full-cache clear requested by `tlb_flush_by_mmuidx_async_work`. |
| `tlb_clean`, `tlb_partial`, `tlb_all` | Requested MMU flush found zero, some, or all MMU-index bits dirty. Even the zero-dirty case still performs the original full jump-cache clear. These counters classify that call; they do not count ARM instructions. |
| `full_range` | Range invalidation exceeded the existing length threshold and selected a full clear. |
| `full_tb` | Full translation-cache flush, per visited CPU. |
| `full_pcrel` | Per-TB invalidation of a PC-relative TB, which already clears every CPU's full jump cache. |
| `full_reset` | Reset's explicit full clear. Its subsequent TLB flush can produce another full clear. |
| `page_total` | Non-NULL cache entering the original hashed-page-group clear loop. |
| `page_previous` | Page/range invalidation's preceding group, needed for a TB that overlaps a page boundary. |
| `page_current` | Single-page invalidation's current group. |
| `page_range` | Each current group in a smaller range invalidation. |
| `tb_check`, `tb_match` | Non-PC-relative invalidation's per-CPU pointer check, and an actual pointer match selecting the original NULL store. |

The full clear paths are in `translate-all.c`, `cputlb.c`, `tb-maint.c`, and
`tcg-accel-ops.c`. Single-page and smaller-range paths clear both current and
previous groups. Their hash grouping is unchanged. A 4096-entry cache has
64-slot page groups; a 16384-entry experiment has 128-slot groups. Thus larger
geometry increases both full and hashed-group clear work; counts and a matched
measurement would be needed to establish whether that matters. Changing the
geometry also changes the page hash's bit folding, so a larger table is not
simply a superset of the smaller table's collision behavior.

ARM DACR, FCSE, context-ID and translation-table changes can legitimately
request TLB invalidation. These counters do not distinguish those instruction
sites or justify weakening invalidation, removing context comparisons, or
changing address-space tags. The existing per-TB pointer check/store is also
unchanged; a conservative extra clear during a race remains possible.

## Ownership and output

`CPUJumpCache` is zero-allocated during CPU realization and retired through
the existing RCU path. Counters share that lifetime and survive cache clears,
TB flushes, and guest resets. A new CPU allocation starts a new counter epoch.
All increments and reads use QEMU atomics because physical invalidation can
visit another CPU's cache. Existing CPU-list, RCU, owner-thread and exclusive
execution requirements continue to govern the original operations. There is
no new lock or wait.

The experimental fields enlarge the cache allocation and move its entry array;
atomic increments add work. Treat this as a cause-counting diagnostic, not an
uninstrumented timing candidate. No comparison to production timing can assume
that instrumentation or layout changes cost zero.

In an experimental build counters increment regardless of `IT_VCPU_ACCT`.
That variable is initialized on the CPU thread and is deliberately not read
by foreign invalidators. Enabling existing `IT_VCPU_ACCT` output also emits
one `[tb-clear]` line at each existing owner-thread accounting dump, outside
the per-TB paths. The `cpu` field identifies the owner; `entries` states cache
geometry. Elapsed seconds use the existing accounting epoch, not a calibrated
host action clock. A dump is delayed if that loop does not run.

Each field is an independent atomic observation, not a coherent transaction.
An invalidator can run between field reads, so site subtotals and total may
differ at a publication boundary. These are cumulative **operation counts**,
not unique emptied slots, valid TBs evicted, TLB entries invalidated, CPU time,
or a direct count of lookup misses caused. Whole and group clear loops count
even when all affected slots were already empty. Match before/after records
by CPU, geometry and cache lifetime; retain actual complete-line byte offsets
and publication-time bounds. Do not assign a one-second record to an exact
input event without an observed clock mapping.

## Focused source test

Preparation invokes no compiler:

```sh
python3 tests/portable-tb-cache-clear.py --prepare-only --output /private/new-clear-slice
```

After a separate compiler allocation, the same command without
`--prepare-only` builds and runs default-OFF and experimental-ON variants at
O2/Werror and with ASan/UBSan, followed by two expected-failure controls. Use
a fresh output directory and an external aggregate time/RSS supervisor;
every individual child has a 30-second timeout. No engine or build cache is
used. The test extracts the actual production functions and uses actual
QEMU atomic helpers and cache declaration. Surrounding MMU/TLB work, CPU list,
RCU type, and lock calls are explicit fixture stubs. This is not full MMU,
RCU-lifetime, engine, Android, or guest validation.

Assertions cover previous/current page preservation, range threshold,
clean/partial/all dirty MMU-index classification, pointer match versus a
different TB at the same PC, PC-relative/all-CPU clears, full-TB flush, reset's
two clears, counter persistence, concurrent remote increments, and one-line
dump shape. Default-OFF checks the fixture's legacy cache layout and that
counter macro arguments are not evaluated. Negative controls remove only the
new total increment or the original preceding-page clear and must fail the
corresponding counter or cache-validity assertion. No result is claimed until
those native checks actually run.

## Publication witnesses and offline extraction

`tb-clear-delta.py` implements the exact 15-field, single-line schema above.
Its `observe(fd, identity, mode, guard, ...)` library function reads an already
opened regular log file; it does not open processes, send input, write logs or
control an engine. The caller owns a bounded `guard()` that returns a freshly
validated identity: `host_boot_id`, `pid`, `start_ticks` (decimal string),
`engine_sha256`, `cpu`, and `cpu_epoch`. The last field is a caller-certified
CPU-allocation lifetime, not a guest boot ID inferred from the line. A fresh
worker and no CPU recreation during the observation are suitable evidence;
CPU number alone is insufficient. These attestations remain a limitation of
offline verification. Only 4096-entry clear-only logs are accepted.

BEFORE selects the latest complete record, ignoring a currently partial tail.
AFTER first records EOF after the inspected full target source, then waits at
most five seconds of polling for the first complete record whose **start** is
at or after that EOF. Completing a line already partially present is not a new
record. Both calls retain raw line/hash/offset, log device/inode, bounded tail
hash/EOF and HOST_MONOTONIC read brackets. Rewrites, truncation, malformed
complete records and owner/epoch changes are refused. Polling bounds cannot
bound an external guard or a blocking filesystem syscall; the caller retains
its original outer watchdog. Log reads are capped at 16 MiB and tail reads at
1 MiB. File append is the only expected live modification.

After strict retirement, the caller creates a schema-1 selection containing
`identity`, `label`, final `log: {path, sha256}`, returned `before` and `after`
witnesses, and `action` with `down_begin_monotonic_ns`,
`down_end_monotonic_ns`, `last_transition_source_ns`, and
`first_complete_source_ns`. The last two are actual source-frame timestamps
from independently inspected transition/full-target images. The AFTER call
also takes `first_complete_source_ns`. The caller retains the strict terminal,
frame evidence and CPU-lifetime certification separately; the extractor does
not independently attest retirement or image content.

```sh
python3 contrib/portable/tb-clear-delta.py /private/final-selection.json
python3 tests/portable-tb-clear-delta.py
```

The command writes JSON to stdout only. It validates final log hash, exact
retained line/tail bytes, EOF eligibility, latest/first selection, owner/epoch,
unsigned ranges, fieldwise monotonicity and input/source/publication ordering.
An incomplete final counter line is refused. It reports operation deltas and
rounded accounting-clock separation, never converts them to CPU time or
cache-miss attribution. Site totals need not equal the central total because
their atomic observations are independent. Host tests cover real append and
partial-line publication as well as refusal paths; they do not execute a guest.

A launcher adapter must keep ownership checks separate from record parsing.
The private Runtime-style adapter uses a retained pidfd plus PID start ticks,
host boot and UID, exact worker argv/parent and selected engine/base/state
paths. It verifies the selected immutable engine's hash once at admission,
then checks its metadata and mapped device/inode on each guard, together with
the log's owned regular-file identity and the worker's stderr FD. This avoids
rehashing a large engine on every polling iteration. A symlink/replaced log,
retired worker, changed engine metadata or missing mapping must be refused.
The guard returns the complete identity before and after each read, including
the launcher's explicit fresh-worker/no-CPU-recreation certificate. Checking
procfs does not itself establish that certificate or inspect guest scenes.
The adapter must also pin this parser's hash, preserve exclusive witness-file
creation and retain failures. Its plan-only mode must not open a live owner.
Tests with an owned inert Python child and mapped data file validate these
host boundaries; such a fixture provides no engine or guest qualification.
