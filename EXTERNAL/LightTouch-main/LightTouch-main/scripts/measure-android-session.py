#!/usr/bin/env python3
"""Measure a debuggable app's worker without collecting unrelated device logs."""
import argparse
import datetime
import json
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--package", default="org.lighttouch.android")
    parser.add_argument("--process", default="org.lighttouch.android:engine")
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--adb", default="adb")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.]+", args.package) or not re.fullmatch(r"[A-Za-z0-9_.:]+", args.process):
        parser.error("invalid package/process")
    if args.seconds <= 0:
        parser.error("duration must be positive")
    args.output.mkdir(parents=True, exist_ok=False)

    def adb(*command):
        return subprocess.check_output([args.adb, "-s", args.serial, *command], text=True, timeout=15).strip()

    start = time.monotonic()
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    report = {"schema_version": 1, "started_at": started, "requested_seconds": args.seconds,
              "package": args.package, "process": args.process, "serial": args.serial,
              "scope": "process-resource-measurement", "guest_acceptance": "NOT_ESTABLISHED"}
    samples = []
    try:
        report["page_size"] = int(adb("shell", "getconf", "PAGESIZE"))
        report["clock_ticks_per_second"] = int(adb("shell", "getconf", "CLK_TCK"))
        pid = adb("shell", "pidof", args.process)
        if not re.fullmatch(r"[0-9]+", pid):
            raise ValueError("expected exactly one running worker PID")
        report["pid"] = int(pid)
        with (args.output / "samples.jsonl").open("w") as output:
            while True:
                elapsed = time.monotonic() - start
                if elapsed >= args.seconds:
                    break
                if adb("shell", "pidof", args.process) != pid:
                    raise ValueError("worker exited or changed PID during measurement")
                # Same app UID can inspect its worker. Do not use root or dumpsys
                # over all apps. Input names are validated, PID contains digits.
                raw = adb("shell", "run-as", args.package, "cat", f"/proc/{pid}/stat")
                fields = raw[raw.rfind(")") + 2:].split()
                if len(fields) < 22:
                    raise ValueError("truncated process statistics")
                sample = {"elapsed_seconds": round(elapsed, 3), "pid": int(pid),
                          "user_ticks": int(fields[11]), "system_ticks": int(fields[12]),
                          "rss_bytes": int(fields[21]) * report["page_size"],
                          "virtual_bytes": int(fields[20]), "threads": int(fields[17])}
                samples.append(sample)
                output.write(json.dumps(sample) + "\n")
                output.flush()
                # Independent sample interval; slow adb never causes a burst.
                time.sleep(min(1, max(0, args.seconds - (time.monotonic() - start))))
        report["result"] = "PASS"
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        report["result"] = "FAIL"
        report["error"] = str(error)
    report["observed_seconds"] = round(time.monotonic() - start, 3)
    report["samples"] = len(samples)
    if samples:
        report["rss_peak_bytes"] = max(sample["rss_bytes"] for sample in samples)
        report["cpu_seconds"] = (samples[-1]["user_ticks"] + samples[-1]["system_ticks"]
                                 - samples[0]["user_ticks"] - samples[0]["system_ticks"]) / report["clock_ticks_per_second"]
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
