# AP-wake input preservation acceptance

Preserving a manually held Home/Power button through an AP hibernate reset must
not extend its original virtual release deadline or change other input ownership.
It is distinct from cold reset, explicit Cancel, Pause/focus loss and Stop. This
contract does not change Android's production100ms host pulse.

The existing button slice checks batching, long holds and repeated presses;
the PMU hibernate slice checks retained PMU events versus cold reset. Neither
alone couples GPIO reset, UI reset and the original manual release timer. The
QMP sequence test also stubs reset registration. Use the actual production reset
functions for the additional cases, not a rewritten preservation algorithm.

| Scenario | Required observation |
| --- | --- |
| DOWN and queued UP at virtual1000; AP reset at1050 | Manual pin remains held, `pressed_at=1000`, pending deadline1100 unchanged |
| GPIO hold before UI reset, then reverse order | Same result; AP classification remains valid through all hold callbacks and clears at exit |
| Advance to1099, then1100 | Still held before deadline; ordinary timer releases pin and ownership at deadline |
| DOWN only, AP wake, later host UP | No invented pending release; later UP uses original press time and normal minimum |
| Explicit cold reset, then AP reset without new DOWN | Input released; no stale cached DOWN reappears |
| Explicit full Cancel while AP flag was set | All contacts/buttons/motion released and manual pending timers deleted |
| Timed automation held a button when AP reset begins | Automation cancels/releases; a high pad alone is not manual ownership |
| Touch/motion active alongside manual button | AP reset preserves only eligible button ownership; contacts/motion still clear |
| Volume and legacy-reset environment | Actual keycode mapping remains active-low; released volumes do not become held |
| Other boards and Apple builds | Existing reset/cancel behavior remains unchanged |

A private coupled slice draft uses production `ios_button_bh`, release/reset
wrappers and GPIO record/hold/exit functions through `tests/slice/run.sh`. Its
boundary stubs do not test the full keycode-to-PMU path, actual sequence body,
reset phase registration or guest delivery. The draft was not compiled by the
product lane; preserve the owner's actual compiler/result receipts separately.

The target trace must pair an actual production button DOWN with an AP wake,
then guest GPIO/PMU reads showing the pin retained across reset and balanced UP
at the unchanged virtual deadline. Distinguish host admission/uptime from guest
virtual timestamps. Use observed guest reads; do not fabricate a dwell result
from delayed Android UP or replayed frame counters. A reset reason/latch must be
one-shot, not a mode that makes a later cold reset preserve input.

Retain foreground/focus/Pause/Stop and Surface ownership observations around the
attempt. Surface destruction, focus loss, backgrounding, owned-contact resize
and explicit cancellation can legitimately release buttons; unowned resize and
attach alone should not. Instrumentation completion can terminate the app, so
hold the resumed activity until trace and raw captures settle. Pad/timer success
is a mechanism result; actual wake still requires guest backlight, new producer
frames and inspected guest response. Neither unit success nor an alive agent
proves a visible wake, clean halt or durable recovery.
