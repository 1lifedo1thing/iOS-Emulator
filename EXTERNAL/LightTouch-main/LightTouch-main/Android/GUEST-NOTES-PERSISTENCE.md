# Opt-in Notes persistence evidence

`GuestNotesPersistenceTest` attaches with service flags0 to an **existing consumed,
running** worker. It creates no Activity/service/guest, never resumes or recovers,
and never presses buttons or requests Stop. Root must keep the actual foreground
session stable and inspect the guest before invoking it. Use a matched main/test
APK; ordinary owners do not need this instrumentation.

Required arguments in both modes:

```text
exerciseNotesPersistence=true
notesMode=write|read
existingNotesSessionConfirmed=true
expectedMainApkSha256=<installed main APK SHA256>
expectedWorkerPid=<existing worker PID>
expectedWorkerStartTicks=<same PID /proc stat field22>
expectedBootId=<Android boot UUID>
recordId=<32 lowercase hex selected record ID>
expectedBaseLockSha256=<that record's immutable base/device.lock.json SHA256>
notesMarker=LTNOTES-<new lowercase UUID>
```

Write mode additionally requires `focusedBlankNotesEditorConfirmed=true`: Root
must visually inspect a blank **focused** Notes body editor, with the expected
record/worker already running and agent available. The test verifies actual
FRONTMOST output `com.apple.mobilenotes`, sends exactly one TYPE_TEXT3 with the
whole marker, then SYNC4. It never retries. A host-app marker reservation prevents
reuse of that marker while the same app data remains intact; a failed/unknown
request must not be retried with another marker either. The reservation is not a
guest transaction or proof of durable insertion.

Read mode instead requires `notesBodyVisibleConfirmed=true`: Root must open the
existing saved note's full body through actual guest UI in a **fresh ordinary
worker of the same retained record/state**. Use the original marker and the new
PID/start ticks. Read mode sends only FRONTMOST1 queries and captures; it never
sends TYPE_TEXT or SYNC. It performs no OCR/text-extraction assertion. The marker
argument binds the expected visual comparison, not a claimed readback.

Operator invocation template (no command is executed by this document):

```sh
adb -s AUTHORIZED_DEVICE shell am instrument --no-restart -w \
  -e class org.lighttouch.android.GuestNotesPersistenceTest \
  -e exerciseNotesPersistence true -e notesMode write \
  -e existingNotesSessionConfirmed true -e focusedBlankNotesEditorConfirmed true \
  -e expectedMainApkSha256 MAIN_SHA -e expectedWorkerPid WORKER_PID \
  -e expectedWorkerStartTicks START_TICKS -e expectedBootId BOOT_UUID \
  -e recordId RECORD_ID -e expectedBaseLockSha256 LOCK_SHA \
  -e notesMarker LTNOTES-UUID \
  org.lighttouch.android.test/androidx.test.runner.AndroidJUnitRunner
```

For readback replace `write` with `read` and the blank-editor opt-in with
`-e notesBodyVisibleConfirmed true`; update worker identity, retain marker/base.
Use `--no-restart`; instrumentation/platform cleanup may still affect the app.
Do not treat a missing/dead worker as permission to start one automatically.

Evidence remains private under `files/notes-evidence-MODE-TIME/result.json` and
`files/screenshots/notes-evidence-MODE-TIME/`. It contains main/worker/boot/record/
lock identity, the consumed record's state-derived engine-log binding, full
status at stage boundaries, exact request IDs/admissions/polls, cancellation,
BOOTTIME client deadlines and before/after capture hashes. Failed operations
retain a best-effort status/capture without masking their original error.
Requests use10s native timeouts and11s client polling budgets inside a60s client
collection budget. Synchronous Binder calls are not interruptible by that
budget; retain an independent bounded owner watchdog. No mutation is retried.

`collectionComplete` means that this bounded harness finished; every result
keeps `guestAcceptance=false`, `guestAcceptancePassed=false` and
`persistenceVerified=false`. Root must inspect the **entire marker in the actual
note body**, then separately press Done/Notes back through guest UI, obtain a
strict clean Stop receipt/storage marker/old-worker retirement, ordinarily start
the same state in a fresh worker, open the saved body, and visually compare the
read capture. SYNC alone is not a Notes-save or clean-shutdown proof. A title,
preview, successful request, cached screenshot or host-only assertion is not
persistence acceptance. Agent insertText exercises the guest UI text operation;
it does not qualify physical keyboard or multi-touch delivery.
