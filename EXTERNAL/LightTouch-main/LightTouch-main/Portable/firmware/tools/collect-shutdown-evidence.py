#!/usr/bin/env python3
"""Root-operated, bounded read-only ADB metadata collection; never request Stop."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
import uuid


def require(condition, message):
    if not condition:
        raise ValueError(message)


def write_json(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")
    path.chmod(0o600)


def uptime_interval(data):
    """Bracket milliseconds, respecting the precision actually printed by procfs."""
    first = data.decode("ascii").split()[0]
    match = re.fullmatch(r"([0-9]+)\.([0-9]{1,9})", first)
    require(match is not None, "invalid device /proc/uptime")
    fraction = match[2]
    nanoseconds = int(match[1]) * 1000000000 + int(fraction.ljust(9, "0"))
    quantum_ns = 10 ** (9 - len(fraction))
    return {"printed": first, "lowerMs": nanoseconds // 1000000,
            "upperMs": (nanoseconds + quantum_ns - 1) // 1000000,
            "precisionNs": quantum_ns}


def remote_result(stdout, marker):
    prefix = b"\n" + marker.encode("ascii") + b"="
    payload, separator, suffix = stdout.rpartition(prefix)
    require(separator and re.fullmatch(rb"[0-9]+\n", suffix), "remote exit sentinel missing/truncated")
    return payload, int(suffix.strip())


class Collector:
    def __init__(self, out, serial, package, adb="adb", budget=60):
        self.out, self.serial, self.package, self.adb = out, serial, package, adb
        self.deadline = time.monotonic() + budget
        self.reads = []

    def read(self, name, script, app=True, allowed=(0,)):
        remaining = self.deadline - time.monotonic()
        require(remaining > 0, "collection budget exhausted")
        marker = "__LT_READ_EXIT_" + uuid.uuid4().hex
        command = ("run-as " + shlex.quote(self.package) + " sh -c " + shlex.quote(script)) if app else (
            "sh -c " + shlex.quote(script))
        command += '; lt_capture_rc=$?; printf "\\n' + marker + '=%s\\n" "$lt_capture_rc"; exit "$lt_capture_rc"'
        argv = [self.adb, "-s", self.serial, "shell", "-T", command]
        started = time.monotonic_ns()
        try:
            result = subprocess.run(argv, capture_output=True, timeout=min(10, remaining))
        except subprocess.TimeoutExpired as error:
            (self.out / (name + ".stdout")).write_bytes(error.stdout or b"")
            (self.out / (name + ".stderr")).write_bytes(error.stderr or b"")
            write_json(self.out / (name + ".json"), {"argv": argv, "timedOut": True,
                       "hostMonotonicBeforeNs": started, "hostMonotonicAfterNs": time.monotonic_ns()})
            raise ValueError("bounded ADB read timed out: " + name) from error
        (self.out / (name + ".stdout")).write_bytes(result.stdout)
        (self.out / (name + ".stderr")).write_bytes(result.stderr)
        try:
            data, remote_exit = remote_result(result.stdout, marker)
        except ValueError:
            write_json(self.out / (name + ".json"), {"argv": argv, "adbExit": result.returncode,
                       "remoteExit": None, "hostMonotonicBeforeNs": started,
                       "hostMonotonicAfterNs": time.monotonic_ns()})
            raise
        receipt = {"argv": argv, "adbExit": result.returncode, "remoteExit": remote_exit,
                   "hostMonotonicBeforeNs": started, "hostMonotonicAfterNs": time.monotonic_ns(),
                   "payloadSha256": hashlib.sha256(data).hexdigest()}
        write_json(self.out / (name + ".json"), receipt)
        self.reads.append(receipt)
        require(result.returncode in (0, remote_exit), "ADB transport failed: " + name)
        require(remote_exit in allowed, f"remote read failed: {name} exit={remote_exit}")
        return data, remote_exit

    def file(self, name, path, app=True):
        # A metadata-only size cap also bounds target stdout, never a NAND read.
        data, _ = self.read(name, "head -c 262145 " + shlex.quote(path), app)
        require(len(data) <= 262144, "metadata exceeds bound: " + name)
        return data

    def clock(self, name):
        return uptime_interval(self.file(name, "/proc/uptime", app=False))

    def boot(self, name):
        value = self.file(name, "/proc/sys/kernel/random/boot_id", app=False).decode("ascii").strip()
        require(re.fullmatch(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", value), "boot ID unavailable/invalid")
        return value


def validate_spec(spec):
    require(spec.get("schema") == 1 and type(spec.get("schema")) is int, "invalid expectation schema")
    for key in ("workerPid", "workerUid", "shutdownGraceMs"):
        require(type(spec.get(key)) is int and spec[key] > 0, "invalid expectation integer: " + key)
    require(spec.get("baseKind") in ("generated-stock", "reference-fixture"), "declare base kind")
    require(re.fullmatch(r"[0-9a-f]{32}", spec.get("recordId", "")), "invalid record ID")
    require(type(spec.get("artifactPins")) is dict, "artifact pins missing")
    for obj, keys in ((spec, ("baseSha256", "bootromSha256", "guestPackageSha256")),
                      (spec["artifactPins"], ("mainApkSha256", "jniSha256", "engineSha256"))):
        for key in keys:
            require(type(obj.get(key)) is str and re.fullmatch(r"[0-9a-f]{64}", obj[key]), "invalid pin: " + key)


def record_metadata(c, spec, prefix):
    root = "files/library/records/" + spec["recordId"]
    selected = c.file(prefix + "-selected", "files/library/selected").decode("ascii").strip()
    require(selected == spec["recordId"], "selected record mismatch")
    record_bytes = c.file(prefix + "-record", root + "/record.json")
    lock_bytes = c.file(prefix + "-lock", root + "/base/device.lock.json")
    state_pin = c.file(prefix + "-state-pin", root + "/state/.base-identity").decode("ascii").strip()
    record = json.loads(record_bytes)
    require(record.get("id") == spec["recordId"], "record metadata mismatch")
    for expected, actual in (("baseSha256", "base_sha256"), ("bootromSha256", "bootrom_sha256"),
                             ("guestPackageSha256", "guest_package_sha256")):
        require(record.get(actual) == spec[expected], "record pin mismatch: " + actual)
    require(hashlib.sha256(lock_bytes).hexdigest() == spec["baseSha256"] and state_pin == spec["baseSha256"],
            "base lock or writable-state pin mismatch")
    return record_bytes, lock_bytes


def before(c, spec):
    boot_start = c.boot("before-boot-start")
    pid = spec["workerPid"]
    status = c.file("before-worker-status", f"/proc/{pid}/status").decode("ascii")
    require(re.search(rf"(?m)^Pid:\s+{pid}\s*$", status), "owning PID status mismatch")
    uid = re.search(r"(?m)^Uid:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$", status)
    require(uid and all(int(value) == spec["workerUid"] for value in uid.groups()), "owning UID status mismatch")
    cmdline = c.file("before-worker-cmdline", f"/proc/{pid}/cmdline")
    require(cmdline.split(b"\0")[0] == (c.package + ":engine").encode(), "not the production engine worker")
    process_stat = c.file("before-worker-stat", f"/proc/{pid}/stat")
    tail = process_stat.rsplit(b")", 1)[-1].split()
    require(len(tail) > 19 and tail[19].isdigit(), "worker start token unavailable")
    record, lock = record_metadata(c, spec, "before")
    (c.out / "record.json").write_bytes(record)
    (c.out / "device.lock.json").write_bytes(lock)
    clock = c.clock("before-ready-uptime")
    require(c.boot("before-boot-end") == boot_start, "device rebooted during before collection")
    write_json(c.out / "before.json", {"schema": 1, "serial": c.serial, "package": c.package,
               "spec": spec, "bootId": boot_start, "readyDeviceClock": clock,
               "workerStartTicks": int(tail[19]), "artifactPinsSource": "integrator supplied; not sampled here",
               "hostMonotonicClockUsedForReceiptComparison": False})
    return {"status": "BEFORE_METADATA_CAPTURED", "requestStop": False,
            "next": "Root may request Stop separately, then run after into another new directory"}


def after(c, before_dir):
    prior = json.loads((before_dir / "before.json").read_text())
    spec = prior["spec"]
    validate_spec(spec)
    require(c.serial == prior["serial"] and c.package == prior["package"], "before collector identity mismatch")
    require(c.boot("after-boot-start") == prior["bootId"], "device boot changed; clocks cannot be joined")
    record, lock = record_metadata(c, spec, "after")
    require(record == (before_dir / "record.json").read_bytes() and lock == (before_dir / "device.lock.json").read_bytes(),
            "record or lock changed between phases")
    (c.out / "record.json").write_bytes(record)
    (c.out / "device.lock.json").write_bytes(lock)
    pid = spec["workerPid"]
    receipt = c.file("after-terminal", f"files/worker-shutdown-{pid}.json")
    (c.out / f"worker-shutdown-{pid}.json").write_bytes(receipt)
    path = f"files/worker-shutdown-deadline-{pid}.json"
    script = "test -d files && test -r files && test -x files || exit 2; "
    script += f"if test -e {path} || test -L {path}; then head -c 262145 {path}; else exit 4; fi"
    deadline, deadline_exit = c.read("after-deadline", script, allowed=(0, 4))
    if deadline_exit == 0:
        (c.out / f"worker-shutdown-deadline-{pid}.json").write_bytes(deadline)
    worker, _ = c.read("after-worker", f"test -r /proc/self/stat && test -x /proc || exit 2; "
                       f"if test -d /proc/{pid}; then printf PRESENT; else printf ABSENT; fi")
    require(worker in (b"PRESENT", b"ABSENT"), "unrecognized worker probe")
    state = "files/library/records/" + spec["recordId"] + "/state"
    _, state_exit = c.read("after-state", f"test -d {state} && test -r {state} && test -x {state}")
    dirty, _ = c.read("after-dirty", f"test -d {state} && test -r {state} && test -x {state} || exit 2; "
                      f"if test -e {state}/.dirty || test -L {state}/.dirty; then printf PRESENT; else printf ABSENT; fi")
    require(dirty in (b"PRESENT", b"ABSENT"), "unrecognized marker probe")
    final_clock = c.clock("after-final-uptime")
    require(c.boot("after-boot-end") == prior["bootId"], "device rebooted during after collection")
    expected = {**spec, "clock": "android-elapsed-realtime-ms",
                "acceptanceWindowStartMs": prior["readyDeviceClock"]["lowerMs"],
                "acceptanceWindowEndMs": final_clock["upperMs"]}
    require(expected["acceptanceWindowStartMs"] <= expected["acceptanceWindowEndMs"], "device uptime went backwards")
    post = {"schema": 1, "clock": expected["clock"], "workerPid": pid, "workerUid": spec["workerUid"],
            "recordId": spec["recordId"], "baseSha256": spec["baseSha256"], "artifactPins": spec["artifactPins"],
            "observedAtHostMs": final_clock["upperMs"], "workerProbe": {"remoteExit": 0, "pids": [pid] if worker == b"PRESENT" else []},
            "stateDirectoryProbe": {"remoteExit": state_exit, "isDirectory": True},
            "dirtyMarkerProbe": {"remoteExit": 0, "exists": dirty == b"PRESENT"}}
    write_json(c.out / "expected.json", expected)
    write_json(c.out / "post-stop.json", post)
    write_json(c.out / "clock-binding.json", {"bootId": prior["bootId"], "beforeDirectory": str(before_dir),
               "beforeManifestSha256": hashlib.sha256((before_dir / "before.json").read_bytes()).hexdigest(),
               "beforeClock": prior["readyDeviceClock"], "afterClock": final_clock,
               "acceptanceWindowIsBracketNotExactActionTime": True, "artifactPinsSampledByCollector": False})
    return {"status": "AFTER_METADATA_CAPTURED", "cleanShutdownInferred": False,
            "next": "Run verify-shutdown-evidence.py on this directory; retain both phases and raw commands"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("before", "after"))
    parser.add_argument("--serial", required=True)
    parser.add_argument("--package", default="org.lighttouch.android")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--expected", type=Path)
    parser.add_argument("--before", type=Path)
    parser.add_argument("--adb", default="adb")
    args = parser.parse_args()
    created_output = False
    try:
        require(re.fullmatch(r"[A-Za-z0-9_.]+", args.package), "invalid package")
        require((args.expected is not None) if args.phase == "before" else (args.before is not None), "phase input missing")
        args.out.mkdir(mode=0o700, parents=False, exist_ok=False)
        created_output = True
        c = Collector(args.out, args.serial, args.package, args.adb)
        if args.phase == "before":
            spec = json.loads(args.expected.read_text())
            validate_spec(spec)
            result = before(c, spec)
        else:
            result = after(c, args.before)
        write_json(args.out / "collection.json", result)
    except (OSError, ValueError, IndexError, KeyError, TypeError, AttributeError) as error:
        result = {"status": "COLLECTION_FAILED", "error": str(error), "cleanShutdownInferred": False}
        if created_output and not (args.out / "collection.json").exists():
            write_json(args.out / "collection.json", result)
        print(json.dumps(result, sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
