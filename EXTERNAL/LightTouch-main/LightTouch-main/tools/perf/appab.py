#!/usr/bin/env python3
"""In-app boot A/B: for each APK, install, start the selected device, time to lock
screen, then Stop and wait for 'stopped'. usage: appab.py label=apk ..."""
import re, subprocess, sys, time, struct
PKG = "org.lighttouch.android"
def sh(*a): return subprocess.run(["adb", "shell", *a], capture_output=True, text=True).stdout
def ui():
    sh("uiautomator", "dump", "/sdcard/ui.xml")
    return subprocess.run(["adb", "exec-out", "cat", "/sdcard/ui.xml"], capture_output=True, text=True).stdout
def find(xml, text):
    for m in re.finditer(r'<node [^>]*?text="([^"]*)"[^>]*?enabled="(true|false)"[^>]*?bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', xml):
        if m.group(1) == text:
            x0, y0, x1, y1 = map(int, m.groups()[2:])
            return m.group(2) == "true", ((x0 + x1)//2, (y0 + y1)//2)
    return None, None
def wait_tap(text, limit=120):
    t = time.monotonic()
    while time.monotonic() - t < limit:
        en, pos = find(ui(), text)
        if en:
            sh("input", "tap", str(pos[0]), str(pos[1])); return True
        time.sleep(2)
    return False
def state():
    xml = ui()
    for s in ("stopped", "running", "starting", "paused", "stopping"):
        if f'text="{s}"' in xml: return s
    return "?"
def guest_box():
    xml = ui()
    # the guest SurfaceView is the largest node without text below the buttons
    best = None
    for m in re.finditer(r'class="android.view.SurfaceView"[^>]*?bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', xml):
        best = tuple(map(int, m.groups()))
    return best
def shot():
    raw = subprocess.run(["adb", "exec-out", "screencap"], capture_output=True).stdout
    w, h = struct.unpack("<II", raw[:8]); off = len(raw) - w*h*4
    return w, h, raw[off:]
def is_lock(box):
    w, h, px = shot(); x0, y0, x1, y1 = box
    # Earth sits in the guest's upper-middle; sample that band.
    ys = range(y0 + (y1-y0)*27//100, y0 + (y1-y0)*69//100, 10); xs = range(x0 + (x1-x0)//8, x1 - (x1-x0)//8, 10)
    r = g = b = n = 0
    for y in ys:
        for x in xs:
            i = (y*w + x)*4; r += px[i]; g += px[i+1]; b += px[i+2]; n += 1
    return b/n > 60 and b/n > r/n + 25
for spec in sys.argv[1:]:
    label, apk = spec.split("=", 1)
    if state() not in ("stopped", "?"):
        wait_tap("Stop"); t = time.monotonic()
        while state() != "stopped" and time.monotonic() - t < 120: time.sleep(2)
    print(label, subprocess.run(["adb", "install", "-r", apk], capture_output=True, text=True).stdout.strip().splitlines()[-1], flush=True)
    sh("input", "keyevent", "KEYCODE_WAKEUP"); sh("am", "start", "-n", f"{PKG}/.MainActivity"); time.sleep(3)
    xml = ui()
    target = "Recover retained state" if find(xml, "Recover retained state")[0] else "Start"
    if not wait_tap(target): print(label, "no", target); continue
    t0 = time.monotonic(); box = None
    while time.monotonic() - t0 < 400:
        box = box or guest_box()
        if box and is_lock(box): break
        time.sleep(1)
    print(label, "APP LOCKSCREEN", round(time.monotonic() - t0, 1), "via", target, flush=True)
    time.sleep(5)
    wait_tap("Stop"); t = time.monotonic()
    while state() != "stopped" and time.monotonic() - t < 120: time.sleep(2)
    print(label, "stopped after", round(time.monotonic() - t, 1), flush=True)
