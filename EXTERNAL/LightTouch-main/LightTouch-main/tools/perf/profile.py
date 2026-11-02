#!/usr/bin/env python3
"""Summarize an Android Start/Stop profile; optionally pull all saved captures.

ANDROID_SERIAL=... tools/perf/profile.py --pull build/profiles
 tools/perf/profile.py build/profiles/profiles/CAPTURE --chart report.png --json report.json

The capture contains engine publication times, completed GL call aggregates,
slow GL intervals, BQL wait/hold intervals and Android post completion times.
GL durations are host service wall time, not GPU execution time. Correlation
identifies candidates; overlapping counters must not be added as disjoint time.
"""
import argparse
from collections import defaultdict
import json
import re
from pathlib import Path
import subprocess
import tarfile
import tempfile

from framestats import analyse, report


def read_rows(path):
    if not path.exists():
        return []
    return [line.split() for line in path.read_text(errors="replace").splitlines()
            if line and not line.startswith("#")]


def overlap(a, b, start, end):
    return max(0, min(b, end) - max(a, start))


def summarize(directory, top):
    meta_path = directory / "metadata.json"
    meta = json.loads(meta_path.read_text()) if meta_path.exists() else {}
    frames, groups, slow, locks, names = [], [], [], [], {}
    discarded = 0
    events_path = directory / "events.tsv"
    for row in read_rows(events_path):
        try:
            if row[0] == "F" and len(row) == 13:
                frames.append(tuple(map(int, row[1:])))
            elif row[0] == "G" and len(row) == 7:
                values = tuple(map(int, row[1:6])); groups.append(values)
                names[values[1]] = row[6]
            elif row[0] == "S" and len(row) == 5:
                slow.append(tuple(map(int, row[1:])))
            elif row[0] in ("W", "H") and len(row) == 5:
                locks.append((row[0], int(row[1]), int(row[2]), row[3], int(row[4])))
            else:
                discarded += 1
        except ValueError:
            discarded += 1
    # Outliers can complete out of timestamp order across engine threads.
    frames.sort(); groups.sort(); slow.sort(); locks.sort(key=lambda item: item[1])
    presented = []
    for row in read_rows(directory / "presentation.tsv"):
        try:
            if row[0] == "P" and len(row) == 14:
                presented.append(tuple(map(int, row[1:])))
            else:
                discarded += 1
        except ValueError:
            discarded += 1
    presented.sort()
    totals = defaultdict(lambda: [0, 0, 0])
    by_frame = defaultdict(list)
    for end, slot, count, total, maximum in groups:
        t = totals[slot]; t[0] += count; t[1] += total; t[2] = max(t[2], maximum)
        by_frame[end].append((slot, count, total, maximum))
    def call_row(slot, count, total, maximum):
        return dict(slot=slot, name=names.get(slot, str(slot)), count=count,
                    totalMs=total/1e6, maxMs=maximum/1e6)
    ranking = [call_row(slot, *values) for slot, values in
               sorted(totals.items(), key=lambda item: item[1][1], reverse=True)]
    worst = []
    for a, b in sorted(zip(frames, frames[1:]), key=lambda pair: pair[1][0]-pair[0][0], reverse=True)[:top]:
        start, end = a[0], b[0]
        delta = [(y-x)/1e6 for x, y in zip(a, b)]
        worst.append(dict(
            atSeconds=(end-frames[0][0])/1e9, frameMs=delta[0], serial=b[1],
            vcpuRunMs=delta[2], vcpuOutsideMs=delta[3], bqlOutsideMs=delta[4],
            bqlInsideMs=delta[5], vcpuCpuMs=delta[6], completedGlMs=delta[8],
            mainCpuMs=delta[10], publicationAfterDueMs=(b[0]-b[11])/1e6,
            calls=[call_row(*values) for values in sorted(by_frame[end], key=lambda row: row[2], reverse=True)[:8]],
            slowCalls=[dict(name=names.get(slot, str(slot)), slot=slot, context=ctx,
                            totalMs=(finish-begin)/1e6, overlapMs=overlap(begin,finish,start,end)/1e6)
                       for begin,finish,slot,ctx in slow if overlap(begin,finish,start,end)],
            locks=[dict(kind=kind, site=f"{file}:{line}", totalMs=(finish-begin)/1e6,
                        overlapMs=overlap(begin,finish,start,end)/1e6)
                   for kind,begin,finish,file,line in locks if overlap(begin,finish,start,end)]))
    host_worst = []
    for a, b in sorted(zip(presented, presented[1:]), key=lambda pair: pair[1][0]-pair[0][0], reverse=True)[:top]:
        host_worst.append(dict(atSeconds=(b[0]-presented[0][0])/1e9, frameMs=(b[0]-a[0])/1e6,
            serial=b[1], guardWaitMs=(b[4]-b[3])/1e6, copyMs=(b[6]-b[5])/1e6,
            bufferWaitMs=(b[8]-b[7])/1e6, scaleMs=(b[9]-b[8])/1e6,
            pacingMs=(b[10]-b[9])/1e6, postMs=(b[11]-b[10])/1e6,
            commitWaitMs=(b[12]-b[0])/1e6))
    stats = analyse([f[:11] for f in frames])
    if stats: stats["causes"] = None  # Print raw overlapping counters for these captures below.
    host_stats = analyse([(p[0],p[1]) for p in presented])
    # Keep all interval samples in optional JSON for further analysis, excluding
    # framestats' redundant derived arrays in the textual summary.
    footer = events_path.read_text(errors="replace")[-512:] if events_path.exists() else ""
    complete = "# end " in footer
    errors = [int(code) for code in re.findall(r"# end \d+ error (\d+)", footer) if int(code)]
    before, after = meta.get("observationsAtStart"), meta.get("observationsAtEnd")
    audio = None
    if before and after:
        ab, ae = before.get("audio", {}), after.get("audio", {})
        audio = {key: ae[key]-ab[key] for key in
                 ("underruns", "nativePullFailures", "writeFailures", "discardedReadSamples", "trackGenerations")
                 if key in ab and key in ae}
        audio.update({key: after[key]-before[key] for key in
                      ("pcmCapturedFrames", "pcmPulledFrames", "sharedPcmDroppedFrames", "pcmDroppedFrames")
                      if key in before and key in after})
    return dict(metadata=meta, complete=complete, discardedRows=discarded,
                recorderErrors=errors, audio=audio,
                engine=stats, presentation=host_stats, gl=ranking, worstFrames=worst,
                worstPresentationFrames=host_worst)


def plot(data, target):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(2, 2, figsize=(13, 7))
    for label,key,color in (("Engine","engine","tab:blue"),("Android post","presentation","tab:orange")):
        s=data[key]
        if not s: continue
        axes[0,0].plot(s["ts"][1:],s["gaps"],lw=.6,label=label,color=color)
        axes[0,1].hist([min(g,999.999) for g in s["gaps"]],bins=[0,8,16.67,20,25,33.34,40,50,67,100,150,250,500,1000],
                       histtype="step",label=label,color=color)
    axes[0,0].axhline(50,color="tab:red",ls="--",lw=.7)
    axes[0,0].set(xlabel="Seconds",ylabel="Frame interval (ms)")
    axes[0,0].legend();axes[0,1].legend();axes[0,1].set(xlabel="Frame interval (ms; last bin includes all >=500)",ylabel="Count",yscale="log")
    rows=data["gl"][:10]
    axes[1,0].barh([r["name"] for r in rows][::-1],[r["totalMs"] for r in rows][::-1])
    axes[1,0].set(xlabel="Total GL service wall time (ms)")
    rows=data["worstFrames"][:12]
    axes[1,1].bar(range(len(rows)),[r["frameMs"] for r in rows],color="0.7",label="Frame interval")
    axes[1,1].plot(range(len(rows)),[r["completedGlMs"] for r in rows],"o",label="Completed GL")
    axes[1,1].set(xlabel="Longest engine frames, ranked",ylabel="ms");axes[1,1].legend()
    fig.suptitle(f"{data['metadata'].get('appName','iOS app')} · {data['metadata'].get('model','')}")
    fig.tight_layout();fig.savefig(target,dpi=140);plt.close(fig)


def main():
    parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture",nargs="?",type=Path)
    parser.add_argument("--pull",type=Path,metavar="DIRECTORY")
    parser.add_argument("--top",type=int,default=15)
    parser.add_argument("--json",type=Path)
    parser.add_argument("--chart",type=Path)
    args=parser.parse_args()
    if args.pull:
        args.pull.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryFile() as archive:
            subprocess.run(["adb","exec-out","run-as","org.lighttouch.android","tar","-cf","-","-C","files","profiles"],
                           stdout=archive,check=True)
            archive.seek(0)
            with tarfile.open(fileobj=archive) as bundle:
                bundle.extractall(args.pull,filter="data")
        print(f"Saved captures under {args.pull / 'profiles'}")
    if not args.capture:
        if args.pull: return
        parser.error("provide a capture directory or --pull")
    data=summarize(args.capture,max(1,args.top))
    meta=data["metadata"]
    print(f"{meta.get('appName','Unknown app')} ({meta.get('bundleId','unknown')}) · {meta.get('model','unknown device')}")
    if not data["complete"]: print("Partial capture: missing stop footer; buffered final events may be absent.")
    if meta.get("error") or data["recorderErrors"]:
        print(f"Capture error: {meta.get('error') or data['recorderErrors']}")
    if data["discardedRows"]: print(f"Ignored {data['discardedRows']} incomplete/unrecognized rows.")
    for label,key in (("Engine publication","engine"),("Android post completion","presentation")):
        if data[key]: report(label,data[key])
    if data["audio"] is not None:
        print("\nAudio counter changes during capture:")
        for key,value in data["audio"].items(): print(f"  {key}: {value}")
    print("\nGL service wall time: count, total ms, max ms")
    for row in data["gl"][:args.top]:
        print(f"  {row['name']:<28} {row['count']:9d} {row['totalMs']:10.2f} {row['maxMs']:9.3f}")
    print("\nLongest engine frames (overlapping timing counters; GL booked on completion):")
    for row in data["worstFrames"]:
        print(f"  {row['atSeconds']:8.3f}s {row['frameMs']:8.2f}ms | GL {row['completedGlMs']:.2f} "
              f"BQL {row['bqlOutsideMs']+row['bqlInsideMs']:.2f} vCPU run {row['vcpuRunMs']:.2f} CPU {row['vcpuCpuMs']:.2f} "
              f"outside {row['vcpuOutsideMs']:.2f} main CPU {row['mainCpuMs']:.2f}")
        for call in row["calls"][:3]: print(f"      {call['name']}: {call['totalMs']:.2f}ms / {call['count']} calls; max {call['maxMs']:.2f}ms")
        for call in sorted(row["slowCalls"],key=lambda item:item['overlapMs'],reverse=True)[:3]:
            print(f"      Slow {call['name']}: {call['totalMs']:.2f}ms total; {call['overlapMs']:.2f}ms overlap")
        for lock in sorted(row["locks"],key=lambda item:item['overlapMs'],reverse=True)[:3]:
            print(f"      BQL {lock['kind']} {lock['site']}: {lock['overlapMs']:.2f}ms overlap")
    print("\nLongest Android post gaps:")
    for row in data["worstPresentationFrames"]:
        print(f"  {row['atSeconds']:8.3f}s {row['frameMs']:8.2f}ms | buffer {row['bufferWaitMs']:.2f} "
              f"scale {row['scaleMs']:.2f} pacing {row['pacingMs']:.2f} post {row['postMs']:.2f} "
              f"guard {row['guardWaitMs']:.2f} copy {row['copyMs']:.2f}")
    print("\nGL wall time includes driver waits and host descheduling; it is not GPU execution time.")
    if args.json: args.json.write_text(json.dumps(data,indent=2)+"\n")
    if args.chart: plot(data,args.chart)


if __name__ == "__main__": main()
