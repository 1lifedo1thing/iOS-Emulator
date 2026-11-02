# Graphics evidence beyond adapter probes

The frozen production adapter probes exercise real GL commands, pixels and
resource state; they do not execute an emulated guest. Keep their manifests and
results separate from guest boot/presentation results. A mock capability test is
an error/state check only. Screenshots of Android's own boot/home screen are not
guest rendering evidence.

## Remaining guest evidence

For the baseline n72ap-7E18 route, retain the backend commit and exact engine
library hash, frontend APK/worker hash, admitted fixture/guest-tool revisions,
actual host page size, renderer/version and worker UID/PID. The normal app
`:engine` process must execute the real guest graphics protocol and publish
actual visible guest frames through the copying API. The engine's initialization
log identifies the requested guest API with the actual host renderer; this is
context evidence, not proof that a shader workload was rendered.

Observe a lit guest Lock Screen/Home and an interaction that visibly changes
its pixels. Keep a copied guest screenshot and an Android Surface screenshot at
corresponding frame sequences/times and inspect both. A frame counter or black
image is insufficient: retain backlight/frontmost state to distinguish sleep,
failed rendering and actual guest shutdown. Repeat after pause/resume, Surface
replacement/activity recreation, orderly stop and a fresh worker restart.
The real guest's required multi-contact input and concurrent render/audio
behavior remain part of integration validation; gyro is deferred/unqualified.

## Bounded n72 button timing trace

The non-Apple S5L8720 GPIO diagnostic uses the existing
`ipod_touch_gpio_read_log` event, disabled by default. It records at most 128
actual guest reads of pad `0x0c` at offset `0x184`, including the returned value,
guest PC and `QEMU_CLOCK_VIRTUAL` nanoseconds. The same event records at most
16 actual Home/Power pad transitions with their value, direction, IRQ and
virtual timestamp, after the model changes the pad. A bottom-half transition
may have PC zero because it has no current vCPU. These are physical model
transitions and guest reads, distinct from frontend button admission.

Budgets are process-wide, independent for reads and edges, and cannot be
refilled by toggling tracing. Other pad reads do not consume the read budget.
Only the non-Apple S5L8720 diagnostic is filtered/bounded this way; the Mac path
and other SoCs retain their existing trace behavior. No GPIO/IRQ/polarity/PMU
state, pixel, clock or public API behavior changes. Rebuild the two GPIO/button
translation units and package the resulting engine before target validation.
The pure `gpio-wake-budget` regression validates disabled/filter/exhaustion
behavior, not actual button delivery or kernel debounce timing.

Enable the event after the real guest agent is ready, around one bounded
press/hold/release attempt, and disable it promptly afterward. The existing
QMP interface can toggle it without boot-wide tracing:

```json
{"execute":"trace-event-set-state","arguments":{"name":"ipod_touch_gpio_read_log","enable":true}}
```

Use the same command with `enable:false` afterward. Retain the exact engine
hash, existing SYSIC/PMU events and the new `[gpio-wake]` lines. The Home bit is
bit 1 and Power is bit 2; each should be set while held and cleared on release.
With only the resting active-low volume-down bit set, pad values are `1` at
rest, `3` for Home and `5` for Power, but preserve other pad bits in the receipt.
Subtract the down/up edge `virtual_ns` values for actual guest-time duration
and locate guest reads with the relevant bit set before release. This does not
establish a target debounce threshold or compare the separate raw timer counter.
Do not poll read-to-clear PMU event registers or override brightness.

Documented n72 Home/Power GPIOs `0x0c01`/`0x0c02`, active-high flags `0x100`, and
IRQs `0x79`/`0x7a` match the host mapping and observed group3 bits25/26. A separate
source discrepancy remains: the 2G handler writes SYSIC `gpio_int_level` as
live pad state, while SYSIC models it as guest-programmed interrupt polarity;
the 1G uses its physical-pad setter. The observed press/release polarity flips
still align, so this diagnostic does not claim that discrepancy caused the
actual wake failure and does not modify it.

The root-owned Poco old-engine attempt independently observed three `0x184=3`
reads while Home was held and three `0x184=1` reads after release, plus two
`0x124=4` reads. That establishes live Home pad consumption for that attempt.
Its trace event was previously disabled and restored, with 704 bytes logged
over 1.884 host seconds. The receipt explicitly lacks guest virtual timing and
does not verify wake. The new timestamped engine has not yet run on that target;
the existing observation must not be relabeled as its validation.

For a black or transparent image, set `IT_GLES_FRAME_METRICS=1` before starting
the engine in a bounded diagnostic run. The first successfully copied frame
and every sixtieth frame report their actual drawable FBO, guest destination
base, copy path, dimensions/stride, source format, exact nonzero RGB/alpha and
opaque pixel counts, mean RGBA and canonical RGBA hash. RGB565 has implicit
alpha, explicitly identified in the log. Metrics read only the buffer actually
copied, exclude padding and use the accepted dimensions. They make no GL calls,
consume no GL errors and modify no pixels. Existing verbose/profiling runs also
include these bounded metrics.

Match those lines to the real guest's copied frame and Surface frame, retaining
LCD scanout/base, backlight/frontmost state and command errors. Nonzero RGB with
zero alpha is different evidence from black RGB with opaque alpha. These metrics
describe copied bytes; they do not assert successful GL readback, CA composition
or visible presentation. Check the real command/error receipt as well. The old
progress mean sampled the reusable readback as a full RGBA drawable even for
RGB565, cropped or direct IOSurface copies; that sample has been removed.

### CA offscreen writeback diagnostics

CoreAnimation can render imported CoreSurface textures offscreen and publish
them through guest scanout memory without calling the explicit drawable-present
operation. `gles_sync_surface_1` marks the attached imported surface dirty;
`gles_surface_flush` writes dirty surfaces on `glFlush`, `glFinish`, return to
the default framebuffer, or a surface alias reread. Consequently, absence of
`[gles] present target` does not prove absence of guest-memory copies.

With the fixed `IT_GLES_FRAME_METRICS=1` flag set before engine startup, the CA
path separately reports at most 16 successful copied surfaces, 16 flushes that
attempt writeback (including failures), and 4 empty flushes per process. Verbose
or profiling alone does not enable these CA summaries. Successful-copy lines
are emitted only after every row reaches guest memory and its generation is
updated. They identify the initialized surface's texture key, executor FBO
identifiers, guest base, accepted format, target, size, guest/readback stride,
page count, row reversal and generation. CPU-only summaries cover BGRA32,
RGBA32, RGB565 and RGB555; the latter has blue in the low five bits and red in
the high five bits, matching the existing BGRA writeback packing. Both packed
formats have implicit opaque alpha. The unused RGB555 bit 15 and row padding
do not enter the canonical RGBA summary.

Flush lines report the requested base, surface count, attempted/completed
writebacks and the existing return result. A failed flush therefore remains
visible even if it has no successful-copy line. The metrics do not add GL
queries, read or clear GL errors, change pixels, alter dirty handling, or change
the existing writeback return value. FBO and texture identifiers are in the
executor namespace; they must not be treated as a guarantee of native driver
object names. A successful copy does not establish LCD scanout selection,
backlight, Android presentation or a guest acceptance pass.

The synthetic `frame-pixels.c` regression checks RGBA/BGRA, RGB565/RGB555,
alpha distinctions, asymmetric channel order, the unused RGB555 bit, padded
rows, cropping and invalid sizes without GL or guest assets.

### Frozen Android/Linux log comparison

The actual packaged 4 KiB AVD attempt used the normal app UID and verified all
197 public ANGLE dispatch owners. Its early `engine-full.log` (214 lines,
SHA-256 `ef0863e3ac95240bc42233d1bb862de80fff6198e4b2129887baa1db7c671c17`)
ended before CA attachment. Its later `engine-cleanup.log` (229 lines,
`cbd4b5bfd323cd7aa76bde1441397a049c6e09832037a047a95c1c7d650769f3`)
contains CA attachment and four offscreen `glDrawElements` records. Actual
copied images show an Apple boot logo while lit, then black pixels when the
modeled panel sleeps. Visible Lock/Home/CA rendering remains unverified.

Existing Linux GUI runs 11 and 12 use engine
`1ffcb57dab386ad2e144337e7670ea4b058ad18608500b6c7d72db2624ba03ff` from
producer `b88aaabee7b57427bbb68f345eae3878716266db`. Each log records 16
offscreen draws, the logger's cap; their log hashes are respectively
`319da8e4594f83c067ee7ed77baf9081820c265642770b60b13bacb995ba07ab` and
`76fc5ea49302a99f9a16da9e96a30371886571d38fcc00ebdbd8c53b43dd5848`.
Their real copied Photos-entry images were independently viewed and contain
the guest's Photo Albums UI. The Linux runs reused a writable state; Android
used a fresh independent baseline import, so elapsed-time/state differences
are not controlled evidence of a driver cause.

All three late logs lack explicit drawable-present, refusal and raised-GL-error
records. Their shared offscreen texture-unit and blend fields are therefore
not isolated failure markers. These frozen logs do not identify the CA
writeback base, format, completed rows or resulting pixel content. The new
CA metrics close that observation gap in a future engine only; no existing
artifact or target result is retroactively qualified by this source change.

GLES2 needs an actual admitted guest/workload that compiles and executes shader
commands, not the standalone host GLES2 triangle. The supplied n81 route may be
used where appropriate with its prepared kernel/NAND/NOR and matching package;
do not request an arbitrary ROM or copy the large fixture merely for this test.
Record API2 context and successful shader/link/draw behavior with actual pixels.
A short bounded `IT_GLES_CALL_TRACE`/existing trace event budget can help identify
protocol calls; keep debug traces private and avoid unbounded call logging in
performance runs. Do not infer GLES2 guest compatibility from an ES1 baseline.

Live native EGL state blocks graphics snapshots. Show snapshots as unavailable;
validate cold restart in a new worker instead. A screenshot is not a snapshot.

## Android packaging and presentation requirements

The frontend loads `lighttouch_jni` in a same-app-UID `:engine` service. Its CMake
imports the real Bionic engine dependency closure; the NDK runtime is supplied
through AGP. The engine must select `gles-host.c` with `GLES_HOST_EGL` and
`GLES_HOST_EGL_ES`, link platform EGL/GLESv1_CM/GLESv2, and exclude the graphics
stub. Inspect every ABI's dependency closure rather than copying Linux glibc
libraries into the APK. Native EGL/GLES calls occur on the engine thread; the
frontend's CPU Surface upload does not replace guest GPU execution.

The ABI publishes copied BGRA8888 frames, top-left origin and byte stride. The
JNI presentation/capture paths convert BGRA to Android RGBA and use the actual
source/window stride. Compare red and blue pixels, orientation and letterboxing
against the copied guest frame; a uniformly green probe does not detect channel
swaps or vertical inversion. Surface replacement must not use an old window or
stale pixels, and restart must reset frame/resource generations.

The current frontend pins AGP8.9.2/NDK28.2 and requests uncompressed JNI packaging.
Android's [16 KiB guidance](https://developer.android.com/guide/practices/page-sizes)
requires compatible ELF segments and ZIP alignment, including prebuilt shared
libraries. Verify the final signed APK, both ABI closures and every dependency's
LOAD alignment; compiler flags on the JNI wrapper alone are insufficient. Use
[zipalign's check mode](https://developer.android.com/tools/zipalign):

```sh
zipalign -c -P 16 -v 4 FINAL_SIGNED_APK.apk
llvm-readelf -l -d EXTRACTED_SHARED_LIBRARY.so
```

Retain a package report with APK/library hashes and source/build revisions, then
load and run the actual engine in the packaged app on the physical arm64 phone,
4 KiB x86_64 AVD and 16 KiB x86_64 AVD. The currently promising host/Mesa AVD
mode requires an isolated X display in a headless environment; supported mode
selection alone does not qualify the full renderer or guest. The scoped probe
runner verifies frozen hashes and final markers on an already allocated AVD;
root still owns allocation, package install, phone access and integration runs.
