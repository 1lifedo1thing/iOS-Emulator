# Android PCM across pause and focus changes

Flushing the native capture ring/domain queue cannot revoke PCM already returned
to the Android reader. The reader now captures a sink ticket before
`Native.audio`, retains it through the format lookup, then checks the ticket
and current pause/focus/stop admission under the same monitor as AudioTrack
write. A pause closes/advances that epoch and pauses/flushes the retained track;
resume reopens a new epoch after `play`. Reads started while closed have ticket0
and cannot become eligible after resume. Repeated play with already held focus
does not invalidate an otherwise current read.

Explicit user pause and focus loss publish the sink fence before native pause
admission. Owner-scoped backgrounding holds the sink monitor across the
authoritative native ownership check/pause admission and epoch close/track flush.
Stale rejection leaves the epoch, track and local pause flags untouched; no sink
write can cross successful native admission before its fence commits. A write
already holding the monitor finishes before native background admission and
the pause fence; the subsequent flush handles its queued platform data. Native
PCM read/format calls never hold the sink monitor. The background native call
does: JNI does not call back into Java/audio, and the reader releases its native
guard before acquiring the sink monitor. Display claim/release remain outside
the lifecycle monitor; no window conversion/release is newly serialized here.
If a newer View claims after accepted background admission, its focus/Resume
uses the lifecycle monitor and reopens audio after the old accepted transition
finishes. Focus abandonment occurs outside the sink monitor. Stop keeps its independent
deadline path and stopping flag; this patch adds no audio lock to that path.

The existing play-before-new-focus publication fix remains: retained track
`play`, epoch reopen and focus assignment share the guard. Runtime's internal
pause barrier must separately prevent fresh domain packets until actual paused
then running/capture-restart acknowledgement. Android does not treat return
from asynchronous native Resume as proof of that acknowledgement; until the
native queue permits fresh packets, reads yield no PCM to write.

`audio.packetGeneration` and `packetAdmissionOpen` report the sink's admission
state, not guest timestamps or audible playback. `discardedReadPackets` and
`discardedReadSamples` count positive returned buffers rejected before writing;
they are not claims about their actual audible content. Pulled sample counters
still count actual native reads, including discarded buffers. Written/played
counters retain their distinct meaning.

Six source regressions exercise the production epoch/monitor: a read blocked
across complete pause/resume cannot write, a closed read cannot become eligible,
and a blocked sink write prevents pause publication until that critical section
finishes. Background regressions additionally block native admission while a
reader contends with the actual sink monitor, verify stale/failed admission
leaves a current packet eligible, and block a newer owner's Resume on the actual
lifecycle monitor until accepted old backgrounding has closed the epoch.
They use symbolic sink writes, not fake PCM or a guest audio pass.
Compilation/target validation remains required. No new Portable or JNI API,
sample format, focus policy, renderer or gyro behavior is introduced here.
