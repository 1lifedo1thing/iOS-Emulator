# Retained state and a fresh-state control

Cold recovery boots retained writable data; it does not restore the imported
fixture. `Portable/src/Storage.cpp` pins state to the base lock, obtains its writer
lease, and rejects `.dirty` unless recovery was explicit. Existing `state/nor.bin`
is reused; only an absent writable NOR is copied from the immutable base. Native
boot arguments pair immutable `base/nand` with writable `state/nand` and use
`state/nor.bin` for NOR writes. Logs and the guest package offer live in state too.

Confirmed shutdown clears `.dirty` after successful engine and storage cleanup.
It preserves the NAND overlay, NOR, guest preferences, photographs and other
edits. A clean marker therefore does not mean unused state. Dirty admission
rejection, explicit recovery, a lit lock screen and successful wake are separate
observations.

The retained Poco attempts do not isolate state as the wake cause. Attempt15
imported a fresh record, reached Home/Photos, wrote a photograph, and later halted
cleanly. A same-state attempt16 crashed; attempts17 and18 recovered that retained
record and failed wake after reaching Lock. Attempt15 also recorded backlight-off
before its clean halt. Engine/native versions changed between attempts, and the
receipts do not compare NAND content before and after the crash. State history is
a confounder; corruption or a stored preference causing wake failure is unproved.

## Nondestructive owner-run comparison

Keep the current record and all diagnostics. Do not delete its marker, rename its
state, replace its NOR/overlay, adopt it as fresh, or edit record metadata.

1. Dispose its worker through Stop and retain the actual outcome. Record its ID,
   base lock/identity metadata, asset provenance, dirty marker and bounded overlay
   size/mtime listing. Missing asynchronous receipts do not prove clean shutdown.
2. Prefer an already-imported **never-booted** record of the same verified fixture,
   if an import receipt and state history establish that fact. `dirty=false` alone
   cannot establish unused state. Match board/build, base lock identity and ROM/
   package provenance; the prepared-stock recipe is a separate fixture control.
3. If none exists, the current production path is a new validated import of the
   same immutable fixture. `PrivateFixtureImportTest` adds a distinct record and
   checks the old records remain present. It copies the base and requires its
   fixed authorized inputs and integrity checks. This is an owner-run operation,
   requiring available storage and scheduled time; this source review performs no copy or hash
   of guest data. If copying is excluded, defer this route.
4. Preserve the new record's import receipt and ID. Before its first boot, confirm
   its new state has no inherited overlay/NOR edits or dirty marker. Let native
   admission create the lease, pin and initial writable NOR. Boot through normal
   Start, with `coldRecover=false`, in a fresh unused same-UID worker.
5. Keep candidate APK/engine/provider/debug configuration, foreground activity,
   settling interval and button sequence equal to the retained-state comparison.
   Record UID/PID, explicit record ID, admission result, sleep/backlight/agent,
   producer frames, raw captures and host pulse times before and after automatic
   sleep. Host timing does not measure guest virtual button dwell.
6. Keep the activity resumed until captures settle; avoid workload edits such as
   saving a photograph before the wake comparison. Retain failures and Stop's
   native confirmation/dirty result. Restore selection to the original record
   after the control, preserving both independent states.

The Portable `lt_device_open` API can pair an immutable base with a separate new
state directory, but the installed Android Binder API accepts record IDs only.
The library derives each record's fixed `base` and `state` paths. There is no
production no-copy fork/reset action; do not describe an alternate-state control
as runnable through the current UI or modify library files to simulate it.

Fresh-state success would support further comparison of retained history;
it would not identify a corrupt file or prove a repair. Failure on the same
fixture with independent fresh state would weaken that explanation. Both need
actual guest response, not admission counters or a fresh agent sample alone.
