#!/usr/bin/env python3
"""simpleperf samples -> `perf script -F tid,time,ip,sym` text, for gtracesamples.py (TEMP diagnostic).

usage: sp2script.py PERF.DATA PERF-PID.MAP ENGINE.unstripped.so > perf.txt
Engine offsets are named with llvm-symbolizer, JIT code with the engine's perf map (guest-0xADDR rows),
other libraries by file name.
"""
import bisect, glob, os, re, subprocess, sys

data, pmap, engine = sys.argv[1:4]
ndk = sorted(glob.glob(os.path.expanduser("~/Android/Sdk/ndk/*")))[-1]
txt = subprocess.run([sys.executable, f"{ndk}/simpleperf/report_sample.py", "-i", data],
                     capture_output=True, text=True).stdout
HDR = re.compile(r"^\S.*?\t(\d+)/(\d+) \[\d+\] (\d+\.\d+):")
FR = re.compile(r"^\s+([0-9a-f]+) (\S+?)\[\+([0-9a-f]+)\] \((.*)\)")
rows = []
cur = None
for l in txt.splitlines():
    m = HDR.match(l)
    if m:
        cur = (int(m[2]), m[3]); continue
    m = FR.match(l)
    if m and cur:
        rows.append((cur[0], cur[1], int(m[1], 16), m[2], int(m[3], 16), m[4])); cur = None
starts, ends, names = [], [], []
for l in open(pmap):
    p = l.split(None, 2)
    if len(p) == 3:
        s = int(p[0], 16); starts.append(s); ends.append(s + int(p[1], 16)); names.append(p[2].strip())
order = sorted(range(len(starts)), key=lambda i: starts[i])
starts = [starts[i] for i in order]; ends = [ends[i] for i in order]; names = [names[i] for i in order]
offs = sorted({r[4] for r in rows if r[3].startswith("libqemu-arm")})
sym = {}
if offs:
    llvm = sorted(glob.glob(f"{ndk}/toolchains/llvm/prebuilt/*/bin/llvm-symbolizer"))[0]
    out = subprocess.run([llvm, "--obj", engine, "--functions=short", "--no-inlines"], input="\n".join(hex(o) for o in offs),
                         capture_output=True, text=True).stdout.split("\n\n")
    for o, block in zip(offs, out):
        sym[o] = block.strip().split("\n")[0] if block.strip() else "?"
for tid, t, ip, dso, off, path in rows:
    if dso.startswith("libqemu-arm"):
        s = sym.get(off, "?")
    elif dso == "unknown":
        i = bisect.bisect_right(starts, ip) - 1
        s = names[i] if i >= 0 and ip < ends[i] else "[jit?]"
    else:
        s = "[" + os.path.basename(path) + "]"
    print(f"{tid} {t}: {ip:x} {s}")
