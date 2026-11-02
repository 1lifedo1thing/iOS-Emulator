#!/usr/bin/env python3
"""Locate publication gaps along guest-present, LCD and frontend milestones.

LT_FRAME_PATH is a bounded memory trace written after clean engine shutdown.
Frame timestamps are host monotonic publication times, not display scanout.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import json
from pathlib import Path
import re

from framestats import analyse, parse


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("events", type=Path)
    ap.add_argument("frames", type=Path)
    ap.add_argument("--secs", type=float, default=60)
    ap.add_argument("--hitch-ms", type=float, default=50)
    ap.add_argument("--allow-late-publication-markers", action="store_true",
                    help="legacy post-unlock markers only; reject affected hitch boundaries")
    ap.add_argument("--residency-intervals", action="store_true",
                    help="retain whole-window address changes and frame intervals for residency comparison")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    lines = args.events.read_text().splitlines()
    if not lines or not lines[0].startswith("# LT_FRAME_PATH 1 "):
        ap.error("unsupported/missing frame-path header")
    footer = re.fullmatch(r"# end records=(\d+) dropped=(\d+) incomplete=(\d+) end-ns=(\d+)",
                          lines[-1])
    if not footer or int(footer[2]) or int(footer[3]):
        ap.error("missing completion footer or dropped/incomplete milestones")
    events = []
    for line in lines[1:-1]:
        fields = line.split()
        if len(fields) != 5 or not all(fields[i].isdigit() for i in (0, 2, 3, 4)):
            ap.error(f"invalid frame-path record: {line}")
        ns, a, b, c = (int(fields[i]) for i in (0, 2, 3, 4))
        events.append(dict(ns=ns, kind=fields[1], a=a, b=b, c=c))
    if len(events) != int(footer[1]):
        ap.error("footer record count mismatch")
    events.sort(key=lambda e: e["ns"])
    by_kind = defaultdict(list)
    for event in events:
        by_kind[event["kind"]].append(event)
    frames = parse(args.frames.read_text())
    if len(frames) < 2 or args.secs <= 0 or args.hitch_ms <= 0:
        ap.error("need multiple frames and positive duration/threshold")
    begin, end = frames[0][0], frames[0][0] + int(args.secs * 1e9)
    frames = [f for f in frames if f[0] < end]
    if len(frames) < 2 or any(b[0] <= a[0] or b[1] <= a[1]
                              for a, b in zip(frames, frames[1:])):
        ap.error("frame timestamps/sequences must strictly increase")
    if not events or events[0]["ns"] > begin or events[-1]["ns"] < frames[-1][0]:
        ap.error("milestones do not span selected published frames")
    published = {}
    for event in by_kind["publish"]:
        if event["a"] in published:
            ap.error("engine publication sequence reused")
        published[event["a"]] = event
    late_markers = []
    for ns, seq, *_ in frames:
        if seq not in published:
            ap.error(f"frontend sequence {seq} missing from engine publications")
        if published[seq]["ns"] > ns:
            if not args.allow_late_publication_markers:
                ap.error(f"frontend sequence {seq} earlier than engine publication")
            late_markers.append(dict(sequence=seq, frontend_ns=ns,
                                     marker_lateness_ms=(published[seq]["ns"] - ns) / 1e6))
    late_sequences = {m["sequence"] for m in late_markers}

    # Present calls run serially on the vCPU. Keep context and target to avoid
    # pairing different renderers when inspecting multi-context scenes.
    calls, pending = [], {}
    for event in events:
        if event["kind"] == "present-begin":
            key = (event["a"], event["b"])
            if key in pending:
                ap.error("overlapping surface presents for one context/target")
            pending[key] = event
        elif event["kind"] == "present-end":
            start = pending.pop((event["a"], event["b"]), None)
            if start is None:
                ap.error("surface present end without matching begin")
            calls.append(dict(context=event["a"], base=event["b"],
                              begin_ns=start["ns"], end_ns=event["ns"], result=event["c"],
                              ms=(event["ns"] - start["ns"]) / 1e6,
                              width=start["c"] >> 32, height=start["c"] & 0xffffffff))
    if pending:
        ap.error("incomplete surface present call")
    contexts = defaultdict(list)
    for call in sorted(calls, key=lambda c: c["begin_ns"]):
        contexts[call["context"]].append(call)
    address_spaces = [e for e in events if e["kind"] in ("context-switch", "ttbr0")]
    address_times = [e["ns"] for e in address_spaces]

    def address_at(ns):
        index = bisect.bisect_right(address_times, ns) - 1
        return address_spaces[index] if index >= 0 else None

    def address_key(row):
        return f"cidr=0x{row['a']:x},ttbr0=0x{row['b']:x}" if row else "unknown"

    hitches = []
    for first, last in zip(frames, frames[1:]):
        ms = (last[0] - first[0]) / 1e6
        if ms <= args.hitch_ms:
            continue
        if first[1] in late_sequences or last[1] in late_sequences:
            ap.error("cannot attribute a hitch involving a late legacy publication marker")
        left, right = published[first[1]]["ns"], published[last[1]]["ns"]
        selected = [e for e in events if left < e["ns"] <= right]
        detail = dict(begin_ns=first[0], end_ns=last[0], ms=ms,
                      relative_end_s=(last[0] - begin) / 1e9,
                      first_sequence=first[1], last_sequence=last[1],
                      skipped=max(0, last[1] - first[1] - 1),
                      engine_begin_ns=left, engine_end_ns=right,
                      engine_gap_ms=(right - left) / 1e6,
                      frontend_lag_begin_ms=(first[0] - left) / 1e6,
                      frontend_lag_end_ms=(last[0] - right) / 1e6,
                      event_counts=dict(Counter(e["kind"] for e in selected)),
                      events=selected, contexts={})
        detail["address_space_initial"] = address_at(left)
        detail["address_space_changes"] = [e for e in address_spaces
                                           if left < e["ns"] <= last[0]]
        detail["max_vsync_lateness_ms"] = max(
            (e["c"] / 1e6 for e in selected if e["kind"] == "vsync-begin"), default=None)
        detail["flips"] = []
        for flip in (e for e in selected if e["kind"] == "lcd-flip"):
            prior = [c for c in calls if c["end_ns"] <= flip["ns"]]
            last_present = max(prior, key=lambda c: c["end_ns"]) if prior else None
            detail["flips"].append(dict(event=flip, latest_present=last_present,
                                        address_space=address_at(flip["ns"]),
                                        since_present_end_ms=(flip["ns"] - last_present["end_ns"]) / 1e6
                                        if last_present else None))
        for ctx, rows in contexts.items():
            inside = [c for c in rows if c["begin_ns"] < right and c["end_ns"] > left]
            # Adjacent boundary calls distinguish a gap between calls from one
            # long host present; they are not automatically a causal frame chain.
            prior = [c for c in rows if c["end_ns"] <= left]
            later = [c for c in rows if c["begin_ns"] >= right]
            detail["contexts"][ctx] = dict(calls=inside,
                                          previous=prior[-1] if prior else None,
                                          following=later[0] if later else None)
        hitches.append(detail)
    stage_counts = {kind: len([e for e in rows if begin <= e["ns"] < end])
                    for kind, rows in by_kind.items()}
    context_summary = {}
    for ctx, rows in contexts.items():
        chosen = [r for r in rows if begin <= r["begin_ns"] < end]
        if chosen:
            context_summary[ctx] = dict(calls=len(chosen), total_ms=sum(c["ms"] for c in chosen),
                                        max_ms=max(c["ms"] for c in chosen),
                                        results=dict(Counter(c["result"] for c in chosen)),
                                        dimensions=sorted({(c["width"], c["height"]) for c in chosen}),
                                        address_spaces=dict(Counter(
                                            address_key(address_at(c["begin_ns"])) for c in chosen)))
    report = dict(window=dict(begin_ns=begin, end_exclusive_ns=end),
                  frames={k: v for k, v in analyse(frames, secs=args.secs).items()
                          if k in ("mean", "ceiling", "median", "low", "low1", "hitches",
                                   "hitch_ms", "skipped")},
                  stage_counts=stage_counts, contexts=context_summary, hitches=hitches)
    report["late_legacy_publication_markers"] = late_markers
    if args.residency_intervals:
        initial = address_at(begin)
        report["address_space_timeline"] = ([initial] if initial else []) + [
            e for e in address_spaces if begin < e["ns"] < end]
        report["frame_intervals"] = []
        for first, last in zip(frames, frames[1:]):
            duration = last[0] - first[0]
            skipped = max(0, last[1] - first[1] - 1)
            category = "skipped" if skipped else (
                "hitch" if duration > args.hitch_ms * 1e6 else
                "late" if duration > 20e6 else "normal")
            report["frame_intervals"].append(dict(begin_ns=first[0], end_ns=last[0],
                                                  category=category))
    if late_markers:
        print("WARNING: legacy publication markers after frontend copies, outside all hitch boundaries:",
              late_markers)
    print("Frame metrics:", report["frames"])
    print("Stage counts:", stage_counts)
    print("Surface-present contexts:", context_summary)
    for hitch in hitches:
        print(f"{hitch['relative_end_s']:.3f}s: frontend gap {hitch['ms']:.3f} ms, "
              f"engine gap {hitch['engine_gap_ms']:.3f} ms, "
              f"frontend lag {hitch['frontend_lag_begin_ms']:.3f} -> "
              f"{hitch['frontend_lag_end_ms']:.3f} ms; {hitch['event_counts']}")
        for ctx, detail in hitch["contexts"].items():
            print(f"  ctx {ctx}: presents {len(detail['calls'])}, max host call "
                  f"{max((c['ms'] for c in detail['calls']), default=0):.3f} ms")
    print("Present targets, LCD bases and pixel changes are distinct milestones; correlation is not identity.")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
