# Controlled Android native TLS experiment

This is a source plan, not a measured speedup or a qualified engine. Preserve
producer `259e4ad31702f4927ed8d06a06b64219121bc0a0` and its frozen engines. The
default remains API 28; the Android frontend already requires API 29.

An older actual Poco task-clock profile contained `__emutls_get_address` and
`pthread_getspecific` samples on the TCG thread (four samples each out of 228).
These sparse samples support an experiment, not a 3.5% speedup prediction.
The current engine uses the NDK Android28 compiler target. Android supports
native ELF TLS from API 29, and NDK r26+ automatically selects it at that floor:
[Bionic linker notes](https://android.googlesource.com/platform/bionic/+/HEAD/android-changes-for-ndk-developers.md#elf-tls-available-for-api-level-29).

## Inputs and isolation

After a compiler allocation, use one reviewed source revision, the pinned NDK
28.2.13676358, unchanged dependency prefixes, and distinct empty output paths.
Build an API28 control and API29 candidate from that same source. Retain the
existing frozen APK as historical context; a different-source APK is not a
controlled TLS comparison. No guest CPU, device, audio timing, graphics provider,
public C ABI, macOS pin or application minimum API changes are intended.

```sh
# PREFIX and CRYPTO_PREFIX are exact verified target prefixes for this ABI.
# Fresh output paths; never reuse a frozen producer's cache for changed flags.
python3 contrib/portable/build-engine.py "$FRESH_CONTROL" \
  --abi arm64-v8a --android-api 28 --ndk "$ANDROID_NDK_ROOT" \
  --prefix "$PREFIX" --crypto-prefix "$CRYPTO_PREFIX" --configure-only
python3 contrib/portable/build-engine.py "$FRESH_CANDIDATE" \
  --abi arm64-v8a --android-api 29 --ndk "$ANDROID_NDK_ROOT" \
  --prefix "$PREFIX" --crypto-prefix "$CRYPTO_PREFIX" --configure-only
# Compiler owner then builds/relinks at the allocated job limit and seals
# exact manifests, source kits, notices and APK strip/input correspondence.
```

The revised recipe hash also invalidates older recipe caches, even for API28;
this controlled experiment deliberately uses fresh control/candidate outputs.
The compiler argv is part of the existing cache identity. API28→29 changes C
and C++ compiler targets and must reject reuse even with `--source-update`.
Do not force initial-exec TLS or use private Bionic TLS slots. Normal shared
library TLS must work when the engine is loaded from a foreign host thread.
Static dependencies built for API28 can retain emulated TLS; record this mixed
closure honestly and measure residual symbols instead of claiming their removal.

## Build and correctness gates

1. Audit every compile command, generated shared-entry/control compile and final
   link: selected Android target API, matching ABI, PIC, unchanged optimization
   flags and no explicit emulated-TLS override. Inspect all selected linked
   objects/archives for native TLS relocations, residual emutls symbols and
   declarations spanning module boundaries. Record unresolved TLS/runtime imports
   and NDK runtime correspondence. No forced TLS model or skipped reader cleanup.
2. Run the actual RCU foreign-loader/main-reader lifecycle regression with normal
   registration/cleanup, forced grace periods, loader exit and repeated host
   thread lifetime. Run real coroutine tests for yield/reentry/current-thread
   identity and TLS access on both host ABIs. Native TLS does not fix ownership
   bugs by itself. Preserve the existing shared-main scoped registration rules.
   The existing RCU runner is a native-host harness; Android requires a reviewed
   test-only JNI harness under the ordinary app UID, not an app-data executable.
3. Verify exports/API, SONAME, Bionic closure, 16KiB LOAD alignment and actual
   minimum-API ELF notes. Exercise both ordinary dlopen and JNI DT_NEEDED loading
   from short-lived host threads without accessing dead TLS. Repeat real ARM
   TCG, CP15 guest-service and input/lifecycle regressions appropriate to the
   whole-engine compiler change, then normal app-UID guest tests on 4/16KiB.
4. Freeze a reviewed fresh worker for each comparison. Same fixture/state,
   provider, guest scene, PCM/input workload, app source/flags, diagnostic flags
   and thermal/power conditions; no monitor while measuring baseline emulator
   cost. Visible guest behavior and clean Stop/retirement remain required.

## Bounded profiling and comparison

Root alone selects and verifies the live worker PID, startticks, UID, exact APK
and its owned TIDs. Keep input/capture operations outside collection unless they
are the declared workload. A proven platform simpleperf configuration is:

```sh
# PROFILE_REL is a newly created, private, app-owned output directory.
# OWNED_TIDS is the verified current worker task list, not the old example PID.
adb -s "$OWNED_SERIAL" shell run-as org.lighttouch.android \
  /system/bin/simpleperf record -t "$OWNED_TIDS" --no-inherit \
  -c 50000000 -e task-clock --duration 20 --clockid monotonic \
  --call-graph dwarf,8192 -m 8 --user-buffer-size 16M --size-limit 32M \
  --cpu-percent 25 --no-dump-kernel-symbols \
  -o "$PROFILE_REL/perf.data"
```

Apply an external 30-second command watchdog; never change perf/security settings.
Recheck the worker and selected task identities afterward. Require nonzero
samples and record lost samples. Use exact unstripped engines/JNI and verified
packaged strip correspondence for symbols; collect both self and inclusive
stacks without adding inclusive percentages. Thread names are not ownership:
earlier `binder:*` names included the QEMU main loop and publisher, not just IPC.
Keep unknown/JIT generated-code samples as unresolved categories.

First obtain one frozen-current-APK profile on a declared stable visible Notes
scene to establish current TCG/TLS, timer, publisher and audio costs. For the
candidate, compare same-source API28 and API29 fresh workers in ABBA order with
at least two paired trials. Compare process CPU time/elapsed time, resolved
sample shares, real scene/input latency and PSS/RSS, not just total CPU%.
Measure an open performance panel separately: its sampling cost belongs to
the host process and must not contaminate the TLS baseline. A cold-load claim
requires a defined first launch/cache state and an explicit input-to-visible
interval; steady Notes samples are not app-loading evidence.

If results are inconclusive, regress guest behavior or show no repeatable benefit,
retain API28 as the default and preserve both candidate and failure receipts.

A three-ABI candidate can be operated with `run-candidate.py --plan PLAN.json`.
Preflight hashes the pinned source/tool/dependency inputs, checks tool versions
and fresh private destinations, and performs no configure or compilation.
Only an explicit allocation permits the second form:
`run-candidate.py --plan PLAN.json --execute-approved-sha256 EXACT_PLAN_SHA256`.
The private plan fixes API28, source revision, dependencies and sequential j1
commands. API29 experiments use separate plans and directories.

The driver retains each command PID, log, exit, GNU time report and sampled
aggregate RSS; it prints progress every30seconds and stops on deadline or RSS
ceiling. These are operator safeguards, not benchmark results or a hard kernel
memory limit. It retires only its owned command process group on interruption.
Full source/config/build outputs must be fresh: it does not migrate existing
cache stamps. ELF exports, SONAME, dependency closure and16KiB LOAD alignment
must match the plan before artifact freeze. Actual selected-source notices and
Bionic source kits are generated and validated; generating a source kit does
not prove a new source replay or replacement relink. Android/frontend source
pins remain the separate Product/Runtime candidate responsibility.

`python3 tests/portable-candidate.py` performs tiny host-only driver checks,
without invoking a native compiler or guest. A real full candidate still needs
an allocated build and subsequent same-workload target measurements.
