#!/bin/bash
# Stop the in-app guest cleanly, then install an APK. usage: swapapk.sh APK (ANDROID_SERIAL set)
set -u
D=$(dirname "$0")
PERF_DIR="$D" python3 - <<'PY'
import time, sys, os
__file__ = sys.argv[0] if len(sys.argv) > 0 and sys.argv[0] else "x"
exec(open(os.path.join(os.environ.get("PERF_DIR", "."), "inappfps.py")).read().split("ap = argparse")[0])
sh("input", "keyevent", "KEYCODE_WAKEUP"); sh("am", "start", "-n", f"{PKG}/.MainActivity"); time.sleep(2)
# "?" means the status was not on screen yet (the app still opening): installing then would kill
# a running guest and leave its record needing recovery, so wait for a real state.
t = time.monotonic()
while (st := state()) == "?" and time.monotonic() - t < 20: time.sleep(1)
if st == "?": sys.exit("guest state unreadable; not installing")
if st not in ("stopped", "?"):
    if st == "paused": tap_id("resume_overlay") or tap_text("Resume"); time.sleep(2)
    stop(); t = time.monotonic()
    while state() != "stopped" and time.monotonic() - t < 90: time.sleep(2)
st = state(); print("state", st, flush=True)
if st != "stopped": sys.exit("guest did not stop; not installing")
PY
[ $? -eq 0 ] || exit 1
adb install -r "$1" | tail -1
