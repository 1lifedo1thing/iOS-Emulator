#!/usr/bin/env python3
"""Summarize vCPU BQL acquisitions between two cumulative accounting dumps."""
import argparse
import json
import re
from pathlib import Path


def read_dumps(path):
    dumps = []
    current = None
    for line in path.read_text(errors="replace").splitlines():
        match = re.search(r"\[acct\]\s+([\d.]+)s\s+run", line)
        if match:
            current = {"seconds": float(match[1]), "sites": {}}
            dumps.append(current)
            continue
        if current is None:
            continue
        match = re.search(
            r"\[acct\]   bounds begin-ns (\d+) end-ns (\d+) full=(on|off)", line)
        if match:
            current.update(origin_ns=int(match[1]), end_ns=int(match[2]),
                           full_accounting=match[3] == "on")
            current["seconds"] = (current["end_ns"] - current["origin_ns"]) / 1e9
            continue
        match = re.search(r"\[acct\]   end-ns (\d+)", line)
        if match:
            current["complete"] = current.get("end_ns") == int(match[1])
            continue
        match = re.search(r"\[acct\]   vcpu-bql n (\d+) wait-ns (\d+)", line)
        if match:
            current["sites"]["TOTAL"] = (int(match[1]), int(match[2]))
            continue
        match = re.search(r"\[acct\]   bql-by-mr (\S+) n (\d+) wait-ns (\d+)", line)
        if match:
            key = re.sub(r"^.*qemu-ios(?:-android)?/", "", match[1])
            n, ns = current["sites"].get(key, (0, 0))
            current["sites"][key] = (n + int(match[2]), ns + int(match[3]))
    return [dump for dump in dumps if "TOTAL" in dump["sites"] and
            ("end_ns" not in dump or dump.get("complete"))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("start", type=float, help="seconds since accounting began")
    parser.add_argument("end", type=float, help="seconds since accounting began")
    parser.add_argument("--json", type=Path, help="retain selected bounds and all sites")
    args = parser.parse_args()
    if args.end <= args.start:
        parser.error("end must be later than start")
    dumps = read_dumps(args.log)
    if not dumps:
        parser.error("no accounting snapshots with BQL totals")
    before = min(dumps, key=lambda dump: abs(dump["seconds"] - args.start))
    after = min(dumps, key=lambda dump: abs(dump["seconds"] - args.end))
    exact = "end_ns" in before and "end_ns" in after
    if exact and before["origin_ns"] != after["origin_ns"]:
        parser.error("selected snapshots have different accounting origins")
    dt = ((after["end_ns"] - before["end_ns"]) / 1e9 if exact else
          after["seconds"] - before["seconds"])
    if dt <= 0:
        parser.error("requested interval selects the same or reversed snapshots")
    rows = []
    for key, (count, wait_ns) in after["sites"].items():
        old_count, old_ns = before["sites"].get(key, (0, 0))
        count -= old_count
        wait_ns -= old_ns
        if count < 0 or wait_ns < 0:
            parser.error(f"counters decreased for {key}")
        rows.append(dict(site=key, acquisitions=count, wait_ns=wait_ns,
                         acquisitions_per_second=count / dt,
                         wait_percent=wait_ns / dt / 1e7,
                         mean_wait_us=wait_ns / count / 1e3 if count else 0))
    rows.sort(key=lambda row: -row["wait_percent"])
    result = dict(requested_start_seconds=args.start, requested_end_seconds=args.end,
                  selected_start_seconds=before["seconds"],
                  selected_end_seconds=after["seconds"], duration_seconds=dt,
                  exact_bounds=exact, start_ns=before.get("end_ns"),
                  end_ns=after.get("end_ns"),
                  full_accounting=after.get("full_accounting"), sites=rows)
    print(f"Selected {before['seconds']:.6f} → {after['seconds']:.6f} s "
          f"({dt:.6f} s; {'exact' if exact else 'rounded legacy'} bounds)")
    for row in rows[:12]:
        print(f"{row['site']:60s} {row['acquisitions_per_second']:8.0f}/s  "
              f"wait {row['wait_percent']:5.1f}%  mean {row['mean_wait_us']:5.1f} us")
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
