# n81 guest GLES2 route audit

Source audit, 2026-10-14. This is a route and evidence requirement, not target
acceptance. Motion is deferred; no historical SDK or newly compiled guest probe
is needed for this route.

## Existing fixture and workload

The supplied n81ap/8C148 handoff uses the prepared kboot route, all eight read-only
NAND stores, NOR and identity, with the matching armv7 package family k48-ios4,
version1.1.24/serial26. It needs no hardware boot ROM. The verified fit report
and its actual Mac boot log identify the firmware's GLEngine.bundle and the
OpenGLES front end's ES1.1+2.0 support. No missing-engine asset has been found.
These Mac results do not qualify Linux or Android.

The existing Linux supplemental run is stronger route evidence: engine b88
initialized a guest-api=2 context, logged the first CoreAnimation surface attach,
and logged16 glDrawElements(ES2) offscreen shader-path draw attempts. This occurred during the
built-in SpringBoard/QuartzCore workload, without installing a GL application.
It proves that this supplied guest requests ES2 and reaches the shader draw path.
The logger can also describe a draw awaiting guest-page fault retry; these lines
do not independently establish that every attempt executed.
The run missed its briefly lit interval, captured an aged black640x960 frame,
and retained EGL_BAD_ACCESS at teardown. It does not prove visible ES2 output,
Home/input, or compatibility of a later engine.

Use the n81 built-in compositor first. Home/Lock and an inspected built-in app
transition give legitimate guest behavior while avoiding an SDK, guest probe,
network-dependent workload or third-party app acquisition. Each future target
must independently establish its actual API2 context and shader/draw path;
a Home image alone cannot establish those. The shipped it_gltest explicitly
uses initWithAPI:1 and fixed-function arrays. GLTest2 exists as source, but no
matching compiled GLTest2 artifact was found in the supplied guest tools/package;
it is unnecessary for this first route.

## Current bridge and providers

The front end enables API2 only when GLEngine.bundle exists (fe_es2), and passes
the requested API through EAGLContext -> GLESCreateGCWithAPI -> NEW_CONTEXT's
second word. The host keeps that API per context. Shader/program creation,
source, compile, link/use, generic attributes and array/indexed shader draws are
implemented in gles-host.c. Android native EGL creates a real ES2 context and
passes GLSL ES source unchanged. Linux uses a desktop compatibility context and
translates GLSL ES1.00 to GLSL1.20. A nonzero guest program selects gles_es2_draw.

The compiled Android EGL provider selected as angle-gl is distinct from the old
GLES_HOST_ANGLE prototype (the latter explicitly refuses API2). The current
public EGL dispatcher supplies both APIs. Poco should keep platform default;
AVD needs its independently verified packaged provider. Neither host triangle
readback nor verified provider owners closes guest rendering acceptance.

From b88 to b59, the EGL lifetime/native adapter files are unchanged; GLES host
changes add copied-pixel diagnostics. Therefore the old teardown BAD_ACCESS is
not known fixed. Regular calls refuse a failed make-current. The source followup now checks
EGL acquisition before context-owned GL deletion; a refusal preserves the
caller's unrelated current context and skips those deletion calls. Context
release still attempts cleanup/destruction; its native texture collector already
skips GL work after failed acquisition. The non-EGL/Mac cleanup path is unchanged.
This fixes an unsafe failure path without eliminating the observed binding
failure. Cross-thread current-context ownership is a plausible teardown
hypothesis, but the retained run has no thread-owner trace and does not establish
that cause. Keep this error separate from CPU/storage shutdown confirmation and
verify the next exact producer's error delta.

`egl-context-cleanup.c` includes the production deletion helper with an unrelated
current-context model. It checks failed acquisition causes no deletion or unbind,
while success deletes the exact owned names and unbinds; the separate non-EGL
build retains the existing successful path. These are source-policy regressions,
not driver/thread or guest acceptance. Compilation is pending an allocation.
No new thread instrumentation is included: a future bounded failure receipt may
record caller/last successful bind thread and context identity if needed.

The EGL release followup retains one safety reference if restoration/unbind
fails while the wrapper remains current. Texture cleanup runs once. A successful
replacement/unbind retires the deferred previous wrapper only after native EGL
changes the binding; an explicit release retry can also finish it. Retention is
limited to the calling thread's still-current released wrapper, rather than
accumulating one orphan for every release failure. Release does not restore a
retired previous wrapper that its acquisition just reclaimed. This is an EGL
adapter lifetime fix; Mac CGL/EAGL paths are unchanged. `egl-context-release.c`
includes the production EGL leaf against injected failures and covers retry,
shared restoration,32 failure/switch cycles and a retired previous binding.
It does not prove the retained n81 failure's cause or actual target teardown.

## Minimal next allocated target runs

Linux can use the supplied original base directly, with a separate small writable
NOR/overlay/state and a fresh checked armv7 offer. Reuse neither Mac endpoints
nor its good-verdict metadata. Bind exact new engine/frontend/package/source
hashes. Full native admission hashes about17.7GB; account for that measured stage
separately from guest readiness within an external absolute run bound. Do not
make another full base copy for Linux or waive native integrity checks.

Poco cannot read the host's base path. Stage one legitimate target-owned base
through the existing prepared-base import contract, with the same full logical
files/hashes. A compact ITPACK01 transport avoids an extra full host directory
copy and a full Android input staging directory; extraction still writes the
full17.7GB target base with the current importer. The supplied handoff tar is not
ITPACK01. Its conversion has not been run here. Existing directory UI import
first copies the tree and then imports it again, so it needs transient duplicate
space and is inappropriate under the no-extra-copy constraint. Do not silently
substitute a smaller fixture, link an inaccessible host base, or claim the
current archive importer preserves holes. Check actual free target capacity,
prepare a verified compact transport without another expanded host copy, and
bind its manifest to the existing verified source before a later allocation.

On both targets retain actual guest-api=2, matching context identity and actual
shader/draw evidence; bounded context/call trace and copied/CA pixel metrics can
supply this without pixel overrides. Capture while backlit/awake with coherent
positive producer/copied sequence and same-clock fresh publication, inspect the
actual640x960 scene, perform an inspected transition/input, and capture its changed
pixels. Retain all compile/link/draw, make-current and writeback failures plus
the original raw captures. A clean Stop receipt is a separate required result.
No new target run, asset conversion, import, SDK build or base mutation occurred
in this audit.

## Upstream MBX model relevance

103d076b09c30eabe39ce3e73a37d21b10693d41 models MBX MMU, 2D copies/fills and
limited3D blits for the original iPhone/iPod and n72. It does not implement the
tile accelerator; guest applications retain the GL bridge. Its source changes
are MBX/model/board/tests and contain no GLES host or SGX implementation change.
It is not an ancestor of b59. Its reported tour sends no MBX2D/3D work with the
GL front end; the new model matters for stock compositing with the shim disabled.
It cannot supply missing n81 GLES2 evidence or be assumed to fix the current
host bridge. App-side CA/LK MBX policy changes and fixture preparation semantics
must be reviewed together before any separate upstream adoption. Its Mac
performance figures are not Android/Linux measurements.
