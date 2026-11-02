# ARM64 Android 16 KiB matrix continuation

The persistent M4 virtual Pixel is an Android compatibility target. Its outer
Android VM uses native ARM64/HVF and host Apple graphics; the inner iOS CPU
remains TCG. Apple emulator graphics cannot qualify a physical Pixel's
Tensor/PowerVR driver. Native macOS app checks are separate evidence.

The latest read-only inventory establishes Android17/API37, actual16384-byte
pages and native arm64-v8a. Previous actual guest runs rendered Earth Lock and
confirmed clean shutdown. Home, unlock/menu input and the complete final
acceptance workload remain open; a Lock screenshot is not a Home pass.

## Reuse the fixed instrumentation artifact

The test-only APK SHA256 is
`177e7c71d2a42c8d083bf06f21f48cd0180485d01258aae3b073883e5df9fa30`.
It was built from test revision2776fde27604084de8fe0421471d4f66c10230ba.
Its GuestUnlockTest and GuestUnlockReadiness sources exactly match the reviewed
11a92e42 fix. The compiled viewport method reads activity.lifecycle.currentState;
remaining ActivityScenario.state calls belong on the instrumentation thread.
This repairs a test helper, not the guest renderer.

The Mac's installed main APK is
`c28d082f197ad1c97013062e30460179827858ccb8313f407d784afd12f6ad3f`.
All four DEX files match the main APK used to supply Test31's compilation cache.
The complete223-entry application compile JAR is also byte-identical, SHA256
`be73d4ee094ab759b6b8f3b7d0bfff08639abd6cf507c59c4b66441da0b6fc32`.
This establishes exact static main API compatibility, including the private
getStatus/getDisplayAsleep reflection targets, GuestDisplay input observations
and IEngine methods. APK signatures match, and the test APK has no native code.
An additional test build is unnecessary. Actual ART/instrumentation execution
against this installed main remains unverified.

Preserve the installed main, native engine, prepared base and writable state.
After an allocated test-only replacement, verify its installed hash and rebind
the actual worker PID/start ticks, boot ID and typed unused status. Do not assume
an installation preserves process identity. Do not substitute the older Test30
unlock implementation: it is different from this fixed Test31 artifact.

## Finite target run

Reuse the existing reviewed controller and independent Stop watcher. Refresh
all state, helper, controller and immutable capture bindings; never replay
consumed grants or a stale current-baseline inventory. The retained source14
controller and its old grant template have different hashes and are not an
executable pair.

Run one ordinary Start, no recovery/retry Start. Preserve the original180s
prestart admission,300s observation,260s input admission,25s unlock command,
35s native Stop budget and700s outer bound. Stop foreground recovery, if
required after a terminal helper failure, consumes the same35s budget.
Keep the existing host resource guard:8GiB admission/8.5GiB continuous owned
RSS;7GiB admission/6GiB continuous reclaimable memory;32GiB free disk;
20 percent minimum memory-pressure availability;256MiB maximum swap growth.
Do not restart another AVD or change graphics/security settings for this run.

Review new immutable native/Android capture pairs before input. Require actual
Home/menu evidence and strict confirmed shutdown with the old worker retired,
clean state and a free lease. Retain failures, raw captures and exact producer
identities. A successful short run qualifies only its declared scope; final
multitouch, concurrent media, lifecycle, persistence and sustained acceptance
remain separate requirements. Gyro remains deferred and unqualified.
