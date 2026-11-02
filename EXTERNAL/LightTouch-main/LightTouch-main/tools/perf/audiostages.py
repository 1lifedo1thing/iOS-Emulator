#!/usr/bin/env python3
"""Attribute pinned AAC/converter boundaries to frame and compositor intervals.

Consumes real-capture guest-points, hitches (--all-states), swap-report,
queue-summary and milestones JSON from one worker. Queue reports preserve the serial derivation
and association limits described in PROFILING.md. No device access.
"""

import argparse
import bisect
import collections
import hashlib
import json
from pathlib import Path
import statistics


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("capture", type=Path)
    ap.add_argument("--json", type=Path, required=True)
    args = ap.parse_args()
    names = (
        "guest-points.json",
        "hitches.json",
        "swap-report.json",
        "queue-summary.json",
        "milestones.json",
        "surfaces.json",
    )
    raw = {name: (args.capture / name).read_bytes() for name in names}
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in raw.items()}
    g, h, api, queue, milestones, surfaces = (json.loads(raw[name]) for name in names)
    begin, end = (g["window"][k] for k in ("begin_ns", "end_exclusive_ns"))
    if (
        len({x["engine_pid"] for x in (g, h, api, queue)}) != 1
        or any(x["window"] != g["window"] for x in (api, queue, surfaces))
        or (h["window"]["begin_ns"], h["window"]["end_exclusive_ns"]) != (begin, end)
        or h.get("trace_errors")
        or not h.get("vcpu_states")
    ):
        ap.error("need one worker/window and complete scheduler states")
    for report in (api, queue):
        for name, digest in report["input_sha256"].items():
            if hashlib.sha256((args.capture / name).read_bytes()).hexdigest() != digest:
                ap.error("input identity mismatch: " + name)
    points = {p["pc"]: p for p in api["config"]["points"]}
    expected = {
        0x303DDB34: ("aac-output", "entry"),
        0x303DDD08: ("aac-output", "exit"),
        0x303F4D98: ("audio-convert", "entry"),
        0x303F4F44: ("audio-convert", "exit"),
    }
    if any(
        pc not in points or (points[pc]["name"], points[pc]["role"]) != meta
        for pc, meta in expected.items()
    ):
        ap.error("need pinned AAC/converter entry and epilogue points")
    rows = sorted(g["samples"] + g["outside_window"], key=lambda r: r["begin_ns"])
    changes = [r for r in rows if points[r["pc"]]["role"] == "switch"]
    times = [r["begin_ns"] for r in changes]
    for row in rows:
        i = bisect.bisect_right(times, row["begin_ns"]) - 1
        if i >= 0 and changes[i]["thread"] != row["thread"]:
            ap.error("guest switch continuity mismatch")
    states = h["vcpu_states"]
    offset = h["clock_offset_ns"]
    st = [r["ts"] - offset for r in states]

    def split(a, b):
        a, b = max(a, begin), min(b, end)
        out = collections.Counter()
        cursor = a
        i = max(0, bisect.bisect_right(st, a) - 1)
        while i < len(states) and st[i] < b:
            lo, hi = max(a, st[i]), min(b, states[i]["end_ns"] - offset)
            if hi > lo:
                if lo != cursor:
                    raise ValueError("host scheduler gap/overlap")
                out[states[i]["state"]] += (hi - lo) / 1e6
                cursor = hi
            i += 1
        if b > a and cursor != b:
            raise ValueError("incomplete host scheduler coverage")
        return dict(out)

    def residency(a, b):
        a, b = max(a, begin), min(b, end)
        out = collections.defaultdict(collections.Counter)
        i = bisect.bisect_right(times, a) - 1
        if b > a and i < 0:
            raise ValueError("missing guest thread at interval start")
        while i < len(changes) and times[i] < b:
            lo = max(a, times[i])
            hi = min(b, times[i + 1] if i + 1 < len(times) else b)
            if hi > lo:
                out[changes[i]["thread"]].update(split(lo, hi))
            i += 1
        return out

    calls = {
        name: sorted(
            [c for c in api["calls"] if c["name"] == name], key=lambda c: c["begin_ns"]
        )
        for name in ("aac-output", "audio-convert")
    }
    window_calls = {
        name: [c for c in cs if begin <= c["begin_ns"] < c["end_ns"] < end]
        for name, cs in calls.items()
    }
    if not all(window_calls.values()):
        ap.error("no complete in-window AAC/converter calls")
    for cs in calls.values():
        last = {}
        for c in cs:
            if c["begin_ns"] < last.get(c["thread"], 0):
                ap.error("recursive/overlapping audio calls are unsupported")
            last[c["thread"]] = c["end_ns"]

    def work(a, b, name):
        out = []
        for c in calls[name]:
            if c["begin_ns"] >= b:
                break
            lo, hi = max(a, c["begin_ns"]), min(b, c["end_ns"])
            if hi > lo:
                selected = residency(lo, hi).get(c["thread"], {})
                out.append(
                    dict(
                        call_id=c["id"],
                        thread=c["thread"],
                        begin_ns=lo,
                        end_ns=hi,
                        selected_host_ms=dict(selected),
                    )
                )
        return dict(
            selected_running_ms=sum(
                x["selected_host_ms"].get("Running", 0) for x in out
            ),
            calls=out,
        )

    def interval(a, b):
        return dict(
            begin_ns=a,
            end_ns=b,
            ms=(b - a) / 1e6,
            covered_ms=max(0, min(b, end) - max(a, begin)) / 1e6,
            outside_ms=((b - a) - max(0, min(b, end) - max(a, begin))) / 1e6,
            host_ms=split(a, b),
            guest_threads=[
                dict(thread=t, owner=api["owners"].get(str(t)), host_ms=dict(v))
                for t, v in sorted(
                    residency(a, b).items(), key=lambda x: -x[1].get("Running", 0)
                )
            ],
            audio={name: work(a, b, name) for name in calls},
        )

    def stats(cs):
        wall = [c["wall_ms"] for c in cs]
        running = [
            c["guest_threads"].get(str(c["thread"]), {}).get("Running", 0) for c in cs
        ]
        return dict(
            calls=len(cs),
            median_wall_ms=statistics.median(wall) if wall else None,
            median_selected_running_ms=statistics.median(running) if running else None,
            max_wall_ms=max(wall, default=None),
            selected_running_ms=sum(running),
        )

    converter = []
    for c in window_calls["audio-convert"]:
        parents = [
            p
            for p in calls["aac-output"]
            if p["thread"] == c["thread"]
            and p["begin_ns"] <= c["begin_ns"] < c["end_ns"] <= p["end_ns"]
        ]
        converter.append(
            dict(
                call_id=c["id"],
                begin_ns=c["begin_ns"],
                end_ns=c["end_ns"],
                thread=c["thread"],
                output=c["entry"]["r1"],
                scalars=c["entry"]["r3"],
                scale_bits=c["entry"]["r2"],
                parent_aac_ids=[p["id"] for p in parents],
                wall_ms=c["wall_ms"],
                selected_running_ms=c["guest_threads"]
                .get(str(c["thread"]), {})
                .get("Running", 0),
            )
        )
    aligned = {
        str(alignment): stats(
            [
                c
                for c in window_calls["audio-convert"]
                if c["entry"]["r1"] % 4 == alignment
            ]
        )
        for alignment in range(4)
    }
    frame_rows = []
    for frame in g["frames"]:
        r = interval(frame["begin_ns"], frame["end_ns"])
        r["category"] = frame["category"]
        frame_rows.append(r)
    classes = {}
    for category in sorted({r["category"] for r in frame_rows}):
        rs = [r for r in frame_rows if r["category"] == category]
        host = collections.Counter()
        for r in rs:
            host.update(r["host_ms"])
        classes[category] = dict(frames=len(rs), host_ms=dict(host), audio={})
        for name in calls:
            running = sum(r["audio"][name]["selected_running_ms"] for r in rs)
            classes[category]["audio"][name] = dict(
                frames_overlapping=sum(bool(r["audio"][name]["calls"]) for r in rs),
                selected_running_ms=running,
                running_share_percent=(
                    100 * running / host["Running"] if host["Running"] else 0
                ),
            )
    stages = []
    for flip in queue["flips"]:
        if not flip["ticks"]:
            continue
        stages.append(
            dict(
                flip_id=flip["flip_id"],
                flip_ns=flip["ns"],
                ticks=flip["ticks"],
                stages=[
                    (
                        dict(name=s["name"], **interval(s["begin_ns"], s["end_ns"]))
                        if s["end_ns"] >= s["begin_ns"]
                        else dict(s, audio=None)
                    )
                    for s in flip["stages"]
                ],
            )
        )
    pending = [r for r in api["pending_entries"] if r["name"] in calls]
    if any(r["begin_ns"] < end for r in pending):
        ap.error("unfinished audio interval overlaps the frame window")
    if milestones["window"] != g["window"] or milestones.get(
        "late_legacy_publication_markers"
    ):
        ap.error("need matching milestone window and ordered publication markers")
    details = {(r["begin_ns"], r["end_ns"]): r for r in milestones["hitches"]}
    presents_by_thread = collections.defaultdict(list)
    for present in surfaces["calls"]:
        association = api["present_associations"].get(str(present["id"]))
        if association and present["result"] == 0:
            key = (association["thread"], present["context"])
            presents_by_thread[key].append(present)
    producer_intervals = []
    for (thread, context), presents in presents_by_thread.items():
        presents.sort(key=lambda c: c["begin_ns"])
        for previous, present in zip(presents, presents[1:]):
            if previous["end_ns"] > present["begin_ns"]:
                ap.error("overlapping presents on the same producer thread/context")
            producer_intervals.append(
                dict(
                    thread=thread,
                    context=context,
                    previous_present_id=previous["id"],
                    present_id=present["id"],
                    begin_ns=previous["end_ns"],
                    end_ns=present["begin_ns"],
                )
            )
    timelines = []
    for i, frame in enumerate(frame_rows):
        if frame["category"] != "hitch":
            continue
        detail = details.get((frame["begin_ns"], frame["end_ns"]))
        if detail is None:
            ap.error("need milestone details for every >50ms frame")
        queries = []
        for query in queue["queries"]:
            if not frame["begin_ns"] <= query["begin_ns"] < frame["end_ns"]:
                continue
            update = (
                queue["updates"][query["update_id"]]
                if query["update_id"] is not None
                else None
            )
            queries.append(
                dict(
                    query,
                    renderer_serial_comparisons=update["serials"] if update else [],
                )
            )
        # Include the lead-in to each present in the gap and its preceding
        # producer interval. Display gaps can begin after the game became late.
        lead_ins = []
        for j, producer in enumerate(producer_intervals):
            if not frame["begin_ns"] <= producer["end_ns"] < frame["end_ns"]:
                continue
            preceding = producer_intervals[j - 1] if j else None
            if preceding and (preceding["thread"], preceding["context"]) != (
                producer["thread"],
                producer["context"],
            ):
                preceding = None
            for span in (preceding, producer):
                if span and not any(
                    x["present_id"] == span["present_id"] for x in lead_ins
                ):
                    lead_ins.append(
                        dict(span, interval=interval(span["begin_ns"], span["end_ns"]))
                    )
        timelines.append(
            dict(
                frame_index=i,
                preceding_frame_indices=list(range(max(0, i - 2), i)),
                engine_gap_ms=detail["engine_gap_ms"],
                frontend_lag_begin_ms=detail["frontend_lag_begin_ms"],
                frontend_lag_end_ms=detail["frontend_lag_end_ms"],
                events=[
                    e
                    for e in detail["events"]
                    if e["kind"]
                    in {
                        "present-begin",
                        "present-end",
                        "lcd-flip",
                        "vsync-begin",
                        "vsync-end",
                        "capture-begin",
                        "publish",
                    }
                ],
                queue_queries=queries,
                producer_lead_ins=lead_ins,
                insert_ids=[
                    c["id"]
                    for c in queue["inserts"]
                    if frame["begin_ns"] <= c["begin_ns"] < frame["end_ns"]
                ],
            )
        )
    report = dict(
        window=g["window"],
        engine_pid=g["engine_pid"],
        input_sha256=hashes,
        functions={name: stats(cs) for name, cs in window_calls.items()},
        converter_alignment=aligned,
        converter_calls=converter,
        classes=classes,
        frames=frame_rows,
        held_flip_stages=stages,
        pending_audio_entries=pending,
        hitch_timelines=timelines,
        limits=queue["limits"]
        + [
            "Function intervals are inclusive; AAC includes conversion. Do not add their totals.",
            "Selected-thread running includes guest kernel/native work while this thread is selected.",
            "Entry/epilogue observations precede their instructions; void converter r0 is not a result.",
            "lcd-flip is a write of the pending framebuffer register, not the later vsync latch or publication.",
            "Producer lead-ins span the previous present end to the next present begin on one thread/context; they are not measured game update/render boundaries.",
            "Overlapping work alone does not prove a scheduling dependency or attainable speedup.",
        ],
    )
    args.json.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {k: report[k] for k in ("functions", "converter_alignment", "classes")},
            indent=2,
        )
    )
    print("Pending audio entries:", len(pending))


if __name__ == "__main__":
    main()
