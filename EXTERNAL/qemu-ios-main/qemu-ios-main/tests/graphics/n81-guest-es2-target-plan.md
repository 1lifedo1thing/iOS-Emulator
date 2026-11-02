# Bounded n81 GLES2 qualification on cleanup engine259

Prepared2026-10-14, source-only. No launch/import/allocation is implied. Bind the
actual producer and target receipts at execution; a later APK/frontend requires
its own manifest rather than this plan's candidate23 attribution.

## Inputs and workload

Engine source259e4ad31702f4927ed8d06a06b64219121bc0a0 includes checked EGL object
cleanup and deferred final-release retention. Root's ASAN/UBSAN injected-EGL
regressions passed; they establish failure-policy safety, not actual driver or
guest rendering. The supplied n81ap/8C148 prepared kboot base and matching armv7
k48-ios4 serial26 package supply the existing GLES2 route. Retain the verified
fixture lock/package/source receipts. No ROM, historical SDK, gyro probe or new
third-party guest application is required.

Use the built-in SpringBoard/QuartzCore compositor: obtain a fresh inspected
Lock/Home image, one inspected unlock/app transition, then its changed image.
The old Linux b88/1ff run initialized guest-api2 and logged16ES2 shader-path draw
attempts but captured aged black pixels and reported EGL_BAD_ACCESS at teardown.
These logs prove entry to the route, not visible rendering or this producer's
compatibility. gles_es2_draw may log an attempted draw while a missing guest page
is being faulted in for retry. Do not count every draw log as a completed draw.

## Existing diagnostics and the question each answers

Keep IT_GLES_FRAME_METRICS=1 fixed before engine startup for the diagnostic run.
This summarizes only bytes actually copied, consumes no GL errors and changes no
pixels. Its separate CA budget is16completed writebacks,16attempted flushes and
4empty flushes; drawable copies report first/every60frames. Retain the complete
log from launch so early boot does not exhaust the budget before collection.

| Retained evidence | Interpretation and next boundary |
| --- | --- |
| guest-api2 initialized, shader compile/link refusal, or GL draw error | Actual context/command path; a refusal remains failure. A context or attempted draw alone is insufficient. |
| CA flush attempted>0/copied0/result-1 | Writeback failed; retain base/format/FBO/path and existing refusal. It is not an Android Surface failure. |
| CA copied api2/accepted format/stride/dimensions/base/nonzero RGB+alpha/hash | Shader-context surface bytes reached guest memory. This does not prove that LCD scanout selected that surface. |
| CA copied nonzero but raw guest frame black while awake | Compare guest scanout, surface generations and publication; do not paint a replacement frame or infer an app scene from RGB counts. |
| Raw guest pixels current/nonzero but native presentation unchanged | Compare source/copy/present sequence and Surface/window epoch/phase/errors; this distinguishes presentation from guest rendering. |
| sleeping1 or backlight0, old publication | Retain as sleep/stale evidence, not a renderer failure or scene pass. Inspect a bounded wake's new pixels separately. |

Match the API2 copy's context/base/generation/path with actual subsequent native
frame receipts and current pixels. A zero-alpha colored buffer is different from
opaque black; packed RGB565/RGB555 have implicit alpha. Explicit drawable-present
logs may be absent during normal CA offscreen compositing: dirty texture writeback
and framebuffer scanout are the applicable publication path. Do not require a
present-target line that this workload need not issue.

Normal shader compile/link failures and GL errors are already logged. A separately
allocated Linux diagnostic can additionally bind a fixed bounded call trace/context
trace if the frontend actually exposes those startup settings. Do not invent
caller-controlled Android environment/path options. The Android debug-only fixed
QMP arm currently enables copied-pixel metrics before a fresh unused worker starts.
Use that admission only through a reviewed n81 harness; the n72-specific fixed
register/wake/unlock helpers are not n81 qualification tools. Arming diagnosis is
not executing or passing a QMP/graphics test. Native metrics must remain default-off
for ordinary workers.

## Linux execution recipe for its owner

Use the supplied original read-only folder directly. A new state gets only the
small writable NOR, sparse NAND overlay, lock/lease and generated checked offer;
never import/copy the full base merely to run Linux. No metadata adaptation,
chmod, legacy Mac endpoints or old good-verdict file is permitted. Use current
engine discovery for machine/dimensions and explicit matching raw armv7 package.

The sealed cleanup engine's Linux library SHA is
a85e640b791aa8e1260ed2c2349e7807a14cf71e3864805b5bd3fd03717c5737.
Runtime supplied frontend32c5bb84745663732663bff629e4a3b657ede212b174aa024976f869ad3efbba
from51736577; a newer lazy-frame frontend is being prepared. This plan must be
repinned to its actually selected executable/manifest before launch.

Use the existing observed admission contract:900s admission,30s checkpoint-stall,
5s cancellation acknowledgement; --seconds360 starts at HELLO. The existing
1340s external supervisor limit covers admission+guest+35sStop; never restart its
clock on a tool/observation timeout. Record PID/startticks/window/UID and explicit
Mesa backing. Launch the existing readiness capture watcher before HELLO, at most
2Hz/12captures, retaining each source-frame/diagnostic receipt and original BMP
immediately. It must not wait for a model reply through the brief lit interval.

Independently inspect fresh640x960 n81 images. The fixed320x480 n72 slider/ROI is
inapplicable. Derive n81's actual scene/viewport before a bounded balanced Home
wake or unlock. Capture before and after the one approved visible transition,
with paired SDL image if comparing presentation. Preserve black/stale attempts.
No audio or multi-touch acceptance is inferred from this initial GLES2 run.

## Poco prerequisites and execution boundary

Use a normal app-UID worker with platform-default Adreno provider, not the x86-only
ANGLE test. Candidate23 main/test are separately sealed with Android/Native2d33
and259engine; its packaged ARM engine hash is
9693024e80667882daa747c403906d22a5ae6047b0a08026ded2f7c5a8f6d9bf.
Bind installed main/test/native/engine hashes and worker PID/UID before asserting
any target behavior. Candidate23 is not yet a guest GLES2 result.

The target needs one legitimate full logical n81 base in its private library.
Current directory UI import stages another full copy before native import and
should not be used under the no-extra18GB-copy constraint. Prepare a verified
compact ITPACK01 transport directly from the existing read-only source, without
an expanded host copy; this is a separate future allocated read/package stage,
not completed here. Current archive extraction writes the full17.7GB target base
once and is not sparse-preserving. Bind all original file hashes/lock and raw
armv7 package, verify actual target capacity, and retain the real import receipt.
The handoff tar is not itself ITPACK01. No new firmware asset is requested.

After allocated import is terminal and the new record is clean, start one fresh
unused worker normally (allow_dirty0), with fixed metrics armed through the
reviewed diagnostic harness. Do not copy/modify the preserved n72 record or clear
dirty state. Use the existing bounded Android supervisor style with one original
clock:600s inspected readiness and840sStop/900shard limit for the guest attempt;
if import is included in that allocation, it consumes the same900s. If startup
is too slow, retain a bounded failure rather than extending/resetting the clock.
Keep Activity/Surface foreground through readiness. Pair native raw PNG and
Android Surface image with coherent producer/copied sequence, publication time,
backlight/sleep and presentation phase/window/error receipts. API2 plus current
nonzero pixels is a diagnostic milestone; model inspection must identify the
guest scene and its changed behavior before guest rendering acceptance.

## Completion and failure receipt

Require actual API2 shader-path participation, successful relevant guest-memory
writeback, fresh recognizable guest pixels and a visibly changed guest scene on
the exact target/producer. Retain before/after command errors and copied/presented
sequence/time; a coherent static awake frame is allowed, no perpetual advancement
requirement. Require normal confirmed Stop/dirty absent/storage healthy/exit0 and
all owned processes gone separately. Compare teardown make-current failures with
the old run without declaring their cause fixed merely because the mock passed.

If any boundary fails, keep its original images/log/error/status/state and report
the last proven boundary. Neither provider ownership, ALIVE, shader-path counters,
nonzero bytes, clean Stop nor a host triangle substitutes for visible guest output.
Linux and Poco qualify independently. A later4KiB/16KiB AVD n81 run remains a
separate app-UID/packaged-provider target allocation.
