#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Admit a full300s Pixel observation only after Start and a bounded clock read."""
import argparse
import decimal
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time
from diagnostic_control import PACKAGE, SERIAL, integer, make_admission, validate_binding


def public_device_clock(adb, binding, output, deadline, now=time.monotonic_ns, run=subprocess.run):
    """Read global BOOTTIME as shell, bracketed by the original Android boot ID."""
    argv=[adb,'-s',SERIAL,'exec-out','cat','/proc/sys/kernel/random/boot_id',
          '/proc/uptime','/proc/sys/kernel/random/boot_id']
    before=now()
    if before>=deadline:raise TimeoutError('Original device-clock budget expired')
    try:r=run(argv,capture_output=True,timeout=min(1.5,(deadline-before)/1e9))
    except subprocess.TimeoutExpired as error:r=subprocess.CompletedProcess(argv,124,error.stdout or b'',error.stderr or b'')
    after=now()
    receipt={'argv':argv,'hostBeforeNs':before,'hostAfterNs':after,'exit':r.returncode,
        'stdout':r.stdout.decode(errors='replace'),'stderr':r.stderr.decode(errors='replace'),
        'stdoutHex':r.stdout.hex(),'stderrHex':r.stderr.hex(),'boundBootId':binding['bootId'],
        'workerPid':binding['workerPid'],'clockReadContext':'adb shell; global BOOTTIME',
        'guestAcceptancePassed':False}
    with Path(output).open('x') as stream:stream.write(json.dumps(receipt,indent=2)+'\n')
    lines=r.stdout.decode(errors='replace').splitlines()
    if r.returncode or r.stderr or after>deadline or len(lines)!=3 or lines[0]!=binding['bootId'] or lines[2]!=binding['bootId']:
        raise ValueError('Public device clock/boot identity unavailable or changed')
    fields=lines[1].split()
    if len(fields)!=2 or any(not re.fullmatch(r'\d+\.\d{1,3}',v) for v in fields):
        raise ValueError('Unknown public /proc/uptime precision')
    text=fields[0];digits=len(text.split('.')[1])
    return {'hostBeforeNs':before,'hostAfterNs':after,
        'deviceElapsedMs':int(decimal.Decimal(text)*1000),'quantizationMs':10**(3-digits),
        'bootId':binding['bootId']}


def admit(adb, attempt, token, now=time.monotonic_ns, run=subprocess.run):
    if not re.fullmatch(r"[0-9a-f]{32}", token):
        raise ValueError("Invalid attempt token")
    out = Path(attempt)
    resolved = out.resolve()
    if any((p / ".git").exists() for p in (resolved, *resolved.parents)):
        raise ValueError("Private admission output must be outside Git")
    raw_origin = (out / "clock-context.json").read_bytes()
    context = json.loads(raw_origin)
    origin = integer(context["originNs"], 1)
    if origin != context["supervisorStartTicks"] * 1_000_000_000 // context["clockTicksPerSecond"]:
        raise ValueError("Original supervisor clock differs")
    origin_token = hashlib.sha256(raw_origin).hexdigest()
    wait_deadline = min(now() + 18_000_000_000, origin + 810_000_000_000)
    prefix = [adb, "-s", SERIAL, "exec-out", "run-as", PACKAGE]
    folder = "files/pixel-boot-" + token
    ready = None
    commands = []
    while now() < wait_deadline:
        before = now()
        r = run(prefix + ["cat", folder + "/ready.json"], capture_output=True,
                timeout=min(1.5, (wait_deadline - before) / 1e9))
        after = now()
        commands.append({"argv": prefix + ["cat", folder + "/ready.json"], "beforeNs": before,
                         "afterNs": after, "exit": r.returncode, "stderr": r.stderr.decode(errors="replace")})
        try:
            ready = json.loads(r.stdout)
        except (ValueError, UnicodeError):
            time.sleep(.1)
            continue
        if r.returncode != 0:
            raise ValueError("Ready read failed")
        break
    if ready is None:
        raise TimeoutError("No timely actual after-Start ready receipt")
    if ready.get("attemptToken") != token or ready.get("originToken") != origin_token:
        raise ValueError("Ready receipt belongs to another attempt/origin")
    binding = {key: ready[key] for key in ("appUid", "workerPid", "workerStartTicks", "bootId", "recordId", "attemptToken", "originToken")}
    binding["originNs"] = origin
    validate_binding(binding)
    paths = [out/name for name in ("recovery-record-bound.json", "normal-record-bound.json") if (out/name).exists()]
    if len(paths) != 1: raise ValueError("Exactly one pinned preflight record/provider binding required")
    recovery_path = paths[0] if paths else out/"recovery-record-bound.json"
    if ready.get("coldRecoveryUsed") is True or paths:
        from bind_recovery import match_ready
        prior = json.loads(recovery_path.read_text())
        if prior["originNs"] != origin:
            raise ValueError("Recovery binding original origin differs")
        match_ready(prior, ready)
        binding.update(priorDirty=prior["priorDirty"], coldRecoveryUsed=prior["coldRecoveryUsed"])
        binding.update({key:ready[key] for key in ('graphicsProviderRequested','graphicsProviderObserved','graphicsProviderFixed')})
    # Publish before budget calculation so early rejection still has scoped capture.
    pending = out / ".pixel-bound.pending"
    pending.write_text(json.dumps(binding, indent=2) + "\n")
    pending.replace(out / "pixel-bound.json")
    for argv, expected in ((["id", "-u"], str(binding["appUid"]).encode()),
                           (["cat", "/proc/sys/kernel/random/boot_id"], binding["bootId"].encode()),
                           (["cat", "files/library/selected"], binding["recordId"].encode())):
        r = run(prefix + argv, capture_output=True, timeout=1.5)
        if r.returncode != 0 or r.stdout.strip() != expected:
            raise ValueError("Current UID/boot/record identity differs")
    r = run(prefix + ["cat", f"/proc/{binding['workerPid']}/stat"], capture_output=True, timeout=1.5)
    fields = r.stdout.decode().rsplit(")", 1)[1].split()
    if r.returncode != 0 or int(fields[19]) != binding["workerStartTicks"]:
        raise ValueError("Worker replaced after Start")
    r = run(prefix + ["cat", f"/proc/{binding['workerPid']}/status"], capture_output=True, timeout=1.5)
    uid = re.search(rb"^Uid:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)$", r.stdout, re.M)
    if r.returncode != 0 or not uid or set(map(int, uid.groups())) != {binding["appUid"]}:
        raise ValueError("Worker UID differs after Start")
    r = run(prefix + ["cat", f"/proc/{binding['workerPid']}/cmdline"], capture_output=True, timeout=1.5)
    if r.returncode != 0 or r.stdout.rstrip(b"\0") != (PACKAGE + ":engine").encode():
        raise ValueError("Worker role differs after Start")
    bracket = public_device_clock(adb,binding,out/'after-start-device-clock.json',
        origin+810_000_000_000,now=now,run=run)
    gate = make_admission(binding, bracket)
    encoded = json.dumps(gate, separators=(",", ":"))
    remote = (f"printf '%s' {shlex.quote(encoded)} > {folder}/.admission.pending && "
              f"mv {folder}/.admission.pending {folder}/admission.json; "
              "lt_gate_exit=$?; printf '\\n__LT_GATE_EXIT__=%s\\n' \"$lt_gate_exit\"")
    remaining = (gate["hostObservationLimitNs"] - now()) / 1e9
    if remaining < 300.5:
        raise ValueError("Admission delivery no longer fits full interval")
    r = run([adb, "-s", SERIAL, "shell", shlex.join(["run-as", PACKAGE, "sh", "-c", remote])],
            capture_output=True, timeout=min(.5, remaining - 300))
    if r.returncode != 0 or not re.search(rb"\n__LT_GATE_EXIT__=0\n$", r.stdout):
        raise ValueError("Device admission publication failed")
    receipt = {"gate": gate, "commands": commands, "gatePublishedHostNs": now(),
               "guestAcceptancePassed": False, "originalClockUnchanged": True}
    (out / "pixel-admission.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--adb", required=True)
    p.add_argument("--attempt", required=True)
    p.add_argument("--token", required=True)
    a = p.parse_args()
    print(json.dumps(admit(a.adb, a.attempt, a.token), indent=2))
