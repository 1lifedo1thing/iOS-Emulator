# Android display ownership

This fixes a source-permitted ordering bug, not a proven explanation of any
retained guest image. Previously any old View could globally cancel input and
clear a newer View's window. A native arrival counter protected slow conversion
results but did not identify who owned the window.

An Activity must be started, resumed and actually focused to select the
process-local UI owner. An unfocused `onStart` cannot displace a focused owner.
A legitimate focus return while still started can reacquire authority without
another `onStart`. Only its current focused `displayView` may acquire a
worker-issued display lease; attaching pixels additionally requires a valid Surface. Created/changed/reconnect callbacks reuse that
lease for the same UI ownership epoch. A genuinely new ownership epoch replaces
the stored old lease, including a same-worker A-to-B-to-A focus return. An
obsolete Activity/View cannot claim a new lease just because its
callback arrives late. A genuinely foregrounded Activity can take a new lease.
The View stores the issuing Binder with the token and always sends owned controls
through that captured worker. A new worker's coincident numeric token does not
validate an old binding.

Native attachment checks the lease under the existing control guard before
conversion and at commit. It also retains arrival ordering for overlapping
conversions from the same owner. Conversion and rejected/retired window reference
release remain outside that guard. A converted reference is adopted exactly once
or released exactly once. Presenter snapshots and the one-post-per-lock rule are
unchanged. No Service lock spans platform conversion or window release.

Detach, contact cancellation, touch admission and background pause validate the
lease inside the native guard. An old View's destroy/focus/resize/touch and an old
Activity's stop cannot affect the new owner through those paths. Local retired
View contact IDs are discarded without issuing an unconditional global cancel.
Reconnect routes through the View's eligibility and stored worker binding.
Surface destruction detaches pixels and invalidates pending conversions but
keeps Activity authority and the captured binding, so a following Activity stop
can still pause. Activity stop keeps the prior session/status pause condition,
then uses the owner-scoped background operation and retires the lease. Pre-start
Activity closure does not mark a
stopped worker paused or create a session error.

The native ledger tracks View-owned **admitted** contacts, not guest consumption.
Routine attach, duplicate callbacks and no-contact resize/detach do not cancel
an unrelated Home/Power pulse. A real held-contact handoff uses the existing full
cancel API; that API also releases hardware buttons. Failures remain actionable
native errors. Explicit user Pause/Stop, direct diagnostic input and worker audio
focus cleanup retain their global semantics. Pause admission and audio-focus
callbacks share a dedicated lifecycle monitor so focus gain cannot resume in
between native background pause and the user-paused flag update. Window claim,
conversion and reference release never enter that monitor; observations and
the independent shutdown watchdog do not wait on it. The lease does not reinterpret
shutdown confirmation, guest input application, audio focus policy or pixels.

Submitted touch IDs are captured from the existing runtime status immediately
after admission under the JNI guard, with a separate error buffer. Rejection
cleanup retains that request's owner and whether its pre/post batch could leave
contacts held. A retired owner's delayed failure cannot cancel the new owner or
replace its diagnostic; ignored IDs are retained in fixed telemetry counters.
Current-owner failures remain actionable. An empty batch needs no full release,
but a failed UP can require it even after the admitted-ID ledger became empty.
Successful full cancellation or terminal model success discharges that request's
cleanup obligation. Before handoff, detach or retirement, a pending owned
UP/CANCEL obligation is balanced even when the local admitted-ID ledger is empty.
A fresh status check first discharges an already successful release, preserving
an unrelated later button pulse. Legacy direct diagnostic input remains explicitly global.
Failed release keeps the old ledger/obligation and native authority retryable
while detaching its window; a new claim must admit that release before minting
the next owner. Admission of a queued cancellation is not proof of guest-wire
delivery; the helper models and source checks do not establish that delivery.

The phase telemetry from `121936c8` remains in this source. Its stage codes,
timestamps and window presence are diagnostic metadata, never guest acceptance.
Bitmap capture and status remain separate calls without an atomic PNG serial.

The pure JVM production-helper cases follow started/resumed/focus/paused/stopped
callbacks, including an unfocused overlap and focus return without another
onStart, a same-worker ownership-epoch change, numeric token reuse across
distinct workers and a bounded blocked-background/focus-gain interleaving.
The native production-helper cases cover delayed obsolete conversion, retired
controls, Surface destruction followed by background admission, same-worker
foreground return, duplicate requests, exact converted-reference release and empty versus
held-contact handoff. The existing synthetic View tests explicitly bind fake
ownership by reflection; they test View ID/resize/rejection behavior without a
real Android Surface or native admission. A fourth View flow test verifies
Surface destruction retains the captured authority for background pause and
subsequent retirement. No production test bypass is added.

Required validation after an allocated build: focused JVM suite, native helper
ASAN/UBSAN/Werror run, both ABI JNI syntax/build and updated synthetic View tests.
Then inspect actual repeated recreation/overlap on target with window/phase
metrics, current copied-versus-producer statuses and retained raw/Surface images.
Source/helper tests do not establish guest rendering or prove an earlier failure
was caused by a late callback. Existing failed receipts remain unchanged.

Release completion is tracked by contact ID. An admitted UP/CANCEL removes its
local held ID but retains uncertain release debt until the exact latest batch
reports success. Success clears only the ending IDs in that batch; overwritten
older requests remain uncertain and require cancellation before handoff or
detach. The debt set is bounded to 64 IDs; overflow remains conservative until
a full cancellation succeeds. This models admission/status bookkeeping, not
proof of backend or guest delivery of a cancellation.
