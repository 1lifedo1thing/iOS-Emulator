#!/usr/bin/env python3
"""Summarize complete LT_GLES_CONTEXT_PROF intervals and retain every block.

Make-current wall time includes driver waits, clocks and host descheduling.
Selected-context changes are engine selections, not measured driver switches.
"""
import argparse
import json
import re
from pathlib import Path

NAMES = {"viewport", "bind-fbo", "present-surface"}
FIELDS = {"n", "switches", "init-ns", "current-ns", "dispatch-ns",
          "max-current-ns", "max-dispatch-ns"}


def blocks(path):
    result, pending = [], {}
    for line in path.read_text(errors="replace").splitlines():
        if "[gles-context] " not in line:
            continue
        text = line.split("[gles-context] ", 1)[1]
        name = text.split()[0]
        row = {key: int(value) for key, value in
               re.findall(r"\b([a-z-]+)=(\d+)\b", text)}
        if name in NAMES:
            if set(row) != FIELDS:
                pending.clear()
                continue
            if name in pending:
                raise ValueError("duplicate context row before completion marker")
            if row["switches"] > row["n"]:
                raise ValueError("context changes exceed call count")
            pending[name] = row
        elif name == "end":
            if set(pending) == NAMES and set(row) == {"begin-ns", "end-ns"}:
                if row["end-ns"] <= row["begin-ns"]:
                    raise ValueError("nonpositive context interval")
                result.append(dict(begin_ns=row["begin-ns"], end_ns=row["end-ns"],
                                   calls=pending))
            pending = {}
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    ap.add_argument("--start-ns", type=int)
    ap.add_argument("--end-ns", type=int)
    ap.add_argument("--last", type=float, default=30)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    rows = blocks(args.log)
    if not rows:
        ap.error("no complete context intervals with exact bounds")
    end = args.end_ns if args.end_ns is not None else rows[-1]["end_ns"]
    start = args.start_ns if args.start_ns is not None else end - args.last * 1e9
    selected = [r for r in rows if start <= r["begin_ns"] and r["end_ns"] <= end]
    if not selected:
        ap.error("no complete blocks wholly inside requested window")
    if any(a["end_ns"] != b["begin_ns"] for a, b in zip(selected, selected[1:])):
        ap.error("timing blocks have a gap or reset; select a contiguous interval")
    seconds = (selected[-1]["end_ns"] - selected[0]["begin_ns"]) / 1e9
    report = dict(begin_ns=selected[0]["begin_ns"], end_ns=selected[-1]["end_ns"],
                  seconds=seconds, blocks=selected, calls={})
    print(f"Context wall timings: {report['begin_ns']}–{report['end_ns']} "
          f"({seconds:.3f} s, {len(selected)} complete blocks).")
    for name in sorted(NAMES):
        calls = [r["calls"][name] for r in selected]
        totals = {key: sum(r[key] for r in calls) for key in
                  ("n", "switches", "init-ns", "current-ns", "dispatch-ns")}
        maxima = {key: max(r[key] for r in calls) for key in
                  ("max-current-ns", "max-dispatch-ns")}
        report["calls"][name] = dict(totals=totals, maxima=maxima)
        count = totals["n"]
        print(f"{name}: {count} calls, {totals['switches']} selected-context changes")
        if count:
            print("  init/current/dispatch us per call: " + " / ".join(
                f"{totals[f'{phase}-ns'] / count / 1e3:.3f}"
                for phase in ("init", "current", "dispatch")))
            print(f"  make-current {totals['current-ns'] / seconds / 1e7:.3f}% wall; "
                  f"max current/dispatch {maxima['max-current-ns'] / 1e3:.3f} / "
                  f"{maxima['max-dispatch-ns'] / 1e3:.3f} us")
    print("Diagnostic FPS is not an ordinary baseline; maxima may be different calls.")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
