#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture a new allocated Pixel supervisor's original process clock exactly once."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time


def capture(attempt, pid, session, avd):
    out = Path(attempt).resolve()
    wrapper = Path(__file__).with_name("pixel_supervisor.py").resolve()
    before_fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    args = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
    if os.fsencode(wrapper) not in args:
        raise ValueError("Wrong supervisor executable")
    for option, value in ((b"--output", os.fsencode(out)), (b"--avd", os.fsencode(avd)),
                          (b"--expected-page-bytes", b"16384")):
        if option not in args or args[args.index(option) + 1] != value:
            raise ValueError("Supervisor allocation differs")
    if any((p / ".git").exists() for p in (out, *out.parents)):
        raise ValueError("Private attempt must be outside Git")
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    if fields[19] != before_fields[19]:
        raise ValueError("Supervisor PID reused during origin capture")
    ticks, hz = int(fields[19]), os.sysconf("SC_CLK_TCK")
    origin_ns = ticks * 1_000_000_000 // hz
    context = {"schema": 2, "supervisorPid": pid, "supervisorStartTicks": ticks,
               "clockTicksPerSecond": hz, "originNs": origin_ns,
               "conservativeProcessOriginMonotonic": origin_ns / 1e9,
               "persistentExecSession": session, "avd": avd,
               "uiGateNs": origin_ns + 240_000_000_000,
               "readbackGateNs": origin_ns + 810_000_000_000,
               "stopGateNs": origin_ns + 840_000_000_000,
               "hardGateNs": origin_ns + 900_000_000_000,
               "capturedAtNs": time.monotonic_ns(), "clockOriginReset": False,
               "wrapperSha256": hashlib.sha256(wrapper.read_bytes()).hexdigest()}
    target = out / "clock-context.json"
    with target.open("x") as stream:
        stream.write(json.dumps(context, indent=2) + "\n")
    return context


def attach_session(attempt, pid, session, avd):
    """Attach the returned tool handle without rewriting the original clock."""
    out = Path(attempt).resolve()
    raw = (out / "clock-context.json").read_bytes()
    context = json.loads(raw)
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    argv = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
    if any((p / ".git").exists() for p in (out, *out.parents)):
        raise ValueError("Private attempt must be outside Git")
    for option, value in ((b"--output", os.fsencode(out)), (b"--avd", os.fsencode(avd)),
                          (b"--expected-page-bytes", b"16384")):
        if option not in argv or argv[argv.index(option) + 1] != value:
            raise ValueError("Supervisor allocation differs")
    if (context["supervisorPid"] != pid or context["supervisorStartTicks"] != int(fields[19])
            or context["avd"] != avd or os.fsencode(Path(__file__).with_name("pixel_supervisor.py").resolve()) not in argv
            or context["originNs"] != int(fields[19]) * 1_000_000_000 // os.sysconf("SC_CLK_TCK")):
        raise ValueError("Original supervisor clock/identity differs")
    if Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19] != fields[19]:
        raise ValueError("Supervisor PID reused during session attachment")
    receipt = {"persistentExecSession": session, "supervisorPid": pid,
               "supervisorStartTicks": context["supervisorStartTicks"],
               "clockContextSha256": hashlib.sha256(raw).hexdigest(),
               "attachedAtNs": time.monotonic_ns(), "clockOriginReset": False}
    with (out / "exec-session.json").open("x") as stream:
        stream.write(json.dumps(receipt, indent=2) + "\n")
    return receipt


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--attempt", required=True)
    p.add_argument("--supervisor-pid", type=int, required=True)
    p.add_argument("--exec-session", type=int, required=True)
    p.add_argument("--avd", required=True)
    p.add_argument("--attach-exec-session", action="store_true")
    a = p.parse_args()
    action = attach_session if a.attach_exec_session else capture
    print(json.dumps(action(a.attempt, a.supervisor_pid, a.exec_session, a.avd), indent=2))
