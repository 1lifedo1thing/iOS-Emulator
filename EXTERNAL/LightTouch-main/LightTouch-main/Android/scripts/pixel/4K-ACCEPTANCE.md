# Required 4KiB AVD guest acceptance plan

This is a source/metadata checkpoint, not an AVD allocation or guest pass.
The host reboot at22:53:53UTC ended the earlier API36/16KiB session; its old
process and clock bindings cannot admit a new test. Preserve its existing COW
state and historical evidence. Any restored5554 instance remains outside this
4KiB lane. Actual Poco or physical Pixel4KiB execution does not satisfy the
separate4KiB AVD requirement.

The current private source package is
`builds/pixel/prepared-g5-api35-4k26-source-03/` in the coordination workspace.
Its `allocation-metadata.json` contains proposed SDK argv arrays and input
destinations; `inputs.json` pins the APK26 pair. The earlier metadata package
`plan-g5-api35-4k25d-01/original-4k-image-metadata.json` records read-only backing
chains; `resource-justification.json` retains the memory calculation;
`compact-index-metadata.json` describes only the compact archive index. No AVD,
image child, NAND copy, asset push, ADB server, compiler or guest was started.

## Available control environment

The existing `LightTouch_API35` smoke AVD uses API35/default/x86_64 revision2 and
Pixel6 geometry. Its historical JNI smoke actually reported4096-byte pages and
SwiftShader EGL; pixel-transfer probes passed, while the historical GLES1 render
probe failed an FBO-completeness assertion. Those results are prerequisites,
not real iOS acceptance. The API36/16KiB image also changes Android version and
image flavor, so comparisons do not isolate page size.

Reuse the installed API35 image and the existing smoke AVD's backing chain, with
new small external QCOW children and a separately named private configuration
`LightTouch_API35_4K_G5_26`. Do not mutate the original smoke disks. The proposed
creation command uses `system-images;android-35;default;x86_64`, profile `pixel_6`,
no `--force`, and stdin `no`. None of these creation commands has run.

Reserve emulator port5556/serial `emulator-5556`. Port5554 is reserved for restoration of the existing
persistent16KiB session and must never be stopped, restarted, reparented
or targeted by these tests.
The new test uses4GiB Android RAM,2 virtual CPUs and outer SwiftShader. KVM applies
to Android's x86 machine; iOS still uses QEMU TCG. Host GPU passthrough is absent.
`-no-audio` means counters can be recorded but speaker playback cannot be passed.

Use sealed main26 SHA
`99b601ee73f09ce9e5f836f9d6636b0971f9d0e4dd29e9e86f3b46dcc4fe160a`
and matching test26 SHA
`30ea248ba52a38fcbadd395ed8d4f20a71842301ebfd0d8369c697afa50e755e`.
Its packaged stripped x86 engine SHA is
`6796ae4ae9401414e4aace4a799ee67f4a172c3d0148598f31916977ebd44274`.
Select inner LightTouch `platform` explicitly. An actual missing/broken platform
GLES path may justify a separately approved fresh-worker `angle-gl` attempt;
retain the platform failure and each provider's scope. Never switch providers
silently or qualify a physical Tensor/vendor GPU from software AVD results.

## Memory admission before allocation

The retained8GiB16KiB APK25/07 log has338 samples spanning0.342–675.690s. Minimum
host MemAvailable was7,067,908KiB; maximum attributed guest-worker RSS687,876KiB
and PSS548,931KiB. A later host-only read of the same QEMU reported VmHWM8,437,204KiB,
RSS7,204,628KiB and VmSwap1,186,404KiB. That high-water value includes subsequent
user activity and is not a causal measurement of one guest operation.

Do not allocate a second8GiB VM. The proposed4GiB VM budget reserves1.5GiB for host
emulator/render overhead and2GiB host headroom: require at least7.5GiB current
MemAvailable, stable memory PSI and an exclusive heavy allocation. This is an
explicit conservative planning estimate, not a proven peak or guarantee. The
latest retained snapshot had7,343,188KiB available, below7,864,320KiB, so this
proposed gate **failed**. Recheck after n81/Poco work; do not reclaim or stop the
user VM to meet it. Zram capacity and swapped-out memory are not free physical RAM.

Continuously sample host MemAvailable, PSI, swap/zram and exact owned QEMU RSS;
retain Android/worker memory and denied reads through guest Stop and own cleanup.
Pressure invalidates timing comparisons. Resource revocation may terminate only
the explicitly owned4KiB worker/AVD through its reviewed cleanup, never5554.

## Two phases with separate original clocks

1. **Provision only:** use a bounded4KiB service/UI session, verify actual
   `getconf PAGE_SIZE=4096`, install the pinned pair and inspect the real UI by240s.
   Provision the verified246,405,911-byte n72 compact archive, ROM and armv6 guest
   package to5556 only; run the existing explicit `PrivateFixtureImportTest`.
   Retain its new actual receipt, selected record, app UID and record/base/assets
   hashes. The index declares111,319 entries,464,293,295 payload bytes and an
   estimated913,121,280 bytes after4KiB file rounding; require at least2GiB actual
   free `/data` before import. No iOS Start is authorized in this phase. Retire
   the completed idle importer and preserve the child state without reimport.
2. **Guest:** reopen that same4KiB child under a new immutable attempt origin.
   The first normal Start must use the actual successful new import receipt plus
   current selected-record/base/ROM/package pins and clean-marker admission.
   Import is not a prior guest clean shutdown. Do not fabricate a clean receipt,
   reuse the16KiB06 receipt, create a dirty marker or force unnecessary recovery.
   Require a fresh unused worker, explicitly selected provider and the complete
   admitted300-second observation within original UI240/readback810/Stop840/
   hard900 bounds. Start the after-Start watcher concurrently, inspect raw and
   Android Surface images, and preserve actual identity/deadline receipts.

Separating provisioning is necessary: the earlier16KiB import took237s and a
later Start took62s. Combining import, boot, UI and full300 observation risks the
original490s admission boundary. The second phase must not reset an in-flight
clock; it is a distinct test with preparation already complete.

## Narrow review required before these commands become launchable

Current Pixel helpers and the common bounded supervisor fix serial5554. The
Pixel plan builder also closes the expected page size to16384. They cannot be
used unchanged alongside the user VM. The private `REVIEW-ONLY-serial-page.patch`
shows concrete5556/4096/2-core substitutions; it has **not** been applied to the
live worktree or treated as tested runtime code.

Apply reviewed changes only to a pinned private source snapshot, or agree a
small checked parameter change separately. Include every ADB/logcat/capture/
admission/monitor/Stop route and launch port. Pin the adapted legacy controller,
its dependencies and exact main/test paths. Require host tests proving that no
reachable operation targets5554, occupied5556 refuses admission, and disposal
cannot signal a replacement or the user QEMU. Do not extend the closed16KiB
persistent-owner contract or introduce a general orchestration framework.

The normal-plan checker currently requires a genuine prior clean Stop receipt.
Add a separately reviewed initial-import admission using the exact new passing
import receipt and current clean record, or use the existing verified-record
gate with an equally explicit checked initial-state contract. This is a real
capability delta, not permission to invent a prior shutdown. Final record IDs,
pins, token and launch manifest are generated from the actual provisioning
receipt; they cannot honestly be prefilled now.

After review and resource approval, execute only the finalized generated argv
for5556. The proposed external-COW `qemu-img create` commands use the existing
userdata/key/cache QCOW2 parents, not full copies. Hash/permission/backing-chain
and read-only-parent FD checks are required at that allocation. The SDK's key
loader and API35 child mounting still need actual verification.

## Acceptance still required

Inspect actual Home and a built-in app, guest input including required
multi-touch, concurrent render/audio/input and lifecycle. Confirm normal guest
shutdown, same-child fresh-worker restart and persistence. Retain failures and
unconfirmed dirty state. Software graphics and muted-host limitations must remain
explicit. Gyro stays deferred/unqualified. Static packaging, historical smokes,
an incrementing frame counter, Lock Screen alone and physical Poco results do
not close this4KiB AVD gate.


## Private source snapshot sealed after review

The proposed substitutions and narrow initial-import admission now exist in the
private snapshot `builds/pixel/prepared-g5-api35-4k25d-source-01/`, separate from
the original closed16KiB helpers. Root independent review of this complete
private snapshot remains pending. Its source-seal SHA-256 is
`b79a6aaf253e3a8078e068bfc884b4e0094c0d83bdbb3b316d46541527761e04`.
All65 host tests passed, including original-root PID reuse, unrelated5554
nonsignalling, exact5556 routes, initial import versus prior-clean admission and
real host subprocess signal cleanup. These are host checks, with Android calls
mocked; they do not qualify the target. The older review-only patch above stays
historical.

`prepare_boot.py --check-source` verifies the sealed tiny sources and the exact
main25/test25d pair without SDK/device commands. `OPERATOR.md` and the emitted
`provision-command.json` retain the future executable route. Actual allocation
still requires a fresh resource check and Root grant. No4KiB image child,
firmware read/copy/import or VM launch occurred. Physical Pixel debugging takes
priority over executing this separately required AVD qualification.

Version01 remains immutable historical evidence: its initial-import binding
reported importer retirement from tuple inequality without proving the old
process exited. It must not be used for allocation. The replacement private
snapshot `prepared-g5-api35-4k25d-source-02` has source seal
`5b1eda957823d4e77a35a16e6601ef3bc82817098f6c4928e792967e6ba2bae7`.
It requires bounded original-process absence/reuse evidence in the same Android
boot, or a fresh matching bracket proving the prior Android boot ended; failed
reads do not mean absent. All72 host tests pass, including live-importer rejection,
permission/read failures, PID reuse, boot changes and a real host process exit.
Root reviewed version02; actual4KiB guest qualification remains pending.

Version03 repins only `inputs.json` and the operator note to the actual APK26
pair, packaged x86 engine and new unallocated4KiB name/path. Every namespace,
retirement and controller code byte equals version02. Its source seal is
`69d69b4c9c59e07dd70b6d5195b53dffdb5ab19431361dd565e53968c65c864c`.
All72 host checks pass again; no image creation, firmware read, ADB or VM launch
occurred. Run the package's `prepare_boot.py --check-source`, then use its exact
operator commands after a fresh Root resource grant. The provision phase creates
the genuine import proof; the guest phase generates its record/token/pins from
that proof. No old25 APK or pre-reboot clean receipt admits APK26.
