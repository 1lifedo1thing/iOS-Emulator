#!/usr/bin/env python3
"""Time sampled architectural alignment handling, preserving guest execution.

Uses LT_ALIGNMENT_PATH_PROF=returns markers and the same capture's audiopath,
framepath and hitchsched JSON. Matching requires thread register, installed
page table/CIDR and return to the next ARM instruction. These intervals begin
after fault-state restore and end after the original exception-return helper;
they exclude the triggering memory probe and restore, and include diagnostics.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import statistics

from frameresidency import residency


def summary(rows):
    if not rows:
        return dict(samples=0)
    wall = sorted(row["wall_ns"] for row in rows)
    return dict(samples=len(rows), mean_ns=statistics.mean(wall),
                median_ns=statistics.median(wall), p99_ns=wall[round(.99 * (len(wall) - 1))],
                max_ns=max(wall), mean_target_running_ns=statistics.mean(
                    row["target_running_ns"] for row in rows),
                mean_all_running_ns=statistics.mean(row["all_running_ns"] for row in rows),
                mean_runnable_ns=statistics.mean(row["runnable_ns"] for row in rows),
                mean_sleeping_ns=statistics.mean(row["sleeping_ns"] for row in rows))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("events", type=Path)
    ap.add_argument("audio", type=Path)
    ap.add_argument("milestones", type=Path)
    ap.add_argument("scheduler", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    audio = json.loads(args.audio.read_text())
    milestones = json.loads(args.milestones.read_text())
    scheduler = json.loads(args.scheduler.read_text())
    if audio["input_sha256"]["events"] != hashlib.sha256(args.events.read_bytes()).hexdigest():
        ap.error("audio report belongs to another trace")
    if audio["input_sha256"]["milestones"] != hashlib.sha256(args.milestones.read_bytes()).hexdigest():
        ap.error("audio report belongs to other milestones")
    if (audio["window"] != milestones["window"] or audio["engine_pid"] != scheduler["engine_pid"]
            or audio["window"] != {k: scheduler["window"][k] for k in audio["window"]}):
        ap.error("capture identities/windows differ")
    states = scheduler.get("vcpu_states")
    changes = milestones.get("address_space_timeline")
    frames = milestones.get("frame_intervals")
    if not states or not changes or not frames:
        ap.error("need hitchsched --all-states and framepath --residency-intervals")
    begin, end = audio["window"]["begin_ns"], audio["window"]["end_exclusive_ns"]
    offset = scheduler["clock_offset_ns"]
    state_starts = [row["ts"] - offset for row in states]
    change_times = [row["ns"] for row in changes]
    frame_times = [row["begin_ns"] for row in frames]
    pending, seen, complete, abandoned, outside = {}, set(), [], [], []
    # audiopath has already validated the exact hashed trace's complete footer.
    for line in args.events.read_text().splitlines()[1:-1]:
        ns, kind, a, b, c = line.split()
        if kind not in ("alignment-begin", "alignment-return", "alignment-abandon",
                        "alignment-args", "alignment-caller"):
            continue
        ns, a, b, c = map(int, (ns, a, b, c))
        if kind == "alignment-begin":
            if a in seen:
                ap.error("alignment sample ID reused")
            seen.add(a)
            pending[a] = dict(id=a, begin_ns=ns, thread=b >> 32,
                              pc=b & 0xffffffff, ttbr0=c)
        elif kind in ("alignment-args", "alignment-caller"):
            row = pending.get(a)
            if row is None:
                ap.error("register snapshot without pending alignment sample")
            if kind in row or ns < row["begin_ns"]:
                ap.error("duplicate or early register snapshot")
            row[kind] = (dict(r0=b >> 32, r1=b & 0xffffffff,
                              r2=c >> 32, r3=c & 0xffffffff)
                         if kind == "alignment-args" else
                         dict(lr=b >> 32, sp=b & 0xffffffff, cidr=c))
        elif kind == "alignment-abandon":
            row = pending.pop(a, dict(id=a))
            row.update(end_ns=ns, reason=b, detail=c)
            abandoned.append(row)
        else:
            row = pending.pop(a, None)
            if row is None:
                ap.error("return without sampled alignment fault")
            if (row["thread"] != b >> 32 or (b & 0xffffffff) != row["pc"] + 4
                    or c != row["ttbr0"] or ns <= row["begin_ns"]):
                ap.error("invalid alignment return identity/timing")
            row.update(end_ns=ns, wall_ns=ns - row["begin_ns"])
            if ("alignment-args" in row) != ("alignment-caller" in row):
                ap.error("incomplete alignment register snapshot pair")
            if row["begin_ns"] < begin or ns >= end:
                outside.append(row)
                continue
            i = bisect.bisect_right(frame_times, row["begin_ns"]) - 1
            if i < 0 or row["begin_ns"] >= frames[i]["end_ns"]:
                ap.error("sample outside retained frame intervals")
            ci = bisect.bisect_right(change_times, row["begin_ns"]) - 1
            address = changes[ci]
            if address["b"] != c:
                ap.error("sampled TTBR0 differs from context trace")
            if ("alignment-caller" in row
                    and row["alignment-caller"]["cidr"] != address["a"]):
                ap.error("sampled CIDR differs from context trace")
            key = f"cidr=0x{address['a']:x},ttbr0=0x{c:x}"
            source = audio["address_spaces"].get(key)
            if source is None:
                ap.error("alignment sample has no matching audio address space")
            values = residency(changes, states, row["begin_ns"], ns, offset,
                               change_times, state_starts)
            totals = Counter()
            for value in values.values():
                totals.update(value)
            row.update(address_space=key, process=source["process"],
                       category=frames[i]["category"], frame_index=i,
                       crosses_frame=ns > frames[i]["end_ns"], residency_ms=values,
                       target_running_ns=1e6 * values.get(key, {}).get("Running", 0),
                       all_running_ns=1e6 * totals["Running"],
                       runnable_ns=1e6 * (totals["R"] + totals["R+"]),
                       sleeping_ns=1e6 * sum(v for k, v in totals.items()
                                            if k not in ("Running", "R", "R+")))
            complete.append(row)
    groups = defaultdict(lambda: defaultdict(list))
    for row in complete:
        groups[row["address_space"]][row["category"]].append(row)
    summaries = {key: {cat: summary(rows) for cat, rows in group.items()}
                 for key, group in groups.items()}
    print("Matched window samples:", len(complete), "outside:", len(outside),
          "abandoned:", len(abandoned), "pending at trace end:", len(pending))
    print("Window register snapshots:", sum("alignment-args" in row for row in complete),
          "(fault-time registers, not function-entry arguments)")
    for key, group in summaries.items():
        print(key)
        for category, values in group.items():
            print(" ", category, values)
    print("Sampled post-restore fault -> original exception-return helper; wall includes waits.")
    print("1/257 systematic sampling. Preserve unmatched/cross-frame evidence; no speedup implied.")
    if args.json:
        args.json.write_text(json.dumps(dict(window=audio["window"],
                                              engine_pid=audio["engine_pid"], sample_period=257,
                                              summaries=summaries, samples=complete,
                                              outside_window=outside, abandoned=abandoned,
                                              pending_at_trace_end=list(pending.values())), indent=2) + "\n")


if __name__ == "__main__":
    main()
