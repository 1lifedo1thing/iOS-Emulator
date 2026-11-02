#!/usr/bin/env python3
"""UI frame rate and panel refresh while scrolling the app's own UI (not the guest).

usage: uirate.py [--swipes N] [--x X] [--top Y] [--bottom Y]
  Run with the screen to measure in front (Settings, the Store, the setup wizard...). Flings it
  up and down, then reports:
  panel     the display mode SurfaceFlinger was in, sampled through the flings (60 or 120 Hz)
  ui fps    the app's own frame rate during the flings, from gfxinfo framestats (each frame's
            intended vsync): what scrolling actually looked like, whatever the panel ran at
  votes     the app's layers that hold a frame rate request (the guest SurfaceView's 60 Hz)
A screen that scrolls at 120 shows ui fps ~120 on a 120 Hz panel; ~60 means the app was held to
60 Hz, ~30 that it missed every other refresh."""
import argparse, re, statistics, subprocess, time
PKG = "org.lighttouch.android"
def sh(*a): return subprocess.run(["adb", "shell", *a], capture_output=True, text=True).stdout

def panel_rate():
    m = re.search(r"activeMode=\{[^}]*?vsyncRate=([\d.]+)", sh("dumpsys", "SurfaceFlinger", "--displays"))
    return float(m.group(1)) if m else None

def framestats():
    """Intended vsync (ns) of each frame the app drew since the last reset."""
    text = sh("dumpsys", "gfxinfo", PKG, "framestats")
    vsyncs, cols = [], None
    for line in text.splitlines():
        if line.startswith("Flags,"): cols = line.rstrip(",").split(","); continue
        if cols and re.match(r"^\d", line):
            row = line.rstrip(",").split(",")
            if len(row) >= len(cols) and row[0] == "0": vsyncs.append(int(row[cols.index("IntendedVsync")]))
        elif cols and line.startswith("---PROFILEDATA---") and vsyncs: cols = None
    return vsyncs

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--swipes", type=int, default=4)
    p.add_argument("--x", type=int, default=540)
    p.add_argument("--top", type=int, default=700)
    p.add_argument("--bottom", type=int, default=1900)
    a = p.parse_args()
    sh("dumpsys", "gfxinfo", PKG, "reset")
    panel = []
    # gfxinfo keeps only the last couple of seconds of frames, so read it after every fling.
    v = set()
    for i in range(2 * a.swipes):
        y0, y1 = (a.bottom, a.top) if i % 2 == 0 else (a.top, a.bottom)
        # One panel reading per fling, taken mid-fling: a SurfaceFlinger dump is heavy enough to
        # cause the jank being measured if it ran throughout.
        swipe = subprocess.Popen(["adb", "shell", "input", "swipe", str(a.x), str(y0), str(a.x), str(y1), "120"])
        time.sleep(0.35); panel.append(panel_rate()); swipe.wait(); time.sleep(0.8)
        v |= set(framestats())
    v = sorted(v)
    # Frames drawn back to back (under 50 ms apart) are the scrolling; gaps are idle time.
    gaps = [(b - c) / 1e6 for c, b in zip(v, v[1:]) if (b - c) < 50e6]
    print(f"panel     {', '.join(f'{r:.0f} Hz x{panel.count(r)}' for r in sorted(set(r for r in panel if r))) or 'unknown'}")
    if gaps:
        med = statistics.median(gaps)
        over = sum(g > 1.5 * med for g in gaps)
        print(f"ui fps    {1000 / med:.0f} (median frame interval {med:.1f} ms over {len(gaps) + 1} frames; "
              f"{over} intervals over 1.5x the median; mean {1000 / statistics.mean(gaps):.0f} fps)")
        # How many refreshes each frame took: 1 is smooth, 2+ is a dropped frame the eye sees.
        hist = {}
        for g in gaps: k = min(4, max(1, round(g / med))); hist[k] = hist.get(k, 0) + 1
        print("refreshes " + "  ".join(f"{k}{'+' if k == 4 else ''}x: {hist.get(k, 0)}" for k in range(1, 5)))
    else:
        print("ui fps    no frames drawn (is the app in front and the screen scrollable?)")
    tree = sh("dumpsys", "SurfaceFlinger")
    for line in tree.splitlines():
        if PKG in line and "requestedFrameRate" in line:
            name = re.sub(r"^[\s│├└─]*", "", line).split(" requestedFrameRate")[0][-60:]
            print(f"vote      {name}: {line.split('requestedFrameRate: ')[1].split(' pid=')[0]}")

if __name__ == "__main__":
    main()
