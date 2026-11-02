#!/usr/bin/env python3
"""Compare sampled VFP operations and alignment faults across frame classes.

LT_AUDIO_PATH_PROF selects original f32 add/sub/mul calls with 1/1021 timing
samples and records every CPU 0 AArch32 alignment fault. Start profile stays off.
Requires complete framepath --residency-intervals output from the same capture.
Timings include clock overhead/descheduling, not an FPS improvement estimate.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import statistics

from guestsymbols import GuestSymbols


def fpclass(value):
    magnitude = value & 0x7fffffff
    if not magnitude:
        return "zero"
    exponent = magnitude >> 23
    if exponent == 0:
        return "subnormal"
    if exponent == 255:
        return "infinity" if magnitude == 0x7f800000 else "nan"
    return "normal"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("events", type=Path)
    ap.add_argument("milestones", type=Path)
    ap.add_argument("--engine-pid", type=int, required=True)
    ap.add_argument("--guest-processes", type=Path, required=True)
    ap.add_argument("--guest-kernel", type=Path)
    ap.add_argument("--guest-cache", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    if bool(args.guest_kernel) != bool(args.guest_cache):
        ap.error("provide both guest symbol inputs or neither")
    symbols = GuestSymbols(args.guest_kernel, args.guest_cache) if args.guest_kernel else None
    milestones = json.loads(args.milestones.read_text())
    frames = milestones.get("frame_intervals", [])
    changes = milestones.get("address_space_timeline", [])
    if not frames or not changes:
        ap.error("need framepath --residency-intervals and context markers")
    snapshot = json.loads(args.guest_processes.read_text())
    if snapshot.get("engine_pid") != args.engine_pid or not snapshot.get("detached"):
        ap.error("snapshot needs this engine_pid and confirmed debugger detach")
    if symbols and snapshot["kernel_sha256"] != symbols.hashes["kernel"]:
        ap.error("snapshot and symbols have different kernel hashes")
    mask = ~((1 << snapshot["ttbr0_alignment_bits"]) - 1)
    processes = {}
    for row in snapshot["processes"]:
        if row["ttbr0"] in processes:
            ap.error("ambiguous process pmap")
        processes[row["ttbr0"]] = row
    lines = args.events.read_text().splitlines()
    footer = re.fullmatch(r"# end records=(\d+) dropped=(\d+) incomplete=(\d+) end-ns=(\d+)",
                          lines[-1] if lines else "")
    if (not lines or not lines[0].startswith("# LT_FRAME_PATH 1 ") or not footer
            or int(footer[2]) or int(footer[3]) or int(footer[1]) != len(lines) - 2):
        ap.error("missing/invalid frame-path header/footer or dropped/incomplete data")
    # Do not accept a different trace merely because its selected bounds match.
    trace_changes = []
    samples, faults, pending = [], [], None
    for line in lines[1:-1]:
        fields = line.split()
        if len(fields) != 5 or not all(fields[i].isdigit() for i in (0, 2, 3, 4)):
            ap.error("invalid frame-path record")
        ns, a, b, c = (int(fields[i]) for i in (0, 2, 3, 4))
        kind = fields[1]
        if kind in ("context-switch", "ttbr0"):
            trace_changes.append(dict(ns=ns, kind=kind, a=a, b=b, c=c))
        elif kind == "vfp-sample":
            if pending is not None:
                ap.error("overlapping VFP samples")
            meta = a >> 32
            if meta & 3 > 2 or meta >> 24:
                ap.error("invalid VFP metadata")
            pending = dict(pc=a & 0xffffffff, op=("add", "sub", "mul")[meta & 3],
                           operands=[b >> 32, b & 0xffffffff], before_flags=(meta >> 2) & 0xffff,
                           rounding=(meta >> 18) & 15, flush_inputs=bool(meta & (1 << 22)),
                           flush_output=bool(meta & (1 << 23)), ns=c, record_ns=ns)
        elif kind == "vfp-result":
            if pending is None or pending["pc"] != a:
                ap.error("VFP result without matching sample")
            pending.update(begin_ns=c, end_ns=c + pending["ns"], result=b & 0xffffffff,
                           after_flags=b >> 32)
            if not c <= pending["end_ns"] <= pending["record_ns"] <= ns:
                ap.error("invalid VFP sample timing")
            samples.append(pending)
            pending = None
        elif kind == "alignment":
            faults.append(dict(begin_ns=ns, pc=a, address=b, access=c >> 32,
                               mmu_index=c & 0xffffffff))
    if pending is not None:
        ap.error("incomplete VFP sample pair")
    trace_changes.sort(key=lambda x: x["ns"])
    first = milestones["window"]["begin_ns"]
    last = milestones["window"]["end_exclusive_ns"]
    initial = [row for row in trace_changes if row["ns"] <= first]
    expected = (initial[-1:] + [row for row in trace_changes if first < row["ns"] < last])
    if expected != changes:
        ap.error("milestones context timeline does not match this trace")
    frame_times = [row["begin_ns"] for row in frames]
    change_times = [row["ns"] for row in changes]
    frame_samples, frame_faults = defaultdict(list), defaultdict(list)
    groups = {}

    def assign(row):
        ns = row["begin_ns"]
        i = bisect.bisect_right(frame_times, ns) - 1
        if i < 0 or ns >= frames[i]["end_ns"]:
            return None
        index = bisect.bisect_right(change_times, ns) - 1
        if index < 0:
            ap.error("no address-space context at event")
        address = changes[index]
        key = f"cidr=0x{address['a']:x},ttbr0=0x{address['b']:x}"
        group = groups.setdefault(key, dict(process=processes.get(address["b"] & mask), classes={}))
        category = frames[i]["category"]
        row.update(address_space=key, category=category, frame_index=i,
                   symbol=symbols.lookup(row["pc"]) if symbols else None)
        target = group["classes"].setdefault(category, dict(samples=[], faults=[]))
        return i, target

    for row in samples:
        location = assign(row)
        if location is None:
            continue
        i, target = location
        a, b = row["operands"]
        row["operand_classes"] = [fpclass(a), fpclass(b)]
        row["result_class"] = fpclass(row["result"])
        row["new_flags"] = row["after_flags"] & ~row["before_flags"]
        # This is eligibility from status/inputs, not a measured softfloat branch.
        row["status_allows_hardfloat"] = bool(row["before_flags"] & 0x10) and row["rounding"] == 0
        effective = ["zero" if row["flush_inputs"] and c == "subnormal" else c
                     for c in row["operand_classes"]]
        row["inputs_allow_hardfloat"] = all(c in ("zero", "normal") for c in effective)
        row["tiny_result_candidate"] = ((row["result"] & 0x7fffffff) <= 0x800000
                                          and effective != ["zero", "zero"])
        target["samples"].append(row)
        frame_samples[i].append(row)
    for row in faults:
        location = assign(row)
        if location is None:
            continue
        i, target = location
        target["faults"].append(row)
        frame_faults[i].append(row)

    for key, group in groups.items():
        print(f"{key}: {group['process']}")
        for category, data in group["classes"].items():
            rows = data["samples"]
            histogram = Counter()
            for row in rows:
                label = (row["op"], "/".join(row["operand_classes"]),
                         str(row["rounding"]), str(row["status_allows_hardfloat"]),
                         row["result_class"])
                histogram[label] += 1
            fault_counts = Counter((row["pc"], row["symbol"]) for row in data["faults"])
            data["summary"] = dict(samples=len(rows), sampled_ns=sum(row["ns"] for row in rows),
                                    faults=len(data["faults"]), operations={})
            for op in ("add", "sub", "mul"):
                selected = [row for row in rows if row["op"] == op]
                if not selected:
                    continue
                values = sorted(row["ns"] for row in selected)
                data["summary"]["operations"][op] = dict(
                    samples=len(values), mean_ns=statistics.mean(values),
                    median_ns=statistics.median(values), max_ns=max(values),
                    status_not_ready=sum(not row["status_allows_hardfloat"] for row in selected),
                    special_input=sum(not row["inputs_allow_hardfloat"] for row in selected),
                    tiny_result_candidate=sum(row["tiny_result_candidate"] for row in selected))
            print(f"  {category}: {data['summary']}")
            print("    operands/round/status-ready/result:", histogram.most_common(12))
            print("    fault PCs:", fault_counts.most_common(8))
    hitches = []
    for i, frame in enumerate(frames):
        if frame["category"] != "hitch":
            continue
        detail = dict(frame, relative_end_s=(frame["end_ns"] - first) / 1e9,
                      samples=frame_samples[i], faults=frame_faults[i])
        hitches.append(detail)
        print(f"Hitch {detail['relative_end_s']:.3f}s: {len(detail['samples'])} VFP samples, "
              f"{len(detail['faults'])} alignment faults")
    print("1/1021 systematic samples, not exact call counts or an optimization gain.")
    print("Wall timings include clocks/descheduling; process matches include kernel work.")
    if args.json:
        report = dict(window=milestones["window"], frames=milestones["frames"],
                      engine_pid=args.engine_pid, sample_period=1021, address_spaces=groups,
                      hitches=hitches, input_sha256={
                          "events": hashlib.sha256(args.events.read_bytes()).hexdigest(),
                          "milestones": hashlib.sha256(args.milestones.read_bytes()).hexdigest(),
                          "guest_processes": hashlib.sha256(args.guest_processes.read_bytes()).hexdigest()},
                      guest_symbol_sha256=symbols.hashes if symbols else None)
        args.json.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
