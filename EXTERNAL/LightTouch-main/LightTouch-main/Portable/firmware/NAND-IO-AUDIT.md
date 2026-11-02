# n72 NAND I/O profiling handoff

Read-only source audit of QEMU producer
`b59b3620b01da9285dea3a9ee6f58c1bb3cecfc2` and the compiled Android21b
native producer `d939543dc9084b455d8fe0b3d44dccbe024acd87`. This identifies
measurement opportunities, not an established performance bottleneck or a patch.
No target collection, asset mutation, cache flush or compiler is implied.

## Actual path

`hw/arm/ipod_touch_2g.c` connects n72 to `ipodtouch.fmss` and supplies the
read-only NAND base and writable overlay. The 1G `s5l8900_fmc` and n81 IOP/block
backend are different routes and must not be used to explain baseline behavior.
Current shared-domain `Storage.cpp` composes `nand=<base>/nand` and
`nandrw=<state>/nand`; base validation requires this NAND output to be a directory.

`hw/arm/ipod_touch_fmss.c` selects these paths, synchronously in the guest's
controller/MMIO work:

| Path | Existing work/cache | Host file work |
| --- | --- | --- |
| Page programmed in this session | `phys_pages` GTree recall at the physical address | Copies stored data/spare; no file open |
| Overlay lookup | Direct-key hash table, populated once by directory enumeration and updated on writes/erases | Indexed misses avoid failed overlay opens; hits open the page |
| Erased-block lookup | GTree caches known erased blocks | When startup `FMSS_ERASE` is enabled, uncached marker misses use `g_file_test`; unset flag bypasses this |
| Directory-backed base hit | No general base-page or open-FD cache | `fopen`, `fread`4096, `fread`64, `fclose` each read |
| Packed `ITNAND01` base | One checked mapping at realize, indexed record lookup | Page data/spare copied from mapping; no per-page base open |

The directory read has no explicit per-page SHA256 or `stat` call. libc may add
internal calls; source does not establish the exact open/read/stat syscall count.
The packed path's initial `stat`, `open`, `mmap`, `close` are startup work.
It is not the `ITPACK01` prepared-base import container. Current domain directory
validation does not permit substituting a packed file in a running record.
Any future packed/cached representation needs intact identity, full integrity,
path/mode/lease, hole/spare and overlay precedence contracts; no conversion or
validation bypass is proposed here.

Base geometry is cached after its initial GPT-page probe. Environment flags are
already cached; repeatedly replacing `getenv` is not a new optimization. Optional
`FMSS_BASESPARE` diagnostic mode adds another base open/spare read on overlay
reads. Inspect only named relevant flags if needed, not the entire environment.
No baseline setter for those diagnostic flags was found in the selected entry/
board source; do not assume the actual worker environment without evidence.

Writes use a temporary overlay page,4096+64-byte checked writes, `fflush`,
per-page `fsync`, close and rename; erase-marker staging also syncs before rename.
Failures set the session's storage-failure flag. Opening an app can involve guest
preferences/FTL writes as well as reads; changing these syncs without durability
evidence would violate the acceptance objective.

Shared `Storage.cpp` enumerates/stats assets and opens/hashes all required base
bytes at import and `open_device_checked` admission. `Library.cpp` includes pinned
ROM/package bytes in that admission. JNI `start_runtime` calls it before runtime
creation; guest app launches and touch/button APIs do not reopen the device or
repeat full base hashing. The Android device dropdown reads small record metadata
and capabilities; it does not hash NAND per menu operation.

## Existing trace/counter evidence

Root may query/toggle this one existing event through its already admitted debug
QMP channel; the source-only lane must not operate that channel:

```json
{"execute":"trace-event-get-state","arguments":{"name":"ipod_touch_fmss_stats_log"},"id":"stats-before"}
{"execute":"trace-event-set-state","arguments":{"name":"ipod_touch_fmss_stats_log","enable":true},"id":"stats-enable"}
{"execute":"trace-event-set-state","arguments":{"name":"ipod_touch_fmss_stats_log","enable":false},"id":"stats-disable"}
```

Confirm query/enable/disable responses on the actual compiled engine. Restore the
previous event state in a finally path and retain command ordering and log bounds.
Stats add two host-clock reads per page and one formatted log every256 traced
reads. Keep a short bounded interval; do not enable every FMSS trace or dump pages.

Reports contain cumulative `reads`, `base`, `ovl`, `recall`, `blank`, `shadowed`
and `read_ms`. **Classification counters advance even with stats disabled;
`reads`/`ns` advance only while enabled.** Use deltas between two reports inside
one continuous enabled interval, never an absolute base/reads ratio. First/last
partial256-read intervals are not independently reported. Fewer than two reports
cannot yield a complete interval delta; no report does not mean zero NAND I/O.

`read_ms` covers page-load and generated-free-pool work, including host waiting/
preemption. It excludes later guest DMA/completion/CPU work and does not time page
writes. Its `QEMU_CLOCK_HOST` implementation uses host `gettimeofday` here, not
CPU time or a monotonic clock; account for any wall-clock correction. Per-page
`read_log` is incomplete for base reads: it reports physical recall, overlay hits
and some relocation diagnostics, so it is not a complete read/syscall counter.

## Minimal no-root measurement Root can collect

Use the same admitted worker, record and displayed app, with no restart, base
copy, cache flush or mutation by the collector. Pin PID/UID/process start token,
actual APK/engine and guest state; use Root's current PID rather than a stale
example. The following are read-only command templates, not executed here:

```sh
adb -s DEVICE_SERIAL shell -T 'run-as org.lighttouch.android cat /proc/WORKER_PID/stat'
adb -s DEVICE_SERIAL shell -T 'run-as org.lighttouch.android cat /proc/WORKER_PID/io'
adb -s DEVICE_SERIAL shell -T 'run-as org.lighttouch.android cat /proc/WORKER_PID/task/TCG_TID/io'
```

Capture the actual remote status and error bytes, not just a local ADB zero.
Select TCG_TID from the observed thread list, check its start token/identity, and
retain before/after values. Permissions may deny these files; record that failure
instead of substituting zeros. Product owns the CPU-profiler collector, so reuse
its clock/identity/raw-command framework rather than introducing another sampler.

Compare one short idle-at-Home window with one known app-launch window, then a
repeat of that same launch only if Root allocates it. Retain the actual guest
scene/action timestamps and the existing FMSS stats interval with Product's CPU/
frame observations. Do not derive app completion from command admission alone.

- `rchar`/`syscr` deltas count returned read bytes/read syscalls; they are not counts
  of opens, stats, unique NAND pages or physical-storage reads. Process counters
  include other work; the observed TCG thread helps narrow synchronous page I/O.
- `read_bytes` records storage-layer reads, not bytes returned from the host page
  cache. A low value with substantial rchar/syscr can still accompany file-open
  and cached-read overhead. `wchar`/`syscw`/`write_bytes` also include logs and other
  writes; trace itself perturbs these counters.
- Combine these with FMSS read-time/classification deltas and CPU/profile samples.
  A transient `/proc/PID/fd` listing cannot count repeated short-lived opens.
  strace/simpleperf/root/kernel tracing availability is not presumed.

If NAND read time is small compared with the launch interval, this path is not a
supported primary explanation. If large, first separate directory base reads,
overlay reads, host waits and synchronous writes before considering a bounded
cache or verified representation change. No causal conclusion, optimizer patch,
gyro work or additional preparation-board coverage follows from this source audit.
