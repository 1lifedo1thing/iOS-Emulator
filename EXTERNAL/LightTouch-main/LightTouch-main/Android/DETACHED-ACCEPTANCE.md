# Finite Android acceptance controllers

Use `tools/detached_test.py` for a reviewed finite controller that must continue
when its invoking terminal or assistant session ends. It starts a separate host
supervisor session and a separate controller session, records their identities,
and retains stdout, stderr and an atomic, synced host result. It never retries.
This Linux host utility needs Python 3 and `/proc`; it does not run on Android.

Prepare a private JSON plan with `execution_authorized: true`, an explicit `argv`
array with an absolute executable, an absolute `cwd`, `timeout_seconds` in
`(0, 600]`, an original `admission_deadline_ns` from `time.monotonic_ns()` no more
than 90 seconds ahead, and a `sha256` object mapping absolute executable/source
paths to their SHA-256 hashes. Pin the real interpreter, controller, its imported
helpers and input manifest. Use the interpreter containing required dependencies.
Keep device identities, grants, fixture paths and captures outside Git.

Launch once with:

```sh
python3 Android/tools/detached_test.py --execute /private/plan.json \
  --sha256 PLAN_SHA256 --out /private/new-output-directory
```

The parent directory must already exist. An existing output directory is refused.
Neither a launch PID nor `launched.json` establishes that the guest started or a
test passed. Inspect `controller.json`, `stdout`, `stderr` and `result.json`, then
independently inspect the controller's target evidence. `result.json.success`
only reports controller exit status; `deviceShutdownVerified` remains false.
Each device action still needs its normal ownership, source and identity checks.
An operator must not change the output directory to retry an uncertain action.

The original host deadline is fixed at admission. At that deadline the supervisor
kills only its controller process group. A device shutdown guard must run in its
own detached session, retain its original device deadlines and persist success
or failure independently. A host timeout cannot verify guest shutdown. A host
reboot invalidates monotonic clocks and requires fresh review, never a resumed
trial with replacement deadlines. This protects against an invoking session
ending; it does not promise survival of a host reboot or an explicit kill of the
supervisor itself.

Shutdown evidence has two different persistence contracts in the Android app.
A native terminal receipt is written and synced before terminal disposal. Deadline
disposal starts a best-effort daemon writer and immediately kills the worker;
the worker can die before a deadline file exists. An absent deadline file therefore
cannot prove that disposal failed or that native shutdown succeeded. Retain dirty
markers, worker identity/retirement, app messages and independent host deadlines.

Verify the supervisor without a phone or guest:

```sh
python3 -m unittest discover -s Android/tools -p test_detached_test.py -v
```

These tests exercise a real invoking-session hangup, detached collection, a fixed
timeout killing the controller's descendant group, refused source drift/output
reuse, and invalid admission/reboot rejection. They do not qualify Android or
iOS behavior.
