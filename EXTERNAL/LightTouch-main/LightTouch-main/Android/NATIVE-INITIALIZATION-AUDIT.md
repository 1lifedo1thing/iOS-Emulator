# Android native loader and thread ownership audit

Source-only review of the compiled candidate16 source `75a32b01a850763df2ab37431dc4d4888ae08ae6`, JNI bridge source `7ae8397a16b641d25ee7678164f5c6fdc6078e7d`, main APK `937e6fd706f96b3d95957a71269b13849393fca1471391cd912bcb0b84abc882`. This audit does not establish the actual first loader thread, thread exit or crash cause. It introduces no initialization workaround. Backend owns the QEMU constructor/RCU lifetime contract; runtime owns Portable entry/guest thread analysis.

## First native load

`Native.kt` is a Kotlin singleton whose initializer calls `System.loadLibrary("lighttouch_jni")`. Actual candidate16 `Native.class` disassembly has a static initializer that constructs/publishes `INSTANCE`, calls `System.loadLibrary`, then returns. The actual packaged `liblighttouch_jni.so` has `DT_NEEDED libqemu-arm.so`, verified in the candidate16 dependency/strip receipt. QEMU constructors can therefore execute during this dependency load, before any explicit Portable `dlopen`/board discovery call. A later capability-discovery `dlopen` must not be assumed to be the first engine load.

The actual `EngineService.class` constructor stores deferred watchdog lambdas and constructs its Binder object; it does not read `Native.INSTANCE` or invoke a Native method. The watchdog deadline comes from the generated 35000 ms BuildConfig constant, so it does not initialize Native while constructing the service. `onBind` returns the Binder, and `onStartCommand` creates the foreground notification without a Native call. `onCreate` starts the `guest-audio` Java thread. Its first loop iteration reads `Native.INSTANCE` and invokes `Native.audio`, even before a guest is selected/started or audio focus is held.

| Candidate initializer | Entry path | Lifetime boundary |
| --- | --- | --- |
| Java `guest-audio` thread | `EngineService.onCreate` launches thread; first iteration calls `Native.audio` | Service-instance `alive` flag, not process lifetime |
| Remote service Binder pool thread | `IEngine.records/status/surface/import/start/debug…` first Native access | Framework-selected service process thread; product code does not own/join it |
| Service main thread | Native cleanup/focus callbacks, e.g. `onDestroy` | Process main thread; not a guaranteed first initializer |
| Shutdown admission thread | Deferred stop/cancel/status functions when Stop is requested | Daemon coordinator worker; normally Native has already initialized, but code does not require that |

The first audio and Binder invocations race after service creation. Android documents service lifecycle callbacks on the main thread and remote Binder calls on a process thread pool; the UI caller's short-lived `device-library`, `worker-status` or `guest-start` thread is not the remote Native execution thread. See [processes and threads](https://developer.android.com/guide/components/processes-and-threads).

`MainActivity.onServiceConnected` launches `refreshDevices`; that UI-process Java thread makes `IEngine.records`. In the dedicated `:engine` process a Binder pool thread calls `Native.records`, which holds `library_guard` and calls `lt_engine_board` for each record. Zero records do not invoke the board call, but earlier singleton initialization may already have loaded QEMU. `Native.start` similarly executes in a service Binder thread before Portable creates its separate native guest thread. No product-side ordering pins engine dependency loading to that later guest thread.

## Initializer lifetime need not equal process lifetime

`EngineService.onDestroy` sets its instance `alive=false`, requests Native contact cancellation and abandons audio focus. It does not invoke Native Stop or kill the process. The `guest-audio` loop then leaves, releases its AudioTrack and returns. There is no product-owned QEMU/RCU reader unregister call when that Java thread returns. The Native singleton and loaded DSOs are process-owned, not reset by creating a new service instance.

A bound-only library/import/discovery client need not start the service foreground. When its final binding closes, Android can destroy the service while retaining the process as cached; a later bind can instantiate a new service in that still-existing process. If the original audio thread won Native initialization, it can already have returned before a later guest starts in the same process. This is a source-backed reachable lifetime mismatch, not proof that it happened in the target crash. See [bound services](https://developer.android.com/develop/background-work/services/bound-services) and the process-lifetime documentation linked above. `AcceptanceBinding.close` and `MainActivity.onDestroy` unbind their clients; whether another binding/start keeps the service alive depends on the actual run.

Failure/disposal paths also set `alive=false` before own-PID disposal. The independent Stop deadline kills without joining audio or native presentation; receipt IO cannot delay it. Startup failure posts a callback that stops the service and kills the PID. These paths retain the fresh-worker policy for started sessions, but do not create an engine-side TLS teardown contract. A Java audio-loop exception outside its inner playback handler could also end that thread; no such event is claimed observed here.

Required diagnosis remains the actual first dependency-load thread, its registration/unregistration/exit, and the failing RCU list ownership from retained native evidence. Root/backend/runtime own that measurement/fix. Moving initialization to another Android thread without fixing or proving the native lifetime contract would be a workaround hypothesis and is not applied.

## Concurrent acceptance clock domains

The new test checkpoint uses `SystemClock.elapsedRealtime[Nanos]` only for host read bounds, deadlines, chord timing and measured interval labels. It uses `SystemClock.uptimeMillis` only to construct Android MotionEvent source timestamps. These clocks differ across deep sleep: elapsed time includes it, uptime excludes it, as documented by [SystemClock](https://developer.android.com/reference/android/os/SystemClock). Neither is subtracted from the other.

Frame/PCM/guest-wire timestamps from status/logs remain raw evidence. Frame timestamps use the engine's monotonic clock; PCM capture timestamps and guest virtual trace times have separate conventions. The test subtracts only successive counters from the same worker and records their adjacent status sample indices. A qualifying interval is bounded by two held-contact observations and requires fresh frame and nonzero playback deltas inside that window. Status reads are not atomic cross-subsystem snapshots, and the interval does not prove exact instantaneous overlap, per-touch guest consumption, visual response or audibility. All corresponding manual flags remain false. This review does not use cumulative pre-test playback to qualify concurrent work.

Private reproducible audit inputs bind the actual candidate16 class files, `javap -p -c` outputs, frozen Kotlin source files and packaged JNI dependency/strip receipts. No target operation or new compiler invocation was performed for this audit.
