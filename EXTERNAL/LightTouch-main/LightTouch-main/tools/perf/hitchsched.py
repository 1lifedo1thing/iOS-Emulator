#!/usr/bin/env python3
"""Align published frame gaps with a Perfetto trace's vCPU scheduler states.

Requires Perfetto trace_processor_shell (pass --trace-processor). No phone access.
Running includes guest and native emulator work; sleeping does not identify a lock.
"""
import argparse
import bisect
import csv
import io
import json
from pathlib import Path
import statistics
import subprocess

from framestats import analyse, parse


def runnable_details(query, hitches, states, vcpu, minimum_ms):
    """Inspect the destination CPU during long runnable parts of hitch gaps.

    An idle task on that CPU rules out another scheduled task occupying it;
    it does not identify why a wakeup or idle exit was delayed.
    """
    waits = []
    starts = [r["ts"] for r in states]
    for hitch in hitches:
        for event in hitch["vcpu"]["events"]:
            left, right = event["overlap_begin_ns"], event["overlap_end_ns"]
            if event["state"] not in ("R", "R+") or right - left < minimum_ms * 1e6:
                continue
            index = bisect.bisect_left(starts, event["end_ns"])
            following = states[index] if index < len(states) else None
            next_cpu = (int(following["cpu"]) if following and
                        following["ts"] == event["end_ns"] and
                        following["state"] == "Running" else None)
            waits.append(dict(hitch_end_s=hitch["relative_end_s"], state=event,
                              next_running_cpu=next_cpu, begin_ns=left, end_ns=right))
    if not waits:
        return dict(minimum_ms=minimum_ms, waits=[])
    begin = min(w["state"]["ts"] for w in waits)
    end = max(w["end_ns"] for w in waits)
    wakes = query("SELECT ts,cpu,name,EXTRACT_ARG(arg_set_id,'target_cpu') AS target_cpu,"
                  "EXTRACT_ARG(arg_set_id,'success') AS success FROM ftrace_event "
                  f"WHERE ts>={begin} AND ts<{end} AND name IN ('sched_waking','sched_wakeup') "
                  f"AND EXTRACT_ARG(arg_set_id,'pid')={int(vcpu['tid'])} ORDER BY ts")
    wake_starts = [int(w["ts"]) for w in wakes]
    cpus = set()
    for wait in waits:
        event = wait["state"]
        lo = bisect.bisect_left(wake_starts, event["ts"])
        hi = bisect.bisect_left(wake_starts, event["end_ns"])
        wait["wake_events"] = wakes[lo:hi]
        initial = [w for w in wakes[lo:hi] if int(w["ts"]) == event["ts"]]
        wait["wake_target_cpu"] = (int(initial[0]["target_cpu"]) if initial and
                                   initial[0]["target_cpu"] != '[NULL]' else None)
        wait["cpus"] = {}
        cpus.update(c for c in (wait["wake_target_cpu"], wait["next_running_cpu"])
                    if c is not None)
    if not cpus:
        return dict(minimum_ms=minimum_ms, waits=waits)
    cpu_list = ','.join(str(c) for c in sorted(cpus))
    slices = query("SELECT s.ts,s.dur,s.cpu,s.priority,t.tid,t.name,p.pid,p.name AS process "
                   "FROM sched s LEFT JOIN thread t USING(utid) LEFT JOIN process p USING(upid) "
                   f"WHERE s.cpu IN ({cpu_list}) AND s.ts<{end} "
                   f"AND (s.dur=-1 OR s.ts+s.dur>{begin}) ORDER BY s.cpu,s.ts")
    idle = query("SELECT ts,cpu,EXTRACT_ARG(arg_set_id,'state') AS state FROM ftrace_event "
                 f"WHERE name='cpu_idle' AND cpu IN ({cpu_list}) AND ts>={begin} "
                 f"AND ts<{end} ORDER BY ts")
    by_cpu = {c: [s for s in slices if int(s['cpu']) == c] for c in cpus}
    for wait in waits:
        left, right = wait["begin_ns"], wait["end_ns"]
        for core in {wait["wake_target_cpu"], wait["next_running_cpu"]} - {None}:
            rows, totals, previous_end = [], dict(idle_task_ms=0.0, other_tasks_ms=0.0,
                                                vcpu_ms=0.0), left
            for raw in by_cpu[core]:
                ts, dur = int(raw['ts']), int(raw['dur'])
                stop = ts + dur if dur >= 0 else end
                a, b = max(left, ts), min(right, stop)
                if b <= a:
                    continue
                if dur < 0 or a < previous_end:
                    raise ValueError('Incomplete/overlapping destination CPU scheduler slices')
                previous_end = b
                key = ('idle_task_ms' if raw['tid'] == '0' else
                       'vcpu_ms' if raw['tid'] == vcpu['tid'] else 'other_tasks_ms')
                totals[key] += (b-a)/1e6
                rows.append(dict(raw, overlap_begin_ns=a, overlap_end_ns=b))
            totals['unknown_ms'] = (right-left)/1e6-sum(totals.values())
            wait['cpus'][str(core)] = dict(totals, sched=rows,
                idle_events=[e for e in idle if int(e['cpu']) == core and
                             left <= int(e['ts']) < right])
    return dict(minimum_ms=minimum_ms, waits=waits)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("trace", type=Path)
    ap.add_argument("frames", type=Path)
    ap.add_argument("--engine-pid", type=int, required=True)
    ap.add_argument("--vcpu-tid", type=int)
    ap.add_argument("--trace-processor", default="trace_processor_shell")
    ap.add_argument("--secs", type=float, default=60)
    ap.add_argument("--hitch-ms", type=float, default=50)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--chart", type=Path)
    ap.add_argument("--frequency", action="store_true",
                    help="also align recorded CPU frequencies with running intervals")
    ap.add_argument("--all-states", action="store_true",
                    help="retain whole-window vCPU state intervals for address-space residency analysis")
    ap.add_argument("--runnable-details", action="store_true",
                    help="inspect wake events and destination CPU occupancy during hitch runnable waits")
    ap.add_argument("--runnable-min-ms", type=float, default=1.0,
                    help="minimum in-hitch runnable interval for --runnable-details (default: 1 ms)")
    ap.add_argument("--allow-superseded-initial-state", action="store_true",
                    help="report/exclude an open trace-initial row only if superseded before the frame window")
    args = ap.parse_args()
    if args.secs <= 0 or args.hitch_ms <= 0 or args.runnable_min_ms <= 0:
        ap.error("duration and hitch threshold must be positive")

    def query(sql):
        result = subprocess.run([args.trace_processor, "query", str(args.trace), sql],
                                text=True, capture_output=True, check=True)
        return list(csv.DictReader(io.StringIO(result.stdout)))

    errors = query("SELECT name,idx,severity,value FROM stats WHERE value>0 "
                   "AND severity IN ('error','data_loss')")
    if errors:
        ap.error(f"trace reports errors or data loss: {errors}")
    clocks = query("SELECT ts,clock_value FROM clock_snapshot "
                   "WHERE clock_name='MONOTONIC' AND machine_id=0")
    if not clocks:
        ap.error("trace has no local MONOTONIC clock snapshots")
    offsets = [int(r["ts"]) - int(r["clock_value"]) for r in clocks]
    offset = int(statistics.median(offsets))
    spread = max(offsets) - min(offsets)
    if spread > 1_000_000:
        ap.error("clock offset changed by over 1 ms; cannot use a constant alignment")
    frames = parse(args.frames.read_text())
    if len(frames) < 2:
        ap.error("fewer than two published frames")
    if any(y[0] <= x[0] or y[1] <= x[1] for x, y in zip(frames, frames[1:])):
        ap.error("frame timestamps and sequences must strictly increase")
    begin = frames[0][0]
    end = begin + int(args.secs * 1e9)
    frames = [r for r in frames if begin <= r[0] < end]
    if len(frames) < 2:
        ap.error("fewer than two published frames in the selected window")
    a, b = begin + offset, end + offset
    summary = query("SELECT t.utid,t.tid,t.name,SUM(s.dur) AS running_ns "
                    "FROM thread t JOIN process p USING(upid) "
                    "JOIN thread_state s USING(utid) "
                    f"WHERE p.pid={args.engine_pid} AND s.state='Running' AND s.dur>0 "
                    "GROUP BY t.utid ORDER BY running_ns DESC")
    candidates = [r for r in summary if int(r["tid"]) == args.vcpu_tid] if args.vcpu_tid else [
        r for r in summary if r["name"] == "CPU 0/TCG"]
    if not candidates:
        ap.error("no matching vCPU thread; supply the engine PID and optionally vCPU TID")
    cpu = candidates[0]  # driver workers can inherit this name: select most CPU time
    states = query("SELECT s.ts,s.dur,s.state,s.cpu,s.blocked_function,w.tid AS waker_tid,"
                   "w.name AS waker_name FROM thread_state s LEFT JOIN thread w "
                   f"ON s.waker_utid=w.utid WHERE s.utid={int(cpu['utid'])} "
                   f"AND s.ts<{b} AND (s.dur=-1 OR s.ts+s.dur>{a}) ORDER BY s.ts")
    intervals, superseded_initial = [], []
    for row in states:
        ts, dur = int(row["ts"]), int(row["dur"])
        if dur < 0:
            if args.allow_superseded_initial_state and ts < a and not superseded_initial:
                initial = query("SELECT ts,dur,state FROM thread_state "
                                f"WHERE utid={int(cpu['utid'])} ORDER BY ts LIMIT 2")
                if (len(initial) == 2 and int(initial[0]["ts"]) == ts and
                        int(initial[0]["dur"]) == -1 and int(initial[1]["dur"]) > 0 and
                        int(initial[1]["ts"]) + int(initial[1]["dur"]) <= a):
                    superseded_initial.append(dict(raw=row, next_observed_state=initial[1],
                                                    inferred_end_ns=int(initial[1]["ts"])))
                    continue
            ap.error("incomplete scheduler interval in the frame window")
        row.update(ts=ts, end_ns=ts + dur)
        if intervals and ts < intervals[-1]["end_ns"]:
            ap.error("scheduler states overlap")
        intervals.append(row)
    starts = [r["ts"] for r in intervals]
    frequencies = {}
    if args.frequency:
        rows = query("SELECT c.ts,t.cpu,c.value FROM counter c "
                     "JOIN cpu_counter_track t ON c.track_id=t.id "
                     f"WHERE t.name='cpufreq' AND c.ts<{b} ORDER BY c.ts")
        for row in rows:
            frequencies.setdefault(row["cpu"], []).append(
                (int(row["ts"]), float(row["value"])))
        if not frequencies:
            ap.error("trace has no cpufreq counters")
    frequency_starts = {cpu: [r[0] for r in rows] for cpu, rows in frequencies.items()}

    def running_frequency(cpu, left, right, totals):
        rows = frequencies.get(cpu, [])
        index = bisect.bisect_right(frequency_starts.get(cpu, []), left) - 1
        while left < right:
            next_ts = rows[index + 1][0] if index + 1 < len(rows) else right
            stop = min(right, next_ts)
            value = rows[index][1] if index >= 0 else None
            key = (cpu, value)
            totals[key] = totals.get(key, 0) + stop - left
            left = stop
            index += 1

    def segment(start, end, details=False):
        index = max(0, bisect.bisect_right(starts, start) - 1)
        totals, events, freq = {}, [], {}
        while index < len(intervals) and intervals[index]["ts"] < end:
            row = intervals[index]
            left, right = max(start, row["ts"]), min(end, row["end_ns"])
            if right > left:
                state = row["state"]
                totals[state] = totals.get(state, 0) + right - left
                if args.frequency and state == "Running":
                    running_frequency(row["cpu"], left, right, freq)
                if details:
                    events.append(dict(row, overlap_begin_ns=left, overlap_end_ns=right))
            index += 1
        result = dict(ms={k: v / 1e6 for k, v in totals.items()}, events=events)
        if args.frequency:
            result["running_frequency"] = [dict(cpu=cpu, khz=khz, ms=ns / 1e6)
                                           for (cpu, khz), ns in sorted(
                                               freq.items(), key=lambda r: str(r[0]))]
        return result

    window = segment(a, b)
    if abs(sum(window["ms"].values()) * 1e6 - (b - a)) > 1000:
        ap.error("scheduler states do not cover the complete frame window")
    hitches, classes = [], {}
    for first, last in zip(frames, frames[1:]):
        duration = last[0] - first[0]
        skipped = max(0, last[1] - first[1] - 1)
        category = "skipped" if skipped else ("hitch" if duration > args.hitch_ms * 1e6
                                              else "late" if duration > 20e6 else "normal")
        item = segment(first[0] + offset, last[0] + offset,
                       duration > args.hitch_ms * 1e6)
        group = classes.setdefault(category, dict(frames=0, wall_ms=0, state_ms={},
                                                 frequency_ms={}))
        group["frames"] += 1
        group["wall_ms"] += duration / 1e6
        for state, ms in item["ms"].items():
            group["state_ms"][state] = group["state_ms"].get(state, 0) + ms
        for row in item.get("running_frequency", []):
            key = f"cpu={row['cpu']},khz={row['khz']}"
            group["frequency_ms"][key] = group["frequency_ms"].get(key, 0) + row["ms"]
        if duration > args.hitch_ms * 1e6:
            hitches.append(dict(begin_ns=first[0], end_ns=last[0], ms=duration / 1e6,
                                skipped=skipped,
                                relative_end_s=(last[0] - begin) / 1e9,
                                vcpu=item))
    report = dict(engine_pid=args.engine_pid, vcpu=cpu, engine_threads=summary,
                  clock_offset_ns=offset, clock_offset_spread_ns=spread,
                  clock_snapshots=clocks, trace_errors=errors,
                  window=dict(begin_ns=begin, end_exclusive_ns=end, vcpu=window),
                  frame_classes=classes,
                  frames={k: v for k, v in analyse(frames, secs=args.secs).items()
                          if k in ("mean", "ceiling", "median", "low", "low1",
                                   "hitches", "hitch_ms", "skipped")}, hitches=hitches)
    if args.all_states:
        report["vcpu_states"] = intervals
    if args.runnable_details:
        report['runnable_details'] = runnable_details(query, hitches, intervals, cpu,
                                                      args.runnable_min_ms)
    report["superseded_initial_states"] = superseded_initial
    if superseded_initial:
        print("WARNING: open trace-initial state superseded before the frame window:", superseded_initial)
    print(f"Selected vCPU TID {cpu['tid']}; clock offset {offset} ns, spread {spread} ns.")
    print("No trace error/data-loss counters; complete scheduler coverage of frame window.")
    print("vCPU window ms:", window["ms"])
    for hitch in sorted(hitches, key=lambda r: -r["ms"]):
        print(f"{hitch['relative_end_s']:.3f}s gap {hitch['ms']:.3f} ms: " +
              ", ".join(f"{k} {v:.3f}" for k, v in hitch["vcpu"]["ms"].items()))
    print("Running includes guest/native work. R/R+ is runnable; S/D does not identify a lock.")
    if args.runnable_details:
        details = report['runnable_details']
        totals = dict(idle_task_ms=0.0, other_tasks_ms=0.0, vcpu_ms=0.0, unknown_ms=0.0)
        missing = 0
        for wait in details['waits']:
            core = wait['wake_target_cpu']
            core = wait['next_running_cpu'] if core is None else core
            if core is None:
                missing += 1
                continue
            for key in totals:
                totals[key] += wait['cpus'][str(core)][key]
        print(f"Hitch runnable intervals >= {args.runnable_min_ms:g} ms: "
              f"{len(details['waits'])}; missing destination: {missing}; CPU occupancy ms: {totals}")
        print("Destination occupancy does not identify interrupt work or prove idle-exit causation.")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")
    if args.chart:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(10, 4))
        bottom = [0.0] * len(hitches)
        categories = [("Running", ("Running",)), ("Runnable", ("R", "R+")),
                      ("Sleeping/blocked", None)]
        for label, names in categories:
            values = [sum(v for k, v in h["vcpu"]["ms"].items()
                          if k in names) if names else sum(
                              v for k, v in h["vcpu"]["ms"].items()
                              if k not in ("Running", "R", "R+")) for h in hitches]
            ax.bar(range(len(hitches)), values, bottom=bottom, label=label)
            bottom = [a + b for a, b in zip(bottom, values)]
        ax.set_xticks(range(len(hitches)), [f"{h['relative_end_s']:.2f}" for h in hitches])
        ax.set_xlabel("Frame gap end (seconds from frame window start)")
        ax.set_ylabel("vCPU scheduler time (ms)")
        ax.set_title("Scheduler states during long published-frame gaps")
        ax.legend()
        fig.tight_layout()
        fig.savefig(args.chart, dpi=130)


if __name__ == "__main__":
    main()
