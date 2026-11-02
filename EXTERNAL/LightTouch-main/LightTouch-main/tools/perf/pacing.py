#!/usr/bin/env python3
"""Frame pacing from an ipod_touch_lcd_frame_log trace: flips/s and vsync lateness
over the last N seconds of guest time (the scroll window)."""
import sys
path, secs = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 20
ev = []
for line in open(path, errors="ignore"):
    if "[FT] " not in line: continue
    p = line.split("[FT] ", 1)[1].split()
    if len(p) >= 4: ev.append((p[0], int(p[1]), int(p[3], 16)))
end = ev[-1][1]; start = end - secs * 1e9
win = [e for e in ev if e[1] >= start]
vs = [e[2] if e[2] < 2**31 else 0 for e in win if e[0] == "vsync"]
flips = sum(1 for e in win if e[0] == "flip")
late1 = sum(1 for v in vs if v > 1_000_000) / max(len(vs), 1) * 100
late4 = sum(1 for v in vs if v > 4_000_000) / max(len(vs), 1) * 100
vs.sort()
p50 = vs[len(vs)//2] / 1e6 if vs else 0; p95 = vs[int(len(vs)*.95)] / 1e6 if vs else 0
print(f"flips/s={flips/secs:.1f} vsyncs/s={len(vs)/secs:.1f} late>1ms={late1:.1f}% late>4ms={late4:.1f}% p50={p50:.2f}ms p95={p95:.2f}ms")
