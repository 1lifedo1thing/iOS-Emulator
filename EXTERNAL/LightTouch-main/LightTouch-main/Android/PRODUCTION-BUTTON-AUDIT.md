# Production button source audit

Candidate18's Compose Home/Power buttons call `MainActivity.pulse(0/1)`.
The callback captures the current `IEngine`, sends DOWN, then schedules UP to
that same worker through the main Handler after100ms host uptime. It does not
call cancellation or reset. There are no `KotlinButtonInputs` or
`SessionController` classes in this Android implementation.

`EngineService.button` forwards to `Native.button` and counts admission or
rejection. JNI calls `lt_runtime_button`, which queues the engine's button BH.
For n72 this reaches `ios_press` and `ipod_touch_press_button`, then the same
board key handler used by console keyboard input. Fixed0 means Home and1 Power.
The engine applies DOWN under guest virtual time; normal UP schedules release
at `max(now, pressed_at+100ms)`. Host Handler time and admission counters do not
measure that virtual dwell or guest consumption.

An unowned `GuestDisplay.surfaceChanged` invalidates local touch continuation
without global cancellation. `surfaceCreated`, service `surface`, and JNI
`surface` attach/replace the presentation window without cancellation or guest
reset. Actual surface destruction/clear, window focus loss, activity background,
Pause/Resume, Stop, and resize with owned contacts retain full cleanup. A Surface
generation counter alone cannot distinguish these paths or prove cancellation.

The retained candidate18 physical-Power trace records DOWN, then a boot-ROM GPIO
read with the power bit clear approximately45.058ms later in guest virtual time.
No UP edge is logged in that scoped trace. The normal100ms virtual UP minimum
does not explain that earlier clear by itself. AP/reset GPIO ownership belongs
to the engine model audit; this source review does not infer an unlogged Android
cancellation or a complete wake cause. Firmware/backend own that diagnosis and
any model correction. No production timing or control patch follows from this
Android audit.
