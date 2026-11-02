#!/usr/bin/env python3
"""Summarize LT_EXCEPTION_PROF snapshots from an engine log (CPU 0, AArch32).

Host dispatch times exclude guest handlers, earlier BQL waits and log output.
SVC r12 values are raw guest registers. Mach message counters count iOS 3 trap
attempts, not delivered messages. No per-process attribution is available.
Kernel episodes run from an exception taken in user mode to the return to user
mode, in the vCPU thread's CPU time (idle WFI and host preemption excluded); a
blocking call's episode includes the thread switch and whatever kernel work runs
before user code does again.
"""
import argparse
import json
from pathlib import Path

NAMES = {1: "undefined", 2: "SVC", 3: "prefetch abort", 4: "data abort",
         5: "IRQ", 6: "FIQ", 7: "breakpoint"}


def snapshots(path):
    result, current = [], None
    for line in path.read_text(errors="replace").splitlines():
        if "[exception-prof] " not in line:
            continue
        parts = line.split("[exception-prof] ", 1)[1].split()
        fields = dict(zip(parts[1::2], parts[2::2]))
        if parts[0] == "snapshot":
            current = {"now": int(fields["now-ns"]),
                       "elapsed": int(fields["elapsed-ns"]),
                       "exceptions": {}, "svc": {}, "aborts": {}, "mach": {},
                       "episodes": {}, "cpu": None}
        elif current is not None:
            if parts[0] == "exception":
                current["exceptions"][fields["id"]] = {
                    k: int(fields[k]) for k in ("n", "ns", "max-ns")}
            elif parts[0] == "svc":
                current["svc"][fields["r12"]] = int(fields["n"])
            elif parts[0] == "data-abort":
                current["aborts"][fields["status"]] = int(fields["n"])
            elif parts[0] == "mach-msg":
                current["mach"] = {k: int(v) for k, v in fields.items()}
            elif parts[0] == "thread-cpu-ns":
                current["cpu"] = int(parts[1])
            elif parts[0] == "kernel-episode":
                current["episodes"][f"{parts[1]} {parts[2]}"] = (
                    int(fields["n"]), int(fields["cpu-ns"]))
            elif parts[0] == "end" and int(fields["now-ns"]) == current["now"]:
                result.append(current)
                current = None
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--last", type=float, help="final N seconds; default 30")
    group.add_argument("--start", type=float, help="seconds after first exception")
    ap.add_argument("--end", type=float, help="seconds after first exception")
    ap.add_argument("--frames", type=Path, help="full LT_FRAME_LOG covering the interval")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    rows = snapshots(args.log)
    if len(rows) < 2:
        ap.error("need two complete exception snapshots")
    origin = rows[0]["now"] - rows[0]["elapsed"]
    end = origin + args.end * 1e9 if args.end is not None else rows[-1]["now"]
    candidates = [r for r in rows if r["now"] <= end]
    if not candidates:
        ap.error("end precedes the first snapshot")
    b = candidates[-1]
    start = (origin + args.start * 1e9 if args.start is not None else
             b["now"] - (args.last if args.last is not None else 30) * 1e9)
    a = next((r for r in reversed(rows) if r["now"] <= start), rows[0])
    seconds = (b["now"] - a["now"]) / 1e9
    if seconds <= 0:
        ap.error("selected interval needs two different snapshots")

    def delta(end_value, start_value):
        value = end_value - start_value
        if value < 0:
            ap.error("counters reset inside the selected interval")
        return value

    frames = None
    if args.frames:
        times = []
        for line in args.frames.read_text().splitlines():
            parts = line.split()
            if len(parts) in (2, 11) and all(p.lstrip("-").isdigit() for p in parts):
                times.append(int(parts[0]))
        if not times or times[0] > a["now"] or times[-1] < b["now"]:
            ap.error("frame log must cover both selected snapshot timestamps")
        frames = sum(a["now"] < t <= b["now"] for t in times)
    report = {"start_ns": a["now"], "end_ns": b["now"], "seconds": seconds,
              "start_seconds": (a["now"] - origin) / 1e9,
              "end_seconds": (b["now"] - origin) / 1e9, "frames": frames,
              "exceptions": [], "svc": {}, "abort_status": {}, "mach_attempts": {}}
    for exc, end_values in b["exceptions"].items():
        initial = a["exceptions"].get(exc, {})
        n = delta(end_values["n"], initial.get("n", 0))
        ns = delta(end_values["ns"], initial.get("ns", 0))
        if n:
            report["exceptions"].append({
                "id": int(exc), "name": NAMES.get(int(exc), exc), "n": n,
                "per_second": n / seconds, "per_frame": n / frames if frames else None,
                "host_ms": ns / 1e6, "mean_us": ns / n / 1e3,
                "cumulative_max_us": end_values["max-ns"] / 1e3})
    for kind, key in (("svc", "svc"), ("aborts", "abort_status"),
                      ("mach", "mach_attempts")):
        for name, end_value in b[kind].items():
            n = delta(end_value, a[kind].get(name, 0))
            if n:
                report[key][name] = {"n": n, "per_second": n / seconds,
                                     "per_frame": n / frames if frames else None}
    print(f"Exception profile CPU 0: {report['start_seconds']:.3f}–"
          f"{report['end_seconds']:.3f} s ({seconds:.3f} s), frames={frames}")
    print("Instrumented host setup; excludes guest handlers, earlier BQL waits and log output.")
    for row in sorted(report["exceptions"], key=lambda r: -r["host_ms"]):
        per_frame = f", {row['per_frame']:.2f}/frame" if row["per_frame"] is not None else ""
        print(f"  {row['name']:16} {row['per_second']:9.1f}/s{per_frame}: "
              f"{row['mean_us']:.3f} us/entry, {row['host_ms']:.2f} ms total; "
              f"cumulative max {row['cumulative_max_us']:.1f} us")
    for key in ("svc", "abort_status", "mach_attempts"):
        print(key + ":")
        for name, row in sorted(report[key].items(), key=lambda kv: -kv[1]["n"])[:30]:
            print(f"  {name:18} {row['n']:9d} {row['per_second']:9.1f}/s")
    if a["cpu"] is not None and b["cpu"] is not None:
        cpu = delta(b["cpu"], a["cpu"])
        report["vcpu_busy"] = cpu / 1e9 / seconds
        report["kernel_episodes"] = {}
        for name, (n1, ns1) in b["episodes"].items():
            n0, ns0 = a["episodes"].get(name, (0, 0))
            if n1 - n0:
                report["kernel_episodes"][name] = {
                    "n": n1 - n0, "per_second": (n1 - n0) / seconds,
                    "mean_us": (ns1 - ns0) / (n1 - n0) / 1e3, "share": (ns1 - ns0) / cpu}
        total = sum(r["share"] for r in report["kernel_episodes"].values())
        print(f"vCPU thread busy {100 * report['vcpu_busy']:.1f}%; kernel episodes "
              f"{100 * total:.1f}% of it:")
        for name, row in sorted(report["kernel_episodes"].items(),
                                key=lambda kv: -kv[1]["share"])[:20]:
            print(f"  {name:18} {100 * row['share']:6.2f}% {row['per_second']:9.1f}/s "
                  f"{row['mean_us']:8.1f} us")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
