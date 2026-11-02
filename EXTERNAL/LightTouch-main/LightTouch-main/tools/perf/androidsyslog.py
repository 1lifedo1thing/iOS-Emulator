#!/usr/bin/env python3
"""Capture the running Android guest's syslog through its private USB bridge.

ANDROID_SERIAL must select the phone. Requires the helper's syslog subcommand.
Does not start, stop or install anything. The JSONL and .raw retain all received
bytes, including NULs. Chunk timestamps use the phone's CLOCK_MONOTONIC, like
LT_FRAME_LOG; they bound transport receives, not guest message execution. A relay
may send historical backlog. This is a diagnostic, not an ordinary FPS baseline.

  androidsyslog.py capture --seconds 60 --out capture.jsonl
  androidsyslog.py analyse capture.jsonl
"""
import argparse
import collections
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

PKG = "org.lighttouch.android"


def adb(*args):
    return subprocess.check_output(["adb", "exec-out", shlex.join(args)], text=True).strip()


def analyse(path):
    if path.suffix != ".jsonl":
        raise ValueError("Use a .jsonl capture path; sibling .raw/.txt/.summary.json files are derived")
    start = footer = None
    chunks = []
    raw = bytearray()
    target = None
    for line in path.read_text().splitlines():
        row = json.loads(line)
        if "error" in row:
            raise ValueError(f"Incomplete capture: {row['error']}")
        if "target" in row:
            if target is not None or start is not None:
                raise ValueError("Repeated or misplaced target")
            target = row["target"]
        elif "syslog_start" in row:
            if target is None or start is not None or footer is not None:
                raise ValueError("Repeated or misplaced start")
            start = row["syslog_start"]
            if not 1 <= start["seconds"] <= 600:
                raise ValueError("Invalid duration")
        elif "syslog_chunk" in row:
            if start is None or footer is not None:
                raise ValueError("Chunk outside capture")
            c = row["syslog_chunk"]
            data = bytes.fromhex(c["hex"])
            previous = chunks[-1]["end_ns"] if chunks else start["monotonic_ns"]
            if not previous <= c["begin_ns"] <= c["end_ns"]:
                raise ValueError("Nonmonotonic chunk")
            if not 0 < c["bytes"] <= 8192 or len(data) != c["bytes"]:
                raise ValueError("Chunk byte count mismatch")
            chunks.append({k: c[k] for k in ("begin_ns", "end_ns", "bytes")})
            raw.extend(data)
        elif "result" in row:
            if start is None or footer is not None:
                raise ValueError("Repeated or misplaced footer")
            footer = row["result"]
        else:
            raise ValueError(f"Unexpected record: {row}")
    if start is None or footer is None or footer.get("complete") is not True:
        raise ValueError("Missing completed capture footer")
    if (footer["begin_ns"] != start["monotonic_ns"] or
            footer["chunks"] != len(chunks) or footer["bytes"] != len(raw) or
            footer["end_ns"] < start["monotonic_ns"] + start["seconds"] * 1_000_000_000 or
            (chunks and footer["end_ns"] < chunks[-1]["end_ns"])):
        raise ValueError("Footer counts or time bounds disagree")
    path.with_suffix(".raw").write_bytes(raw)
    text = raw.decode("utf-8", "replace").replace("\x00", "")
    path.with_suffix(".txt").write_text(text)
    lines = [line for line in text.splitlines() if line.strip()]
    # Fold changing numbers only in the summary; original text/raw stay intact.
    def key(line):
        line = re.sub(r"^\s*\w{3} \w{3}\s+\d+ [\d:]+ \S+ ", "", line)
        return re.sub(r"\d+", "N", line)
    counts = collections.Counter(map(key, lines))
    report = dict(target=target, capture=footer, chunks=chunks, lines=len(lines),
                  folded_messages=counts.most_common(),
                  timing="Android monotonic receive bounds; not guest execution times; backlog possible")
    path.with_suffix(".summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Complete: {len(raw)} bytes, {len(chunks)} chunks, {len(lines)} lines")
    for message, count in counts.most_common(15):
        print(f"{count:7} {message[:180]}")
    return report


def capture(args):
    if args.out.suffix != ".jsonl":
        raise ValueError("--out must end in .jsonl")
    if not os.environ.get("ANDROID_SERIAL"):
        raise ValueError("Set ANDROID_SERIAL to select the phone")
    pid = adb("pidof", PKG + ":engine")
    if not pid.isdigit():
        raise ValueError("Expected one running engine")
    package = adb("dumpsys", "package", PKG)
    match = re.search(r"legacyNativeLibraryDir=(\S+)", package)
    if not match:
        raise ValueError("Native library directory missing")
    helper = match[1] + "/arm64/liblt_appservice.so"
    sockets = adb("run-as", PKG, "sh", "-c", "ls -t files/usb-run/*/mux.sock 2>/dev/null").splitlines()
    if not sockets:
        raise ValueError("No running guest USB bridge")
    socket = f"/data/data/{PKG}/{sockets[0]}"
    command = ["run-as", PKG, helper, "syslog", "--socket", socket, "--seconds", str(args.seconds)]
    with args.out.open("xb") as output:
        result = subprocess.run(["adb", "exec-out", shlex.join(command)], stdout=output)
    metadata = dict(serial=os.environ["ANDROID_SERIAL"], engine_pid=int(pid),
                    helper=helper, socket=socket, returncode=result.returncode,
                    engine_pid_after=adb("pidof", PKG + ":engine"))
    args.out.with_suffix(".identity.json").write_text(json.dumps(metadata, indent=2) + "\n")
    if result.returncode:
        raise ValueError(f"Helper exited {result.returncode}; partial output preserved in {args.out}")
    if metadata["engine_pid_after"] != pid:
        raise ValueError("Engine changed during capture; output preserved")
    analyse(args.out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    cap = commands.add_parser("capture")
    cap.add_argument("--seconds", type=int, choices=range(1, 601), metavar="1..600", required=True)
    cap.add_argument("--out", type=Path, required=True)
    report = commands.add_parser("analyse")
    report.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "capture":
            capture(args)
        else:
            analyse(args.path)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
