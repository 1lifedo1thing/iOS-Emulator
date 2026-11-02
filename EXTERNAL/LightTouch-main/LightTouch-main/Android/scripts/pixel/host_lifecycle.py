# SPDX-License-Identifier: GPL-2.0-or-later
"""Parent-only host termination observations; signal handlers never perform IO."""
import os
import signal
import time


def install(observations):
    owner = os.getpid()
    watched = (signal.SIGTERM, signal.SIGHUP)

    def reset_child():
        for number in watched:
            signal.signal(number, signal.SIG_DFL)

    def interrupted(number, frame):
        if os.getpid() != owner:
            # Defensive fallback if a fork did not run Python's fork hooks.
            signal.signal(number, signal.SIG_DFL)
            os.kill(os.getpid(), number)
            return
        # Repeated terminal signals cannot interrupt the bounded finally path;
        # the independent hard-deadline watchdog still uses SIGKILL.
        for watched_number in watched:
            signal.signal(watched_number, signal.SIG_IGN)
        observations.append({"signal": number, "signalName": signal.Signals(number).name,
                             "observedAtNs": time.monotonic_ns(), "supervisorPid": owner,
                             "senderKnown": False, "guestAcceptancePassed": False})
        raise KeyboardInterrupt("Host supervisor received " + signal.Signals(number).name)

    os.register_at_fork(after_in_child=reset_child)
    for number in watched:
        signal.signal(number, interrupted)
