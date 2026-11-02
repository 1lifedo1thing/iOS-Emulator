# Controlled virtual Pixel memory comparison

The failed APK22 4GiB attempt is preserved evidence. Android LMKD killed its
worker for a breached minimum watermark; that establishes an Android capacity
failure in that attempt. Host disk swap was almost full, but retained samples
bracket the kill at513.840/529.915s: MemAvailable11439552/11360316KiB, SwapFree28KiB,
PSI memory some/full avg10/60=0 and total stall increase10microseconds over16.08s.
This weighs against sustained host memory thrashing at the kill; it cannot rule
out brief spikes. Historical host pswpin/out rates were not collected. Android
CPU/IO PSI permission denial is unavailable evidence, not zero pressure.

The next host baseline changes: the user authorized disabling disk swap and
persistent zram at50% host RAM, lz4, priority100. Root independently verified its
completion. Preserve the old swap file. The Pixel lane never administers swap.
Capture effective `/proc/swaps`, meminfo/vmstat and zram sysfs on every attempt;
do not attribute a combined host-zram/APK-frame-allocation/guest-RAM change to RAM.

## Exact state and target route

1. Root seals a matched production APK24 and the aa553 test overlay, its actual
   main SHA passed through `expectedMainApkSha256`. APK24 changes eager frame
   allocations, so a comparison to historical22 alone cannot measure a RAM fix.
2. Read-only parent is the preserved failed AVD's active userdata QCOW chain and
   matching encryption-key QCOW/raw chain. SDK qemu-img info confirms these
   formats; emulator help documents `-data`, `-datadir`, `-encryption-key` and
   `-cache`. Use `cow_plan.py` to emit external QCOW children, small fresh AVD
   config and an exact private launcher. It emits commands/text only and refuses
   overlapping directories, existing child targets and Git paths. Creating child
   images or launching the emulator still requires Root's exclusive grant.
3. Hash/pin every parent backing-chain file before use, retain the old failed
   logs/config and recheck parents after cleanup. Do not rebase, commit, repair,
   decrypt or write parents. Do not copy the multi-GiB userdata or extracted base.
   Each comparison fork starts from the same parent snapshot. All cache/config,
   implicit additional emulator QCOW layers, snapshots and lock outputs belong
   to its new directory; `-no-snapshot` remains mandatory.
4. Pre-guest admission must inspect actual emulator disk arguments/backing paths,
   verify parent images opened read-only, encrypted Android userdata mounted,
   actual16384-byte pages, expected package identity/selected record and
   unobstructed UI. The SDK help does not prove the emulator key-loader accepts
   nested QCOW; retain actual failure if unsupported. Root may then explicitly
   choose recovery on the original private test state with preserved failure
   logs rather than require a full copy. This document does not grant that route.
5. No import is run: the imported readonly base and assets are inherited. Start
   diagnosis in a verified fresh unused worker process. Record old importer4309
   and new PID/start ticks/UID; PID reuse alone is not fresh identity. This avoids
   the previous importer heap high-water factor without inventing an idle-Stop
   contract. The old importer is already dead in the preserved snapshot.
6. Retained selected record7f99ae0b4e8958311e6b337c416ab6b8 is expected dirty.
   Explicit `pixelColdRecovery=true` checks actual record metadata and invokes
   existing `recoverStart`, with priorDirty=true/coldRecoveryUsed=true and no
   clean initial admission claim. Never delete or synthesize a dirty marker.
   Parent dirty state is retained. A confirmed Stop may naturally clear only the
   child's marker. Normal Stop/PID retirement/persistence remain separately
   measured requirements; a successful screenshot is not a recovery pass.

The concrete next diagnostic is the8GiB fork, API36/16KiB/x86/KVM4cores,
SwiftShader/packagedANGLE, matched24, explicit cold recovery and no reimport.
A new4GiB fork using those same identities/host baseline/provider/state route is
the control. Root chooses serial order/resource allocation. Both keep the original
UI240/readback810/Stop840/hard900 clock and full300s post-Start admission. If setup
cannot fit, retain a budget failure; no restart or shortened interval. Continuous
host/guest monitoring starts at launch, continues through Stop and host-only
through cleanup, with actual gaps/denials recorded. Preserve scoped precleanup
raw image/QMP/logs and independently inspect actual Android/guest images.

Changing Android RAM also changes automatic TCG reservation and Android swap
policy. Measure them where available; virtual reservation is not resident memory.
Compare PSS/RSS/VmSwap, actual swap-counter rates/PSI, guest progress, LMKD/native
logs and strict shutdown evidence. Do not announce that8GiB fixes Android or the
physical Pixel unless target evidence supports that conclusion.

The original4KiB and16KiB runtime qualification remains required. This16KiB
capacity diagnostic cannot close4KiB acceptance or physical Pixel8/9/10 Tensor
CPU/GPU/audio/input/lifecycle gates. Gyro remains deferred and unqualified.
