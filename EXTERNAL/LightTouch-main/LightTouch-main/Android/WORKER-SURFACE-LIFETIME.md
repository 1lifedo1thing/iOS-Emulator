# One UI Surface per engine worker

Production24 retained a Surface image from the retired worker after recovery
started a different worker. The new producer reached frame53/awake/agent Lock,
while presentation had2posts,7500window-lock failures/-22 and no frame-copy or
geometry failures. Saved UI images still show the earlier power-off scene.
This establishes a presentation failure. It does not identify the platform's
exact reason for returning EINVAL or a guest framebuffer/TCG defect.

The source previously retained one AndroidView/GuestDisplay across worker Binder
replacement. Its mutable engine closure and immediate onServiceConnected callback
could attach the existing holder Surface/BufferQueue to the new process. A new
ANativeWindow reference, native generation or View lease does not construct a new
SurfaceView/BufferQueue. In particular, a producer's unconfirmed disposal did not
cause the UI-owned Surface to be replaced.

WorkerGuestSurface now keys its AndroidView by strict Binder incarnation identity,
with reuse disabled. A new worker constructs a new View/holder; same-worker status
recomposition keeps the existing View. Each View captures its worker, and checks
that it is still current before attachment, touch admission or delivering an error.
Cleanup retains the captured worker/token after replacement, so it can release
A's resources without resolving to B. An
old callback cannot use a mutable closure to acquire the new worker's lease before
Compose disposes the old View. Existing native owner/request/generation/ref rules
remain authoritative. Release retires only that View; MainActivity clears its
pointer only if it still names the released View. Native window calls and retained
window lifetime remain unchanged.

The pure identity test covers referential worker identity even if objects compare
equal. The opt-in WorkerGuestSurfaceTest uses the production composable with fake
workers on actual Android SurfaceViews. It requires a stopped unused real service
and the exact installed main APK hash, retains same-worker View identity, replaces
the View/holder on worker change, checks a valid new Surface, and invokes late old
callbacks before and after replacement. It asserts old cancel/detach target A,
disposal releases A exactly once and no old callback reaches B. It does not kill a process, run a guest,
prove BufferQueue internals or qualify recovery from an actual dead CPU producer.
These tests are staged UNRUN until allocation.

A later allocated target check must repeat the actual normal Stop/fresh-worker
restart and separately the already-retained failed Stop/explicit recovery route
without deleting state. Bind the exact main/engine/source/old-new PID+Binder
incarnations. Capture a current recognizable guest raw frame and UI Surface before
Stop; after the new worker becomes ready, capture its new raw image and actual UI
Surface with coherent source/copied/presented sequence/time, window generation,
lock/post/geometry/copy error deltas and awake/backlight state. Inspect that the UI
shows the new worker's changed scene rather than the old power-off frame. Retain
normal confirmed Stop separately. No recovery/visible target pass follows from
this source fix or synthetic Surface test.
