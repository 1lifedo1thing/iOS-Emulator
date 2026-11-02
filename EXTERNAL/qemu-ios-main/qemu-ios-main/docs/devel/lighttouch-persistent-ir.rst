Experimental LightTouch persistent frontend IR
============================================

This candidate is **off by default and not qualified for a product release**.
It is not persistent native code, AOT compilation, or a replacement CPU backend.
There is no measured performance claim. Actual Linux and Android guest correctness,
cross-process hits, first-action measurements and final packaging remain required.

The ARM translator can save pointer-free preoptimization TCG operations. A fresh
worker restores temporary/label identities, binds exits to its own newly allocated
TranslationBlock, then runs QEMU's normal optimizer, liveness passes, register
allocator and host emitter. Existing page locking, generated-code publication,
lookup, interrupt checks, SMC invalidation and TB retirement remain in place.
Neither a native-code buffer nor a helper, CPU, label or TB address goes on disk.

Current supported subset
------------------------

* Linux hosts (including Android), 64-bit TCG hosts, system mode without plugins.
* Exact ARM1176 CPU, little-endian A32, single page, ordinary integer data
  processing, byte/word LDR/STR and direct branches, and a closed helper-free IR
  opcode set. CPU/guest state remains dynamic through existing TCG globals.
* Unsupported blocks, including Thumb, VFP/SIMD, helper calls, privileged
  instructions, dynamic lookup jumps, stepping, breakpoints, MMIO, plugin and
  parallel translation contexts, use normal translation. They are not errors.
* Cache hits skip ARM frontend decoding only. Optimization and native emission
  are still performed. The supported subset and extra hashing/storage costs can
  limit gains; measure real guest workloads before enabling by default.

System ARM TCG's normal ``CF_PCREL`` optimization remains enabled. Version 2
records explicitly bind that mode and preserve the translator's page-offset
instruction markers, dynamic PC updates and branch ordering. Capture bounds use
the actual translator PC, because ``tb->pc`` is unspecified in this mode. Replay
does not change ``arm_restore_state_to_opc`` or generated-code publication.
The cache key still includes the exact translation PC and full flags; it does
not broaden disk reuse to virtual aliases or change normal physical TB lookup.

Identity and failure behavior
-----------------------------

The key binds the full remaining locked code page, PC, all TB context/compilation
flags, instruction limit, CPU ISA identification/features/idle-loop setting,
current global-temp layout, host optional-op support and SHA256 of the actual
mapped engine file. The opened engine inode/device must match its executable
mapping in ``/proc/self/maps``. Its size and timestamps must remain unchanged
while hashing. For native libraries mapped directly from an Android APK, it hashes
the exact mapped backing APK and library member name; this conservatively
invalidates on any APK content change and avoids extraction. An inaccessible or
non-file image disables caching. A key is checked again before publishing a miss.

Records use explicit little-endian integers and symbolic temp/label/exit indices.
The decoder checks all lengths, counts, enums, operand indices/types, instruction
PCs/count, host CPU-state memory bounds, supported memory operation flags, label
definitions, exit slots and host opcode availability before changing live TCG
state. The file envelope checks build/key/SHA256 before decoding. Files must
never be accepted from an archive, guest, network, user import or a shared cache:
the owner-only store is trusted local process output, not an untrusted IR format.
Processes with the same UID can already modify the engine and guest state; the
cache is not a security boundary against them.

An existing absolute directory must be owner-only (0700 recommended). Records
and the lifetime exclusive lock are owner-only regular files, with no hard links;
final symlinks are not followed. There are at most 4096 records, 32 MiB total and
128 KiB per payload, including accounting for stale build records. Initialization
loads only bounded valid records. A second owner, malformed/oversized/unsafe store
or unknown leftover file disables caching; normal translation continues.
Each record is written to a new private file, flushed, closed and atomically
renamed; partial files never become hits. Unknown files are never removed. After
a crash leaves a temporary file, normal translation remains available; explicitly
replace the disposable private cache directory when the worker is stopped.
No guest overlay/base/dirty-state or saved-state files are modified by this cache.

Experimental invocation and frontend contract
--------------------------------------------

Root must allocate/review an actual engine build and guest run before using::

    mkdir -m 700 /absolute/private/profile/cache/tcg-ir-candidate
    LT_TCG_IR_CACHE_ENABLE=experimental-v1 \
      LT_TCG_IR_CACHE=/absolute/private/profile/cache/tcg-ir-candidate \
      <existing unchanged frontend invocation>

The shared frontend contract uses a separately owned profile cache directory,
inherited only by a fresh native worker through those two environment variables.
Controlled default-off hooks now exist in the canonical App source: the Linux
helper and ordinary launcher clear inherited opt-ins, and Android exposes
debug-only ``IEngine.debugSelectTcgIrCache`` admission on a fresh worker (see the
App repository's ``Android/EXPERIMENTAL_TCG_CACHE.md``). Release/UI enablement
remains disabled. No qualified persistent cache is shipped: engine/API packaging,
actual guest reuse and performance benefit still need their own acceptance.
Do not infer guest-clean shutdown or a performance pass from cache statistics.
Cache records are disposable, never bundled with firmware or shared publicly.
The worker emits image identity plus bounded counter milestones into its existing
native stderr log and final counters on ordinary process exit. Milestones remain
available even when Android kills the terminal worker without running atexit.

Opt-in source diagnostics
-------------------------

With an actual experimental store already enabled, exactly
``LT_TCG_IR_CACHE_DIAGNOSTICS=aggregate-v1`` adds fixed aggregate counters and
monotonic wall microseconds at the existing report sites. Diagnostics alone does
not enable caching. The legacy counter line, keys, format, limits, durability and
fallback behavior stay unchanged. The additional line distinguishes ARM/Thumb
translation attempts, key eligibility, capture/validation/store rejection, cache
stage wall spans, first observed hard capacity and actual file/directory fsync
counts, errors and wall durations. Timing is neither guest CPU utilization nor a
complete translation CPU profile; ordinary translator, optimizer and emitter
costs remain outside these spans. ``put_us`` includes its fsync timings, so the
spans must not be summed as independent CPU shares. Measurement overhead remains
unqualified. See ``contrib/portable/ir-cache-feasibility.md`` for exact fields,
denominators, limitations and a separate future durability-policy experiment.

Source tests without an engine build
------------------------------------

Use an existing configured native Linux ARM system-mode build's compilation
database (the command does not invoke configure/Ninja, a guest or a device)::

    python3 tests/unit/run-tcg-ir-cache-tests.py \
      --compile-commands /existing/qemu-build/compile_commands.json \
      --out /new/private/evidence/directory --sanitize

This checks real configured-header syntax, isolated store/codec tests, and actual
separate producer/consumer execs for stored bytes. The codec's small independent
integer fixture exercises relocated temps/labels/current-TB exits and interrupt
branches. It is not actual QEMU-generated code execution or iOS qualification.
The store unit also participates in the normal Meson unit suite.

Real generated-code differential probe
--------------------------------------

``tests/portable-ir-cache-engine.py`` can exercise an existing pinned standalone
``qemu-system-arm`` build after the owner allocates a runtime slot. It uses an
asset-free ARM1176 ``none`` machine with 16 MiB RAM, handwritten integer/load/store
code, and QMP run/stop plus GDB observation. It never single-steps or sets code
breakpoints, because those deliberately disable cache eligibility. Separate
processes cover cache-off, cold storage, fresh-process hits, a changed CPU mode,
changed guest instruction bytes and ordinary memory-write invalidation within a
process, with actual registers and RAM checked. The arithmetic/load-store block
starts at a nonzero page offset and branches within that same page. Both that
block and the target loop must produce fresh-process hits; loop-only reuse does
not qualify the differential.
Default invocation is plan-only; execution needs ``--execute``. Controller
contract checks are ``python3 tests/portable-ir-cache-engine-test.py``.

This is an additional real generated-code correctness step. Bounded normal
execution polls for the target loop, allowing first-use engine hashing before
the first instruction runs. Observations include startup, sleep and debug
transport and cannot rank performance.
It does not boot iOS or qualify a frontend, Android, real workload coverage,
audio/input, persistence or a practical reduction in first-action latency.

Retained iOS session comparison
------------------------------

``contrib/portable/ir-cache-session-report.py`` compares retained actual iOS
session manifests only. It checks matching engine/frontend/base/seed-state/
workload/host identities, separate native processes, strict clean shutdown and
resource witnesses, retained image/log hashes and cache cold-producer/reuse
inventory binding. It requires at least two runs per variant and reports input/
source-frame latency brackets plus worker-process CPU time. The latter includes
host emulation/render/audio/runtime work, not guest iOS CPU utilization.

The output is explicitly owner-evidence comparison: offline byte checks do not
independently verify live process witnesses or visual scene judgments. Fixed
execution-window timings from the asset-free differential cannot be substituted
for actual app/menu actions. Check the reporter with
``python3 tests/portable-ir-cache-session-report.py``; synthetic contract tests
never qualify an actual performance gain.
