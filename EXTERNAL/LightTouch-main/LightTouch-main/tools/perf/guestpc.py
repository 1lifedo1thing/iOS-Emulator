#!/usr/bin/env python3
"""Attribute simpleperf self samples to guest PCs using the same worker's perfmap.

This does not identify guest processes or unwind guest stacks. Unmapped samples
include native work and JIT instructions missing from the map. Overlapping map
entries with different guest PCs are ambiguous and are excluded.
"""
import argparse
from bisect import bisect_right
from collections import Counter
import json
from pathlib import Path
import re
import sys


class PerfMap:
    def __init__(self, path):
        records = set()
        self.ignored = 0
        for line in path.read_text().splitlines(keepends=True):
            if not line.endswith("\n"):
                self.ignored += 1
                continue
            parts = line.split(maxsplit=2)
            if len(parts) != 3 or not re.fullmatch(r"guest-0x[0-9a-fA-F]+", parts[2].strip()):
                self.ignored += 1
                continue
            start, size = int(parts[0], 16), int(parts[1], 16)
            if size:
                records.add((start, start + size, int(parts[2].strip()[8:], 16)))
        self.records = sorted(records)
        self.starts = [r[0] for r in self.records]
        self.max_ends = []
        end = 0
        for row in self.records:
            end = max(end, row[1])
            self.max_ends.append(end)
        if not self.records:
            raise ValueError(f"{path}: no guest-PC entries")

    def lookup(self, ip):
        index = bisect_right(self.starts, ip) - 1
        found = set()
        # Prefix maxima retain all overlapping candidates, including entries
        # from a reused translation buffer. Never silently choose the last.
        while index >= 0 and self.max_ends[index] > ip:
            start, end, pc = self.records[index]
            if start <= ip < end:
                found.add(pc)
                if len(found) > 1:
                    return "ambiguous", None
            index -= 1
        return ("mapped", next(iter(found))) if found else ("unmapped", None)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("record", type=Path)
    ap.add_argument("perfmap", type=Path, help="perf-PID.map from the recorded worker")
    ap.add_argument("--before-map", type=Path,
                    help="also require the same unambiguous mapping before recording")
    ap.add_argument("--simpleperf-dir", type=Path,
                    default=Path.home() / "Android/Sdk/ndk/28.2.13676358/simpleperf",
                    help="NDK directory containing simpleperf_report_lib.py")
    ap.add_argument("--comm", default="CPU 0/TCG")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    if args.top <= 0:
        ap.error("--top must be positive")
    identity = re.match(r"perf-(\d+)(?:[.-])", args.perfmap.name)
    if not identity:
        ap.error("perfmap filename must retain its worker PID: perf-PID.map")
    pid = int(identity[1])
    try:
        mapping = PerfMap(args.perfmap)
        before = PerfMap(args.before_map) if args.before_map else None
        if args.before_map:
            before_id = re.match(r"perf-(\d+)(?:[.-])", args.before_map.name)
            if not before_id or int(before_id[1]) != pid:
                ap.error("before/after maps must name the same worker PID")
    except (OSError, ValueError) as error:
        ap.error(str(error))
    sys.path.insert(0, str(args.simpleperf_dir))
    try:
        from simpleperf_report_lib import GetReportLib
    except ImportError:
        ap.error("set --simpleperf-dir to the NDK simpleperf Python tools directory")
    lib = GetReportLib(str(args.record))
    cycles, samples, totals, sample_totals = Counter(), Counter(), Counter(), Counter()
    event_name = None
    first = last = None
    try:
        while (sample := lib.GetNextSample()) is not None:
            if sample.thread_comm != args.comm:
                continue
            if sample.pid != pid:
                ap.error(f"recorded worker PID {sample.pid} differs from map PID {pid}")
            event = lib.GetEventOfCurrentSample().name
            if event_name is None:
                event_name = event
            if event != event_name:
                ap.error("use a recording with one event; mixed event weights cannot be added")
            first = sample.time if first is None else min(first, sample.time)
            last = sample.time if last is None else max(last, sample.time)
            state, pc = mapping.lookup(sample.ip)
            if before and state == "mapped" and before.lookup(sample.ip) != (state, pc):
                state, pc = "unstable", None
            totals[state] += sample.period
            sample_totals[state] += 1
            if pc is not None:
                cycles[pc] += sample.period
                samples[pc] += 1
    finally:
        lib.Close()
    if first is None:
        ap.error(f"no samples for {args.comm}")
    total = sum(totals.values())
    mapped = totals["mapped"]
    rows = [{"pc": f"0x{pc:08x}", "samples": samples[pc], "event_weight": weight,
             "percent_vcpu": 100 * weight / total if total else 0,
             "percent_mapped_guest": 100 * weight / mapped if mapped else 0}
            for pc, weight in cycles.most_common()]
    report = {"pid": pid, "comm": args.comm, "event": event_name,
              "first_sample_ns": first, "last_sample_ns": last,
              "event_weights": dict(totals), "sample_counts": dict(sample_totals),
              "map_entries": len(mapping.records), "map_ignored_rows": mapping.ignored,
              "requires_stable_mapping": before is not None, "guest_pcs": rows}
    print(f"{args.comm}, worker {pid}, {sum(sample_totals.values())} self samples ({event_name})")
    print(f"Mapped guest: {100 * mapped / total if total else 0:.2f}% of vCPU event weight")
    print("Unmapped includes native work and missing JIT mappings; process identity is unknown.")
    print("Sample categories:", dict(sample_totals))
    for row in rows[:args.top]:
        print(f"  {row['pc']} {row['percent_vcpu']:6.2f}% vCPU "
              f"{row['percent_mapped_guest']:6.2f}% mapped guest, {row['samples']} samples")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
