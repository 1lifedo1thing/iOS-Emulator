# Bounded shutdown decision diagnostics

`IT_SHUTDOWN_DIAG=1` enables at most eight `[shutdown-diag]` stderr records per
fresh portable worker. Any other value is off. Apple builds remain off. Set
the closed flag before starting the worker; this is not a setting, arbitrary
trace selector, new public engine API, or a shutdown correctness change.

Each invocation of `qemu_ios_ui_shutdown` can emit one record:

| Field | Meaning |
| --- | --- |
| `seq`, `at_ms` | Unique bounded record sequence; host monotonic milliseconds. |
| `ready`, `storage` | Existing admission checks. Storage is sampled only when ready. |
| `agent` | Existing status: -1 not sampled, 0 absent, 1 alive, 2 stale. |
| `agent_present`, `prefer_agent` | Status was positive; status was exactly alive. |
| `submit_attempt`, `submit_ok` | Whether the existing agent request was tried and accepted. |
| `route` | Existing selection: 0 refused, 1 agent, 2 scripted powerdown fallback. |
| `cancel_stable`, `cancel_seq`, `cancel_reason`, `cancel_age_ms` | Most recent recorded host agent cancellation, sampled with trylock. |
| `cont_stable`, `cont_seq`, `cont_result`, `cont_running`, `cont_age_ms` | Most recent completed `qmp_cont` callback; one atomic snapshot attempt. |

Cancellation reasons are 0 none since reset, 1 ID not found, 2 removed
undispatched request, 3 dispatched request cleared the claim/token, 4 removed
completed result. No request ID, body, path or guest content is logged. A
claim-reset reason describes the cancellation operation, not proof the claim
was previously valid. Its serial and timestamp belong to the agent object;
machine/agent reset clears them. Old internal cancellation callers retain
their unrecorded behavior. Only the host wrapper records when the flag is on.

Resume results are 0 no completion recorded, 1 returned without error,
2 returned an error, 3 refused because storage had failed. Its serial counts
callback completions, not scheduled requests; metadata lasts for this worker
process. The BQL callback only caches atomic result/state/time fields. It does
not read the environment, allocate, log or take a diagnostic mutex. The host
entry sets the enable bit before scheduling that callback.

Read cached metadata only when its stable flag is true; an age of -1 is
unavailable. Age and serial do not tie an earlier cancellation or resume to
this Stop as its cause. Records can appear out of sequence under concurrent
callers; quota admission has at most eight CAS attempts, with no retry loop
waiting for a writer. The decision record is emitted on the host caller, not
the BQL callback. Agent snapshot contention makes its metadata unavailable
instead of blocking on the agent lock.

The default shutdown checks, request, fallback, pause acknowledgments and
35-second host watchdog remain unchanged. Route selection is not guest receipt,
recognition of a scripted gesture, kernel halt, storage flush or clean shutdown.
The existing `ipod_touch_pwroff_log` trace separately reports gesture phases;
it is not enabled by this flag. A stale agent lease or failed submission needs
actual decision evidence before selecting a behavior fix.

Prepare a focused production-function slice without a compiler:

```sh
python3 tests/portable-shutdown-diag.py --prepare-only --output NEW_PRIVATE_DIR
```

Without `--prepare-only`, the script requires an allocated compiler slot and
runs portable/Apple variants with O2, warnings-as-errors and ASan/UBSan.
It extracts actual cancellation/reset/snapshot, control callback/scheduler,
host cancellation and shutdown decision functions. Tests cover normal routing,
flag-off behavior, cancellation reasons, claim/token effects, busy snapshots,
resume success/error/refusal, unstable metadata, and the eight-record limit.
Queue, clock and atomic primitives are mocks; this is not a concurrency,
full-engine, guest shutdown or target-performance test.
