# Host performance panel

Measured CPU engine comparisons and their limits are recorded separately in
[CPU-PERFORMANCE.md](CPU-PERFORMANCE.md).

Current Android builds include cumulative CPU time and the **QEMU worker CPU**
label. This describes the host engine process; it is not an iOS or vCPU-only
statistic. Historical candidate 25 retains its original **Emulator CPU** label
and six rows; current builds use seven rows.

The collapsed **Performance** button requests no samples. While the panel is open
and its Activity is started, a separate UI background reader requests a cached
snapshot each second. The worker admits at most one collection every two seconds,
with one in-flight read and no queued history. Closing/backgrounding the panel
stops requests; one already admitted read may finish. No timer continues sampling
in the worker. A stuck platform memory read cannot queue more reads or block a
native/control/audio lock. Worker destruction interrupts the sampler without
joining it.

The displayed values have distinct scopes:

| Label | Actual measurement |
| --- | --- |
| QEMU worker CPU | Host CPU used by the whole `:engine` process: CPU-time delta / elapsed-time delta, multiplied by 100. One busy core is 100%; multiple cores may exceed 100%. This includes QEMU execution, worker support/audio threads and sampler overhead, rather than only emulated vCPU execution. Candidate 25 calls this same measurement **Emulator CPU**. |
| QEMU worker CPU time | Existing `processCpuMs` counter converted to cumulative CPU seconds consumed by this worker process. It is available on the first successful counter read, even while the percentage is warming up. It is not wall-clock guest uptime or iOS scheduler accounting. |
| Emulator memory | Current worker's reported proportional set size (PSS), converted from KiB to MiB. This is neither RSS nor guest RAM usage. Android may omit protected allocations such as graphics. |
| Android RAM | Device memory available / total memory accessible to the kernel, converted from bytes to MiB. Available is not identical to free memory. |
| Android whole-device CPU | Unavailable; no privileged or `/proc` fallback. |
| iOS guest CPU / RAM | Unavailable. The current guest protocol has no measured guest statistics operation. Configured guest RAM and host TCG CPU are not substitutes. |

The CPU counters are `Process.getElapsedCpuTime()` and
`SystemClock.elapsedRealtime()` milliseconds. Their interval includes Android
deep sleep. Guest Pause does **not** reset or force CPU to zero: host work can
continue while emulated execution is paused. A new PID, panel/foreground
observation epoch, invalid/regressing counter, nonpositive interval or gap above
10 seconds warms up a fresh baseline. A valid zero CPU delta is displayed as 0%.

Each metric includes its own elapsed-time sample/read-completion age and the
worker PID. CPU rate and cumulative time share the same counter-read age. A
missing/failed CPU read is unavailable, not cumulative zero; a valid zero is
allowed. Cumulative time belongs to the current worker PID, continues during
guest Pause if the host does work, and is not reset by opening the panel or
resetting the rate baseline. A new worker shows its own counter, never its
predecessor's accumulated time. The UI additionally requires the sampled PID to
match the current status PID. Platform memory reads are not an atomic snapshot with CPU or each
other; the Bundle additionally retains collection completion/cost and raw units.
A slow read leaves prior values visible with increasing ages. Missing/failed
reads are unavailable, never silently substituted with zero. A nonpositive PSS
from a live worker is also unavailable: the public void memory wrapper does not
expose successful map reading and can leave a zero-initialized result unchanged.
This differs from a valid measured zero CPU delta. Seven fixed-height,
single-line rows prevent sample updates from changing the viewport; opening or
closing the panel can resize it and retains existing gesture cancellation rules.
Candidate 25 has six rows; later producer APKs include the seventh cumulative-time
row. Opening the panel in those APKs therefore has different geometry from sealed
25. Installing a test-only runner does not change the main application's layout.

The single trailing `IEngine.performance(long observationEpoch)` method only
requests collection and returns scalar cached data. Its observer key is the
calling process PID plus an Activity-local token. This token provides **no**
display, input, guest or process-control authority. A changed observer or demand
gap above four seconds invalidates old cache publication and resets the CPU
sampling epoch. Late UI responses are accepted only for the same Binder and
current observation token. There are no changes to Native/status/observations,
Portable APIs, audio policy, settings or gyro support.

`PerformanceCountersTest` contains the original eleven regression cases covering multicore
and zero usage, PID/epoch changes, invalid counters/clocks, long gaps without
invented guest-pause semantics, cadence/single-flight, observer handoff, blocked
read closure, duplicate completions, and an unfilled void-memory-read result.
Those eleven passed in candidate 25's actual 41-test JVM run. Three additional
source cases cover first-read cumulative time before a valid rate, missing or
wrong-PID counters, and cumulative time across observer/rate resets. All fourteen
passed a separate cached Kotlin 2.1.20/JUnit 4.13.2 host run using the exact
production counter helper and test sources. The updated Compose UI compiled in
candidate 29 (App 6b07201c, QEMU e7a56d80b1, 49 JVM tests). Later producer APKs
have rendered the seven-row panel on the Poco. Actual screenshots and counter
intervals were reviewed for the current worker, panel close/reopen, Pause/Resume
and a brief Android background/return sequence. One static Home
closed/open/closed observation also met its predeclared bounded CPU/PSS overhead
thresholds. This is evidence for that device, APK and workload; it does not
establish universal overhead neutrality, concurrent media performance or
completion of multi-touch/lifecycle acceptance. Compilation and host counter
tests alone do not qualify rendered UI or target measurements.
No new sampler, Binder/Native API or guest-tools operation is introduced. The
current protocol exposes no genuine iOS CPU/RAM measurement; host QEMU worker
CPU time already provides the available VM-process view without a guest-tools
port. Sealed candidate 25 and its test-only runners remain unchanged.

`MONITOR-COLLECTION.md` describes same-PID closed/open/closed observations. Its
collector explicitly recognizes both the original six-row labels and the new
seven-row labels, retaining the layout identity; mixed or incomplete CPU labels
are rejected. Host collection does not substitute for target UI inspection.

Primary API definitions checked for this implementation:

- [Process CPU time](https://developer.android.com/reference/android/os/Process#getElapsedCpuTime())
- [Elapsed time, including deep sleep](https://developer.android.com/reference/android/os/SystemClock#elapsedRealtime())
- [Own-process memory information and protected-allocation limitation](https://developer.android.com/reference/android/os/Debug#getMemoryInfo(android.os.Debug.MemoryInfo))
- [PSS in kB](https://developer.android.com/reference/android/os/Debug.MemoryInfo#getTotalPss())
- [Available and total device memory](https://developer.android.com/reference/android/app/ActivityManager.MemoryInfo)

PSS failure semantics are grounded in the pinned Android 14 public
[void self-memory wrapper](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-14.0.0_r1/core/java/android/os/Debug.java)
and [native failed map-load branch](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-14.0.0_r1/core/jni/android_os_Debug.cpp).
The hidden PID overload's boolean result is not used. A positive-only live-worker
PSS check avoids interpreting an unfilled result as a successful zero measurement;
it does not diagnose the cause of an unavailable read.
