#!/usr/bin/env python3
"""Summarize complete LT_GLES_PRESENT_PROF blocks, preserving slowest calls.

Wall times include clocks and descheduling. Acquire includes GPU completion and
mapping; mapped copy includes row swizzling. No per-draw/full accounting needed.
"""
import argparse
import json
from pathlib import Path

PHASES = ("prep", "acquire", "readback", "convert", "copy", "tail")


def blocks(path):
    result, pending = [], {}
    for line in path.read_text(errors="replace").splitlines():
        if "[gles-present] " not in line:
            continue
        parts = line.split("[gles-present] ", 1)[1].split()
        if len(parts) % 2 != 1:
            continue
        fields = dict(zip(parts[1::2], parts[2::2]))
        try:
            row = {key: int(value) for key, value in fields.items()}
            bounds = row["begin-ns"], row["end-ns"]
            if parts[0].startswith("path="):
                name = parts[0].split("=", 1)[1]
                if sum(row[f"{p}-ns"] for p in PHASES) != row["total-ns"]:
                    raise ValueError("phase totals do not match")
                if sum(row[f"worst-{p}-ns"] for p in PHASES) != row["max-total-ns"]:
                    raise ValueError("slowest call phase totals do not match")
                if not 0 <= row["retries"] <= row["errors"] <= row["calls"]:
                    raise ValueError("invalid outcome counts")
                paths = pending.setdefault(bounds, {})
                if name in paths:
                    raise ValueError("duplicate path in a timing block")
                paths[name] = row
            elif parts[0] == "end" and bounds in pending:
                result.append(dict(begin_ns=bounds[0], end_ns=bounds[1],
                                   paths=pending.pop(bounds)))
        except KeyError:
            continue  # partial line or an incompatible log schema
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
        ap.error("no complete presentation blocks")
    end = args.end_ns if args.end_ns is not None else rows[-1]["end_ns"]
    start = args.start_ns if args.start_ns is not None else end - args.last * 1e9
    selected = [r for r in rows if start <= r["begin_ns"] and r["end_ns"] <= end]
    if not selected:
        ap.error("no complete blocks wholly inside the requested window")
    if any(a["end_ns"] != b["begin_ns"] for a, b in zip(selected, selected[1:])):
        ap.error("timing blocks have a gap or reset; select one contiguous interval")
    seconds = (selected[-1]["end_ns"] - selected[0]["begin_ns"]) / 1e9
    if seconds <= 0:
        ap.error("nonpositive timing interval")
    report = dict(begin_ns=selected[0]["begin_ns"], end_ns=selected[-1]["end_ns"],
                  seconds=seconds, blocks=selected, paths={})
    print(f"Presentation wall timings: {report['begin_ns']}–{report['end_ns']} "
          f"({seconds:.3f} s, {len(selected)} complete blocks).")
    for name in sorted({name for r in selected for name in r["paths"]}):
        paths = [r["paths"][name] for r in selected if name in r["paths"]]
        summed = {key: sum(r[key] for r in paths) for key in
                  ("calls", "errors", "retries", "total-ns") +
                  tuple(f"{p}-ns" for p in PHASES)}
        worst = max(paths, key=lambda r: r["max-total-ns"])
        calls = summed["calls"]
        report["paths"][name] = dict(totals=summed, worst_call=worst,
            phase_max_ns={p: max(r[f"max-{p}-ns"] for r in paths) for p in PHASES})
        print(f"{name}: {calls} attempts ({calls / seconds:.1f}/s), "
              f"{summed['errors']} errors including {summed['retries']} fault retries; "
              f"mean {summed['total-ns'] / calls / 1e3:.2f} us, "
              f"{summed['total-ns'] / seconds / 1e7:.3f}% wall")
        print(f"  slowest {worst['max-total-ns'] / 1e3:.2f} us, "
              f"ends at {worst['max-end-ns']} ns")
        for phase in PHASES:
            print(f"  {phase:8}: mean {summed[f'{phase}-ns'] / calls / 1e3:.2f} us; "
                  f"slowest-call component {worst[f'worst-{phase}-ns'] / 1e3:.2f} us")
    print("Diagnostic FPS is not an ordinary baseline. Max phases may belong to different calls.")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
