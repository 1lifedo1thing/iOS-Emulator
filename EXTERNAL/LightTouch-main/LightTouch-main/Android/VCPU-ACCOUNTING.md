# Fixed debug vCPU accounting

This source-only diagnostic is independent of the host Performance panel. The
panel's CPU rate/time describe the whole emulator worker, including support
threads. Engine vCPU accounting describes execution/wait categories of the
emulated CPU thread; neither supplies genuine iOS scheduler CPU/RAM usage.

`IEngine.debugArmVcpuAccounting()` has no arguments and is appended to the Binder
interface, preserving prior transaction IDs. A debuggable own-app caller may
request it only in a fresh, unused worker. JNI also requires `LT_ANDROID_DEBUG`
and rejects a runtime, in-progress start or any prior native start attempt,
including one that failed before runtime creation. It sets exactly
`IT_VCPU_ACCT=5`; there is no caller interval, variable name, path or command.
Successful calls are idempotent before consumption. Failure returns a bounded
message and does not falsely set the configured flag.

`WorkerStartAdmission` serializes the configuration callback with irreversible
Service start admission. A concurrent Start cannot pass the initial unused check
and consume the worker in the gap before configuration. Its monitor is released
before foreground notification, storage import/hash admission, Native.start or
status/audio/window work. JNI takes its existing control guard only for the
fresh-worker check and fixed setenv. No binding flags, affinity, scheduling
priority, power hints, governor or UI/settings controls change.

The trailing status boolean `vcpuAccountingConfigured` reports the actual
successful process configuration, not proof that the engine emitted samples.
The existing engine reads the environment in `mttcg_acct_init` on the vCPU thread
before its execution loop (`accel/tcg/tcg-accel-ops-mttcg.c`); discovery/loading
the library through records() does not itself consume this setting. Its fixed
five-second reports go to the already retained private `engine-PID.log`. Retain
actual `[acct]` output, UID/PID/start identity and exact engine/main hashes before
interpreting run/halt/io, BQL or thread CPU fields. Missing output is unavailable,
not zero usage. Accounting wall time, thread CPU time, guest virtual MT trace
time and Android BOOTTIME are distinct clocks.

The existing QMP instrumentation accepts the additional **explicit** opt-in:

```text
-e debugGuestQmp true -e debugVcpuAccounting true
```

Its fresh branch configures accounting before QMP and Start, records requested,
initial/pre-start/started observed flags, and verifies a consumed worker refuses
reconfiguration without changing worker errors. Each snapshot retains the flag.
`snapshotOnly=true` cannot arm an existing guest: the requested option requires
the current worker to have been configured before it started. Omitting the flag
never calls the setter, disables nothing on an already configured worker, and
preserves ordinary operation. Existing dirty-recovery/foreground/hold requirements
remain independent; no automatic restart or cold-state acceptance follows.

Use a matched newly compiled main/test pair. These methods and status tail do
not exist in immutable APK25 or test25d/e attempts. The new admission tests cover
failed configuration without consumption, irreversible refusal after consumption
and an actually blocked concurrent Start waiting for configuration commit. They
are source coverage pending compiler allocation; no build or target pass is
claimed by this checkpoint.
