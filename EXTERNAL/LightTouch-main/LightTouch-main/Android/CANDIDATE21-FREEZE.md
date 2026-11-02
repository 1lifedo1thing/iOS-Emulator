# Candidate21 source freeze and Surface ownership review

Candidate20 remains the sealed Android3409/native53ea/engineb59 producer. Its
retained stale captures identify a producer/copy divergence, not the callback
order or presenter blocker. Candidate21's telemetry and ownership work is a new
source candidate; it has no compiler or target acceptance receipt yet.

## Ownership conditions to review before freezing

- An explicit active/focused View transition receives a worker-issued lease.
  Retain the actual worker/Binder identity with the lease. A later service pointer
  must not make a previous worker's numeric token valid in a fresh process.
- Created/changed/reconnect callbacks use their retained active lease. A retired
  View's late callback cannot acquire or reactivate ownership. MainActivity's
  reconnect must route through the current View rather than attach directly.
- Lease admission and the native mutation must be atomic under the native control
  guard. Recheck attach authority after Surface conversion. Keep platform
  conversion, geometry, lock/post and reference release outside that guard;
  never hold a service control mutex across those platform operations.
- A stale owner cannot clear the new Surface, cancel its contacts/buttons or pause
  its worker. Check old Activity.onStop and window-focus loss, not just destroy.
  Explicit user Pause/Stop and the current owner's full lifecycle cleanup still
  release input. A resize with no View-owned contacts preserves unrelated
  Home/Power pulses. Drop obsolete local contacts even when native release is
  rejected; suppress continuations until a new admitted DOWN.
- Review stale touch MOVE/UP and other callbacks as well as cancellation. A valid
  local Surface or Activity object does not establish current worker ownership.
  Accepted Surface parcels/native references retain balanced ownership and posts.

Required source regressions include A→B→late A destroy/focus/onStop/reconnect,
old slow conversion completing after B, same-owner duplicate callbacks,
worker replacement with colliding numeric tokens, held-contact transfer,
unowned resize during a button pulse, and rejected parcel/reference cleanup.
These checks establish the contract, not actual guest rendering or wake.

## Freeze after root approves the final source

1. Obtain the exact final Android and native Git revisions from root. Do not use
   a provisional worktree HEAD or describe candidate21 native files as53ea-equal.
   Export Android plus its tracked catalog; export Portable, CActivation and all
   selected native/helper files. Preserve the actual source used for JNI, whose
   path may be under the Android snapshot rather than the Portable snapshot.
2. Compare candidate21 against the62-file candidate20 native baseline. Record
   unchanged, changed, added and removed paths with old/new SHA256 and source
   revisions. Enumerate newly added native helpers; a fixed62 count must not
   omit them. Changed JNI telemetry/lease code belongs to its new producer even
   when Portable/firmware implementation bytes remain unchanged.
3. Freeze the root-selected engine inputs and matching notices per ABI. Recheck
   the actual public headers/fixed QMP helper against that engine producer.
   Keep old20 engines, archives, APKs, runners and receipts immutable. New source
   kits must bind the new actual JNI/firmware inputs and packaged outputs.
4. Record the exact Kotlin/AIDL/JNI signatures, observation-array length, key
   order and clock domains from the final code. Preserve and verify the old
   observation prefix. Label native phase/window telemetry as sampled evidence;
   stable counters do not prove a nonnull window or a completed platform call.
5. After an allocated build, select actual compile-command/object origins from
   the new output directory, excluding copied warm-cache origins. Verify gates,
   effective optimization, typed/recovery/observed implementations, compiled
   lease regressions, source-to-NDK-strip mappings, complete native closure,
   notice hashes, signatures, catalog and ZIP/ELF16KiB alignment. Report host/JVM
   execution separately from compiled-only Android cases and target evidence.

A candidate21 build must not silently adapt the candidate19-only unlock helper to
another main APK. Any diagnostic identity change needs its own reviewed source
and producer receipt. Actual lifecycle recovery requires paired owner/window/
phase observations and current coherent raw frames under the same worker; a
cached screenshot or shell exit0 is not an acceptance result.
