#!/usr/bin/env python3
"""Join guest address-space residency to vCPU scheduler states and frame classes.

Consumes framepath.py and hitchsched.py JSON from the same capture. Residency
identifies register values, not a guest process/thread or user/kernel execution.
"""
import argparse
import bisect
from collections import defaultdict
import json
from pathlib import Path


def residency(changes, states, begin, end, offset, times=None, starts=None):
    """Intersect monotonic address intervals with Perfetto scheduler intervals."""
    if times is None:
        times = [e["ns"] for e in changes]
    if not times or times[0] > begin:
        raise ValueError("requested residency precedes captured address-space context")
    if starts is None:
        starts = [e["ts"] - offset for e in states]
    index = max(0, bisect.bisect_right(starts, begin) - 1)
    totals = defaultdict(lambda: defaultdict(float))
    cursor = begin
    while index < len(states) and states[index]["ts"] - offset < end:
        state = states[index]
        left = max(begin, state["ts"] - offset)
        right = min(end, state["end_ns"] - offset)
        if right <= left:
            index += 1
            continue
        if left != cursor:
            raise ValueError("scheduler coverage has a gap or overlap")
        while left < right:
            address_index = bisect.bisect_right(times, left) - 1
            change = changes[address_index]
            stop = min(right, times[address_index + 1]
                       if address_index + 1 < len(times) else right)
            key = f"cidr=0x{change['a']:x},ttbr0=0x{change['b']:x}"
            totals[key][state["state"]] += (stop - left) / 1e6
            left = stop
        cursor = right
        index += 1
    if cursor != end:
        raise ValueError("incomplete scheduler coverage for residency interval")
    return dict(totals)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("milestones", type=Path)
    ap.add_argument("scheduler", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    milestones = json.loads(args.milestones.read_text())
    scheduler = json.loads(args.scheduler.read_text())
    if milestones["window"] != {k: scheduler["window"][k]
                                for k in ("begin_ns", "end_exclusive_ns")}:
        ap.error("frame and scheduler windows differ")
    offset = scheduler["clock_offset_ns"]
    sched_hitches = {(h["begin_ns"], h["end_ns"]): h for h in scheduler["hitches"]}
    output = []
    for hitch in milestones["hitches"]:
        pair = (hitch["begin_ns"], hitch["end_ns"])
        if pair not in sched_hitches:
            ap.error("milestone hitch missing from scheduler report")
        initial = hitch["address_space_initial"]
        if initial is None:
            ap.error("no guest context markers; enable LT_FRAME_PATH_CONTEXT")
        changes = [initial, *hitch["address_space_changes"]]
        states = sched_hitches[pair]["vcpu"]["events"]

        def interval_residency(begin, end):
            try:
                return residency(changes, states, begin, end, offset)
            except ValueError as error:
                ap.error(str(error))

        entry = dict(begin_ns=pair[0], end_ns=pair[1], relative_end_s=hitch["relative_end_s"],
                     ms=hitch["ms"], residency_ms=interval_residency(*pair), after_present=[])
        for flip in hitch["flips"]:
            present = flip["latest_present"]
            if not present:
                continue
            # Only report the portion inside this frontend hitch: the preceding
            # present can belong to an earlier frame and address-space interval.
            begin = max(pair[0], present["end_ns"])
            end = min(pair[1], flip["event"]["ns"])
            if begin >= end:
                continue
            entry["after_present"].append(dict(begin_ns=begin, end_ns=end,
                                                full_present_to_flip_ms=flip["since_present_end_ms"],
                                                residency_ms=interval_residency(begin, end)))
        output.append(entry)
        print(f"{entry['relative_end_s']:.3f}s hitch {entry['ms']:.3f} ms:")
        for key, states in sorted(entry["residency_ms"].items(),
                                   key=lambda r: -r[1].get("Running", 0)):
            print(f"  {key}: {dict(states)}")
        for interval in entry["after_present"]:
            print(f"  Present-to-flip full {interval['full_present_to_flip_ms']:.3f} ms; "
                  f"in-hitch residency: {interval['residency_ms']}")
    classes = {}
    if "frame_intervals" in milestones or "vcpu_states" in scheduler:
        if not all(("frame_intervals" in milestones, "vcpu_states" in scheduler,
                    milestones.get("address_space_timeline"))):
            ap.error("class comparison needs framepath --residency-intervals and hitchsched --all-states")
        times = [e["ns"] for e in milestones["address_space_timeline"]]
        starts = [e["ts"] - offset for e in scheduler["vcpu_states"]]
        for frame in milestones["frame_intervals"]:
            try:
                values = residency(milestones["address_space_timeline"], scheduler["vcpu_states"],
                                   frame["begin_ns"], frame["end_ns"], offset, times, starts)
            except ValueError as error:
                ap.error(str(error))
            group = classes.setdefault(frame["category"], dict(frames=0, wall_ms=0,
                                                               residency_ms={}))
            group["frames"] += 1
            group["wall_ms"] += (frame["end_ns"] - frame["begin_ns"]) / 1e6
            for key, states in values.items():
                for state, ms in states.items():
                    target = group["residency_ms"].setdefault(key, {})
                    target[state] = target.get(state, 0) + ms
        for category, group in classes.items():
            running = sum(states.get("Running", 0) for states in group["residency_ms"].values())
            print(f"{category}: {group['frames']} frames, {group['wall_ms']:.3f} ms wall, "
                  f"{running:.3f} ms running:")
            for key, states in sorted(group["residency_ms"].items(),
                                      key=lambda r: -r[1].get("Running", 0)):
                ms = states.get("Running", 0)
                print(f"  {key}: {ms:.3f} ms running, "
                      f"{100 * ms / running if running else 0:.2f}% of running time")
    print("Residency covers native engine/driver and guest execution, including kernel code.")
    print("It does not establish guest process identity or prove the present-to-flip dependency.")
    if args.json:
        args.json.write_text(json.dumps(dict(window=milestones["window"],
                                              render_contexts=milestones["contexts"],
                                              frame_classes=classes,
                                              hitches=output), indent=2) + "\n")


if __name__ == "__main__":
    main()
