#!/usr/bin/env python3
"""Compare timestamped vCPU self samples during ordinary and long frame gaps.

Consumes hitchsched.py JSON, the matching frame log, attached simpleperf data,
before/after guest-PC maps and the frozen build's unstripped engine library.
CPU samples describe running code, not the reason a thread slept or GPU time.
"""
import argparse
import bisect
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

from framestats import parse
from guestpc import PerfMap
from guestsymbols import GuestSymbols


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("record", type=Path)
    ap.add_argument("frames", type=Path)
    ap.add_argument("scheduler", type=Path)
    ap.add_argument("--before-map", type=Path, required=True)
    ap.add_argument("--after-map", type=Path, required=True)
    ap.add_argument("--engine-symbols", type=Path, required=True)
    ap.add_argument("--build-identity", type=Path, required=True,
                    help="frozen build JSON containing library_sha256")
    ap.add_argument("--nm", default="llvm-nm")
    ap.add_argument("--milestones", type=Path,
                    help="same capture's framepath JSON with --residency-intervals")
    ap.add_argument("--guest-processes", type=Path,
                    help="post-capture proc/pmap snapshot with capture engine_pid")
    ap.add_argument("--guest-kernel", type=Path)
    ap.add_argument("--guest-cache", type=Path)
    ap.add_argument("--simpleperf-dir", type=Path,
                    default=Path.home() / "Android/Sdk/ndk/28.2.13676358/simpleperf")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    scheduler = json.loads(args.scheduler.read_text())
    pid, tid = scheduler["engine_pid"], int(scheduler["vcpu"]["tid"])
    if bool(args.guest_kernel) != bool(args.guest_cache):
        ap.error("guest symbols require both --guest-kernel and --guest-cache")
    guest_symbols = GuestSymbols(args.guest_kernel, args.guest_cache) if args.guest_kernel else None
    addresses, address_times, address_groups, process_tables = [], [], {}, {}
    if args.milestones:
        milestones = json.loads(args.milestones.read_text())
        if milestones["window"] != {k: scheduler["window"][k]
                                    for k in ("begin_ns", "end_exclusive_ns")}:
            ap.error("milestone and scheduler windows differ")
        addresses = milestones.get("address_space_timeline", [])
        if not addresses:
            ap.error("milestones need framepath --residency-intervals and context tracing")
        address_times = [e["ns"] for e in addresses]
    if args.guest_processes:
        if not args.milestones:
            ap.error("process attribution requires --milestones")
        processes = json.loads(args.guest_processes.read_text())
        if processes.get("engine_pid") != pid or not processes.get("detached"):
            ap.error("guest snapshot needs this engine_pid and confirmed debugger detach")
        if guest_symbols and processes["kernel_sha256"] != guest_symbols.hashes["kernel"]:
            ap.error("guest snapshot and symbol kernel differ")
        table_mask = ~((1 << processes["ttbr0_alignment_bits"]) - 1)
        for row in processes["processes"]:
            if row["ttbr0"] in process_tables:
                ap.error("process snapshot has ambiguous page-table identity")
            process_tables[row["ttbr0"]] = row
    identity = json.loads(args.build_identity.read_text())
    digest = hashlib.sha256(args.engine_symbols.read_bytes()).hexdigest()
    if digest != identity["library_sha256"]:
        ap.error("engine symbols differ from the frozen build identity")
    for path in (args.before_map, args.after_map):
        match = re.match(r"perf-(\d+)(?:[.-])", path.name)
        if not match or int(match[1]) != pid:
            ap.error("both perfmap filenames must retain the scheduler's worker PID")
    before, after = PerfMap(args.before_map), PerfMap(args.after_map)
    mapped_ips = {}
    symbols = []
    result = subprocess.run([args.nm, "-n", "-S", "--defined-only", str(args.engine_symbols)],
                            text=True, capture_output=True, check=True)
    for line in result.stdout.splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) == 4 and parts[2] in ("t", "T", "w", "W"):
            address, size = int(parts[0], 16), int(parts[1], 16)
            if size:
                symbols.append((address, address + size, parts[3]))
    symbols.sort()
    symbol_starts = [s[0] for s in symbols]
    if not symbols:
        ap.error("engine has no function symbols")

    def native(symbol):
        dso = symbol.dso_name.rsplit("/", 1)[-1]
        name = symbol.symbol_name
        if dso == "libqemu-arm.so":
            index = bisect.bisect_right(symbol_starts, symbol.vaddr_in_file) - 1
            if index >= 0 and symbols[index][0] <= symbol.vaddr_in_file < symbols[index][1]:
                name = symbols[index][2]
        return f"{dso}:{name}"

    def classify(ip, symbol):
        if symbol.dso_name == "unknown":
            if ip not in mapped_ips:
                left, right = before.lookup(ip), after.lookup(ip)
                mapped_ips[ip] = right if left == right else ("unstable", None)
            state, pc = mapped_ips[ip]
            if pc is not None:
                # These are address regions, not a guest process identification.
                region = "guest-high-address" if pc >= 0x80000000 else "guest-low-address"
                label = guest_symbols.lookup(pc) if guest_symbols else None
                return region, f"guest:{label}" if label else f"guest:0x{pc:08x}"
            return f"anonymous-{state}", f"anonymous-{state}"
        if symbol.dso_name.endswith("/libqemu-arm.so"):
            category = "engine"
        elif any(s in symbol.dso_name for s in ("adreno", "libgsl", "libEGL", "libGLES")):
            category = "GL-driver"
        else:
            category = "other-native"
        return category, native(symbol)

    begin = scheduler["window"]["begin_ns"]
    end = scheduler["window"]["end_exclusive_ns"]
    frames = [r for r in parse(args.frames.read_text()) if begin <= r[0] < end]
    if len(frames) < 2 or frames[0][0] != begin:
        ap.error("frame log does not match scheduler window")
    if any(b[0] <= a[0] or b[1] <= a[1] for a, b in zip(frames, frames[1:])):
        ap.error("frame timestamps/sequences must strictly increase")
    starts = [r[0] for r in frames]
    hitch_bounds = {(h["begin_ns"], h["end_ns"]) for h in scheduler["hitches"]}
    classes, intervals = {}, []
    for a, b in zip(frames, frames[1:]):
        gap = b[0] - a[0]
        skipped = max(0, b[1] - a[1] - 1)
        category = "skipped" if skipped else ("hitch" if (a[0], b[0]) in hitch_bounds
                                              else "late" if gap > 20e6 else "normal")
        intervals.append(dict(begin_ns=a[0], end_ns=b[0], ms=gap / 1e6,
                              skipped=skipped, category=category))

    def accumulate(group, category, name, weight, chain):
        group["samples"] = group.get("samples", 0) + 1
        group["event_weight_ns"] = group.get("event_weight_ns", 0) + weight
        for field, key in (("categories", category), ("self", name)):
            group.setdefault(field, Counter())[key] += weight
        for key in set(chain + [name]):
            group.setdefault("inclusive", Counter())[key] += weight

    sys.path.insert(0, str(args.simpleperf_dir))
    from simpleperf_report_lib import GetReportLib
    lib = GetReportLib(str(args.record))
    first = last = None
    try:
        command = lib.GetRecordCmd()
        tokens = shlex.split(command)
        if not any(tokens[i:i + 2] == ["--clockid", "monotonic"]
                   for i in range(len(tokens) - 1)):
            ap.error("record simpleperf with --clockid monotonic to align timestamps")
        while (sample := lib.GetNextSample()) is not None:
            if sample.pid != pid or sample.tid != tid:
                continue
            if lib.GetEventOfCurrentSample().name != "cpu-clock":
                ap.error("record one cpu-clock event; mixed event weights cannot be combined")
            first = sample.time if first is None else min(first, sample.time)
            last = sample.time if last is None else max(last, sample.time)
            index = bisect.bisect_right(starts, sample.time) - 1
            if index < 0 or index >= len(intervals):
                continue
            interval = intervals[index]
            category, name = classify(sample.ip, lib.GetSymbolOfCurrentSample())
            callchain = lib.GetCallChainOfCurrentSample()
            chain = [native(callchain.entries[j].symbol) for j in range(callchain.nr)]
            accumulate(classes.setdefault(interval["category"], {}), category, name,
                       sample.period, chain)
            accumulate(interval, category, name, sample.period, chain)
            if addresses:
                address_index = bisect.bisect_right(address_times, sample.time) - 1
                # Avoid assigning a sample interrupted inside the register-write
                # helper to one of its short-lived transitional register pairs.
                near_boundary = address_index < 0 or sample.time - address_times[address_index] <= 10_000
                if address_index + 1 < len(address_times):
                    near_boundary |= address_times[address_index + 1] - sample.time <= 10_000
                if near_boundary:
                    key, process = "address-boundary/unknown", None
                else:
                    row = addresses[address_index]
                    key = f"cidr=0x{row['a']:x},ttbr0=0x{row['b']:x}"
                    process = process_tables.get(row["b"] & table_mask) if process_tables else None
                group = address_groups.setdefault(key, dict(process=process, classes={}))
                accumulate(group["classes"].setdefault(interval["category"], {}), category, name,
                           sample.period, chain)
    finally:
        lib.Close()
    if first is None or first > begin or last < frames[-1][0]:
        ap.error("vCPU CPU samples do not span the selected frame intervals")
    report = dict(engine_pid=pid, vcpu_tid=tid, command=command,
                  engine_sha256=digest, build_identity=identity,
                  window=scheduler["window"], classes=classes, intervals=intervals,
                  first_sample_ns=first, last_sample_ns=last,
                  before_map_entries=len(before.records), after_map_entries=len(after.records))
    report["address_spaces"] = address_groups
    report["guest_symbol_input_sha256"] = guest_symbols.hashes if guest_symbols else None
    for label, group in classes.items():
        total = group["event_weight_ns"]
        print(f"{label}: {group['samples']} samples; {total / 1e6:.3f} ms event weight")
        for field in ("categories", "self", "inclusive"):
            print(f"  {field}:")
            for name, weight in group[field].most_common(15):
                print(f"    {100 * weight / total:6.2f}% {name}")
    for interval in intervals:
        if interval["category"] == "hitch":
            print(f"Hitch ending {(interval['end_ns'] - begin) / 1e9:.3f}s: "
                  f"{interval['ms']:.3f} ms, {interval.get('samples', 0)} samples")
    for key, address in address_groups.items():
        print(f"Address space {key}: {address['process']}")
        for label, group in address["classes"].items():
            print(f"  {label}: {group['samples']} samples:")
            total = group["event_weight_ns"]
            for name, weight in group["self"].most_common(12):
                print(f"    {100 * weight / total:6.2f}% {name}")
    print("Sparse per-hitch samples are leads, not proof of a culprit. Inclusive rows overlap.")
    print("Guest address regions alone do not identify a process; unstable/unmapped PCs are excluded.")
    if addresses:
        print("Address-space groups include kernel/native work; samples within 10 us of a change are separate.")
        print("Process names are post-capture pmap matches, not per-sample guest thread identities.")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
