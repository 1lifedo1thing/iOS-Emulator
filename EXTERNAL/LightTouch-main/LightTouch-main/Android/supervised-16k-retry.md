# Bounded 4 KiB and 16 KiB Android guest attempts

This Linux supervisor requires an explicit exclusive AVD allocation. Preparing
the tool or passing its synthetic tests is not an allocation or target result.
It refuses any existing `emulator-5554` device-list entry, including offline or
unauthorized entries, and refuses launch if enumeration fails. It explicitly
binds emulator port 5554. Every target Android command specifies that serial.
It launches one AVD in an isolated Xvfb as the current user, using fresh
sudo credentials, two cores, requested 2048 MB RAM, `-gpu swiftshader`, no window,
no snapshot and no audio. It never wipes data or changes the GPU/image/version.
The closed `--memory-mb` selector accepts 2048 (default) or 4096 only. Use 4096
only for a reviewed allocation: retained API36 first-boot evidence showed
minimum-watermark LMKd kills and framework ANRs at actual 2560 MB before the
secondary SystemServer death. This supports a controlled RAM hypothesis, not a
proven fix. Preserve the same emulator, GPU, CPU count and AVD data. No manual
heap override is applied; record the actual RAM and derived heap from startup
logs/hardware-qemu.ini and retain host/guest pressure observations. The published
emulator [memory option](https://developer.android.com/studio/run/emulator-commandline)
supports overriding AVD RAM. Capacity and the exclusive allocation must be
checked before launching; source tests alone do not authorize a 4 GiB VM.
Retained evidence showed the emulator raised RAM to 2560 MB; inspect the new
actual startup log rather than labeling requested RAM as actual RAM.

The closed `--cores` selector accepts 2 (default) or 4 only. Four guest cores
require a separately reviewed allocation. The retained API36 run with actual
4096 MB and two cores reported 91.91% guest CPU pressure and 99–100% aggregate
CPU use around a 20.309-second Keyguard ANR, before application installation.
This supports a CPU capacity experiment; it does not prove the ANR's cause or
that four cores will resolve it. Startup work may also block on IPC or I/O.
Keep RAM, image, emulator version, SwiftShader, heap policy, userdata and all
deadlines fixed. Record actual CPU configuration, host affinity/quota/pressure
and runtime KVM evidence; an acceleration capability check alone does not
prove the running emulator used KVM. Repeated boots of retained userdata may
change startup work independently. Android documents the
[multi-core setting](https://developer.android.com/studio/run/managing-avds).

The closed `--expected-page-bytes` selector accepts 4096 or 16384 only, with
16384 as the unchanged default. It verifies the actual `getconf PAGE_SIZE`
response exactly; a mismatch fails before service/record/UI readiness. The
selector changes no emulator arguments or deadlines. A 4 KiB route must name
its separately allocated 4 KiB AVD and explicitly pass
`--expected-page-bytes 4096`; merely choosing a different AVD never relaxes the
16 KiB default. Launch receipts record the expected size, and readiness
receipts record only the actually matched size. Neither value is guest proof.

Use the retained successfully imported record. This tool neither imports,
selects, installs APKs nor starts a guest. Freeze the intended main/test APK and
verify their actual installed identity separately. Supply the selected record
ID from its import receipt, the SHA256 of the verified prepared base's
`device.lock.json`, and the verified ROM/package SHA256 values. If a prior
record.json hash exists, also supply `--record-sha256`. No private record IDs,
firmware paths, asset bytes or hashes are embedded in this source.

After the allocation, launch the supervisor as a foreground command in a
persistent execution session. Retain the running session ID and inspect both
supervisor and independent watchdog PIDs. Do not background it with nohup and
end the launching command: a retained attempt showed the command runner removed
both those processes while the emulator's separate session survived. The model
may continue other bounded commands after the execution tool returns a running
session. For example, with task-specific variables containing reviewed paths
and receipts:

```sh
python3 -u Android/scripts/supervise_16k_retry.py \
  --allocation-granted --user "$task_user" \
  --adb "$task_adb" --emulator "$task_emulator" \
  --avd LightTouch_API35_16K --record-id "$task_record" \
  --lock-sha256 "$task_lock_hash" --rom-sha256 "$task_rom_hash" \
  --guest-sha256 "$task_guest_hash" --output "$task_output"
```

The output directory must not exist. Before 240 seconds from supervisor start,
the device must return the selected exact page size (16384 by default), report
boot complete, and retain one
system_server PID with functioning package/activity services for 30 seconds.
The record/state/base/assets must exist; selected identity, record schema and
source pins must match; the dirty marker must be absent. The tool checks actual
ROM/package bytes and lock hash without another asset copy. Dirty state fails
this normal-start recipe; explicit recovery is a different authorized action.
Native startup still performs its full integrity, lease and admission checks.
Path checks require an explicit remote presence/absence sentinel and readable
parent. A zero host exit from `adb exec-out` is not a remote command-success
guarantee: retained target evidence returned zero alongside a missing-marker
stat error. Missing, unknown and present are separate outcomes.

## Explicit fresh-library staging

For a new isolated AVD, replace `--record-id` with `--fresh-library` and provide
its explicit `--avd` name. These modes are mutually exclusive. Keep the verified
source lock/ROM/package hashes. The fresh mode's 240-second gate verifies only
the selected exact page size, boot complete and one stable system_server with package/activity
services for 30 seconds. It makes no run-as, installed-app or record assumption.
`ready.json` records `freshLibrary=true`, `recordGatePassed=false`, `appUid=null`
and `recordId=null`; UI and guest flags also remain false.

The controller must verify/install the frozen main/test pair and capture/view
the new application's UI within the original 240 seconds. Then provision the
legitimate private baseline inputs only to this new Android target and run the
existing opt-in production import test. The fresh import counts against the
original 900-second budget; neither completing import nor binding its record
resets the start, Stop or hard deadline. The supervisor does not import, select,
start or recover a guest automatically.

After the actual import test terminates successfully, extract its app-private
`files/import-evidence-MILLIS/result.json` bytes and SHA256. Atomically publish
`import-record.json` in the private output directory with these fields:

```json
{
  "schema": 1,
  "recordId": "HEX32_FROM_ACTUAL_IMPORT_RECEIPT",
  "appUid": 10200,
  "importReceiptPath": "files/import-evidence-MILLIS/result.json",
  "importReceiptSha256": "SHA256_OF_ACTUAL_RECEIPT_BYTES"
}
```

The UID shown is illustrative; use the actual receipt/target UID. Receipt paths
are restricted to that fixed app-private import-evidence pattern. The
supervisor reads the actual receipt through run-as, compares its bytes to the
pin, and requires `pass=true` with the same record and UID. It then independently
checks selected record identity, source lock/ROM/package pins and actual bytes,
readable state and absent dirty marker, and compares the live app UID. Only
after those checks does it write `record-bound.json` and `import-binding.json`.
Wait for both before guest startup. Import binding after 240 seconds is allowed
until the existing 840-second Stop boundary; it is not delayed UI acceptance.
The bound ID/UID/pins are fixed. Replacing the controller record fails closed.

If no record was verified and bound, Stop explicitly records that its marker
path is unknown instead of inspecting a guessed record. Missing binding or a
timeout does not establish clean shutdown. The existing-record mode retains its
240-second selected-record and clean-state admission checks.

`ready.json` records only this service/record gate, with `uiGatePassed=false`,
`visualUIInspectionRequired=true` and `uiGateDeadlineSeconds=240`. It does not
launch a previously installed application. After that handoff the controller
verifies and installs the frozen matching main/test APKs, runs scoped
`am start -W` with a bounded 30-second timeout, then captures and views the
actual new application's Android UI by the original 240-second deadline.
Installation, foreground launch and image review do not reset either clock.
Read a capture's SHA256 from its actual capture receipt when recording explicit
visual approval; do not transcribe it from memory. A rejected local hash or
image argument must not approve the UI or be reported as a target failure.
Retain that rejection and allow a corrected argument before the original
240-second deadline while the controller's approval wait continues. Missing
timely approval still requests Stop. Native, target, ownership and import
binding failures continue to request Stop, and an existing Stop cannot be
undone by a later approval.
An Android framework ANR overlay does not establish a usable application UI.
If a bounded Wait action is appropriate, use the actual UI hierarchy's enabled
Wait control bounds. A resized image displayed by an inspection tool may use
different coordinates from the target's pixels; do not tap those displayed
coordinates. Retain the hierarchy, action and a fresh screenshot, then visually
verify the overlay cleared before the original 240-second deadline. A late
correction does not extend the gate or establish guest acceptance.
Hierarchy resolution is only for a specifically inspected ANR overlay; a
visibly unobstructed application does not need that additional prerequisite.
If this UI gate fails or misses that deadline, create `request-stop` and retain
the failure; do not start a guest. An older APK's foreground timeout is not a
new guest failure. The supervisor receipt is not UI, guest, provider or
rendering acceptance. Reuse
the existing record in a fresh unused worker, select the fixed debug ANGLE enum
before the first context, and foreground MainActivity before every
`am instrument --no-restart`. Do not repeat standalone GL probes. Retain actual
normal app UID, 16384 page size, exact packaged engine/library identities and
the 197 public address-owner lines. Allow actual same-worker guest readiness
up to 600 seconds, subject to the overall deadline. A logo or agent ALIVE does
not justify a Frontmost request or input test. Inspect recognizable Lock/Home
and Photos pixels before those stages. Surface/raw captures and any concurrent
media test remain independently reviewed evidence.
The provider instrumentation test shares its original observation deadline with
a subsequent frame wait. Provider verification is recorded first. Frame evidence
requires a coherent copied and producer sequence/timestamp, a lit awake panel
and a publish age of at most five seconds on the same native clock; the nonzero
pixel requirement remains explicit. A static coherent frame may qualify this
frame evidence. Raw capture attempts and errors are retained even when the wait
fails. After capture, the same worker must still have a fresh coherent copied
frame before the original deadline; an earlier ready observation cannot qualify
a subsequently stale cache. Capture diagnostics do not grant another readiness
interval. Status and bitmap capture are separate calls: these observations do
not attach an atomic frame serial to the PNG. None of these receipts verifies a
particular guest scene or extends the external budget.

An independent forked OS process tracks only this emulator launcher's
descendants and their Linux start ticks. At 898 seconds it requests termination;
just before 900 it force-disposes remaining owned processes even if the supervisor or
interactive model turn stalls. PID reuse cannot confer ownership. No global
ADB server, other AVD, X session or physical phone is affected. This mechanism
bounds emulator resources; it does not establish orderly guest shutdown.

At 840 seconds, or when a controller creates `request-stop` in the private
output directory, the supervisor attempts a single production Stop. It reads
the current UI hierarchy and taps only a unique enabled LightTouch semantic Stop control;
there are no guessed coordinates. It observes the identified worker for up to
35 seconds within the remaining budget and extracts own-PID shutdown/error
receipts and dirty-marker status. If UI/Binder admission fails, the error is
retained and cleanup still proceeds. It never calls a blocking native status
method or changes markers to manufacture success. Missing receipts, forced
disposal and a disappeared PID alone are unconfirmed. Independently require
native terminal confirmation, correct record/state identity, dirty-marker
removal and persisted guest user state before claiming clean Stop/recovery.

Boot/service failure terminates this attempt without fallback. Continuous
Android logs begin at the first available device; command receipts retain
service/process and Android/host memory-pressure samples. Binary asset checks
retain lengths/hashes instead of embedding firmware bytes. At terminal, inspect
the exact owned PIDs and receipt before releasing the allocation. Keep all
outputs private and retain failed samples; no screenshot or fixture goes in Git.

## Focused checks and JNI build preparation

The supervisor tests use real hung synthetic host subprocesses, without Android
tools or an emulator. They check independent deadline disposal, disarming after
cleanup, start-tick ownership, reused-root exclusion and semantic Stop lookup:

```sh
python3 -m unittest discover -s Android/scripts/tests -p test_retry_supervisor.py -v
```

JNI normal/recovery routing uses one C++17 helper with a fixed boolean. Ordinary
`Native.start(root,id)` passes false; `Native.recoverStart(root,id)` passes true.
The Kotlin declarations and trailing Binder recovery operation match these
exports. Existing CMake applies C++17, debug definition `LT_ANDROID_DEBUG=1`,
the pinned NDK API29 targets for both ABIs, and 16384 maximum-page alignment
at link time. No new native compile definition or public Portable ABI is needed.

When a compiler allocation is granted, derive the syntax argv from the frozen
compile_commands.json entry ending in lighttouch_jni.cpp: replace its source
with the reviewed JNI source, remove `-o OBJECT` and `-c`, and append
`-fsyntax-only -Wall -Wextra -Werror`. Preserve all other target/sysroot/include
and debug flags; run arm64-v8a then x86_64 sequentially. The targets are
`aarch64-none-linux-android29` and `x86_64-none-linux-android29`; the pinned NDK is
28.2.13676358. Record the database and source hashes with command exits. For a
configured candidate build, `ninja -C PRIVATE_ABI_BUILD -j1 lighttouch_jni`
rebuilds the native adapter and its required dependencies; that requires its
own build allocation. Syntax success alone does not verify linking or packaging.

Shared Contracts.cpp already tests dirty refusal, explicit recovery and marker
preservation. The separate recovery UI and opt-in coldRecover test must also
be verified with the matching packaged JNI. No target recovery or input result
is inferred from this source audit or synthetic process test.
