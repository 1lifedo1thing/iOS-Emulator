#!/usr/bin/env python3
"""Boot benchmark for the qemu-ios n72 guest, run locally or on an Android
device via `adb shell run-as`.

QMP runs over stdio. Every --poll seconds a screendump is taken and pulled;
each distinct frame is saved with its timestamp so boot milestones can be
read off afterwards (first Apple logo, lock screen, ...).

Usage:
  ltbench.py local --qemu Q --base B --rom R --out DIR [--seconds N] [-- extra qemu args]
  ltbench.py adb   --qemu Q --base B --rom R --out DIR [--seconds N] [-- extra qemu args]
For adb, Q/B/R are paths on the device inside the app's data dir.
"""
import argparse, hashlib, json, os, subprocess, sys, threading, time

PKG = "org.lighttouch.android"

p = argparse.ArgumentParser()
p.add_argument("mode", choices=["local", "adb"])
p.add_argument("--qemu", required=True)
p.add_argument("--base", required=True)
p.add_argument("--rom", required=True)
p.add_argument("--state", help="writable state dir (created fresh by default)")
p.add_argument("--keep-state", action="store_true", help="reuse --state as is (warm guest data)")
p.add_argument("--out", required=True)
p.add_argument("--seconds", type=float, default=240)
p.add_argument("--poll", type=float, default=1.0)
p.add_argument("--wrap", default="", help="command prefix on the target, e.g. 'simpleperf record ...'")
p.add_argument("--stop-on", help="stop when a frame's sha256 prefix matches one of these (comma list)")
p.add_argument("--stop-lock", action="store_true", help="stop at the first lock-screen frame")
p.add_argument("--workload", help="after the lock screen, run this workload: swipes")
p.add_argument("--workload-seconds", type=float, default=60)
p.add_argument("--fling", action="store_true", help="scroll workload: flings, guest-driven momentum")
p.add_argument("--vcpu-mask", help="after start, restrict only the 'CPU 0/TCG' thread to this hex mask")
p.add_argument("extra", nargs="*")
a = p.parse_args()
os.makedirs(a.out, exist_ok=True)

def sh_target(cmd):
    """Run a shell command on the target, return stdout bytes."""
    if a.mode == "local":
        return subprocess.run(["sh", "-c", cmd], capture_output=True).stdout
    return subprocess.run(["adb", "exec-out", f"run-as {PKG} sh -c '{cmd}'"], capture_output=True).stdout

state = a.state or (a.out + "/state" if a.mode == "local" else "files/bench-state")
if not a.keep_state:
    sh_target(f"rm -rf {state}; mkdir -p {state}/nand; cp {a.base}/nor.bin {state}/nor.bin; chmod 600 {state}/nor.bin")
dump = f"{state}/dump.ppm"

def opt(k, v):
    return f",{k}={v.replace(',', ',,')}"
machine = "iPod-Touch" + opt("boot-args", "amfi_allow_any_signature=1 cs_enforcement_disable=1")
machine += opt("direct-iboot", f"{a.base}/iBoot.bin") + opt("direct-llb", "") + opt("bootrom", a.rom)
machine += opt("nor", f"{a.base}/nor.bin") + opt("nandrw", f"{state}/nand") + opt("gid-blobs", f"{a.base}/gid-blobs.bin")
machine += opt("wifi", "off") + opt("nand", f"{a.base}/nand") + opt("nor-rw", f"{state}/nor.bin")
qargs = [a.qemu, "-M", machine, "-m", "128M", "-display", "none", "-no-shutdown", "-audio", "driver=none",
         "-serial", f"file:{state}/serial.log", "-qmp", "stdio"] + a.extra
if a.mode == "local":
    cmd = (a.wrap.split() if a.wrap else []) + qargs
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(a.out + "/stderr.log", "wb"))
else:
    quoted = " ".join("'" + x.replace("'", "'\\''") + "'" for x in qargs)
    remote = f"cd /data/data/{PKG} && exec {a.wrap} {quoted} 2>files/bench-stderr.log"
    cmd = ["adb", "shell", "-T", f"run-as {PKG} sh -c \"{remote}\""]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
t0 = time.monotonic()
json.dump({"argv": cmd, "t0_wall": time.time()}, open(a.out + "/run.json", "w"), indent=1)

replies = []
lock = threading.Condition()
def reader():
    for line in proc.stdout:
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        with lock:
            replies.append(msg)
            lock.notify_all()
threading.Thread(target=reader, daemon=True).start()

def qmp(command, **args):
    with lock:
        n = len(replies)
    proc.stdin.write((json.dumps({"execute": command, "arguments": args}) + "\n").encode())
    proc.stdin.flush()
    deadline = time.monotonic() + 30
    with lock:
        while True:
            for m in replies[n:]:
                if "return" in m or "error" in m:
                    replies.remove(m)
                    return m
            if not lock.wait(timeout=max(0, deadline - time.monotonic())):
                raise TimeoutError(command)

# Greeting, then capabilities.
with lock:
    lock.wait_for(lambda: any("QMP" in m for m in replies), timeout=60)
qmp("qmp_capabilities")
if a.vcpu_mask:
    for _ in range(50):
        out = sh_target("for p in $(pidof qemu-system-arm qemu-head qemu-tls qemu-pke qemu-lto 2>/dev/null); do for t in /proc/$p/task/*; do case $(cat $t/comm) in *TCG*) echo ${t##*/};; esac; done; done").decode().split()
        if out:
            print("VCPU", out[0], sh_target(f"taskset -p {a.vcpu_mask} {out[0]}").decode().strip(), flush=True)
            break
        time.sleep(0.2)
def is_lock(ppm):
    """The 3.1.3 lock screen has the blue Earth wallpaper at the panel centre."""
    try:
        parts = ppm.split(b"\n", 3)
        w, h = map(int, parts[1].split())
        px = parts[3]
        tot = [0, 0, 0]; n = 0
        for y in range(130, 330, 6):
            for x in range(40, 280, 6):
                i = (y * w + x) * 3
                for c in range(3): tot[c] += px[i + c]
                n += 1
        r, g, b = (v / n for v in tot)
        return b > 60 and b > r + 25
    except Exception:
        return False
stops = set(a.stop_on.split(",")) if a.stop_on else set()
frames, last = [], None
stderr_path = a.out + "/stderr.log" if a.mode == "local" else "files/bench-stderr.log"
lock_seen = False

def mtt(kind, x=None, y=None, slot=0, tid=1):
    """x, y in panel fractions (0..1, origin top-left, portrait)."""
    ev = []
    if x is not None:
        ev.append({"type": "mtt", "data": {"type": "data", "slot": slot, "tracking-id": tid, "axis": "x", "value": int(x * 32767)}})
        ev.append({"type": "mtt", "data": {"type": "data", "slot": slot, "tracking-id": tid, "axis": "y", "value": int(y * 32767)}})
    ev.append({"type": "mtt", "data": {"type": kind, "slot": slot, "tracking-id": tid, "axis": "x", "value": 0}})
    r = qmp("input-send-event", events=ev)
    if "error" in r: print("MTT ERR", r, flush=True)
    return r

def drag(x0, y0, x1, y1, secs=0.4, steps=12):
    mtt("begin", x0, y0)
    for i in range(1, steps + 1):
        time.sleep(secs / steps)
        f = i / steps
        mtt("update", x0 + (x1 - x0) * f, y0 + (y1 - y0) * f)
    mtt("end", x1, y1)

def tap(x, y):
    mtt("begin", x, y); time.sleep(0.08); mtt("end", x, y)

def key(code):
    """Hardware buttons sit behind Command: Cmd+Shift+H is Home, Cmd+L power."""
    k = lambda c, d: {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": c}}}
    mods = ["meta_l", "shift"] if code == "h" else ["meta_l"]
    qmp("input-send-event", events=[k(m, True) for m in mods] + [k(code, True)])
    time.sleep(0.12)
    qmp("input-send-event", events=[k(code, False)] + [k(m, False) for m in mods])
    time.sleep(0.05)

def snap(label):
    t = time.monotonic() - t0
    if "error" in qmp("screendump", filename=dump):
        return
    data = sh_target(f"cat {dump}")
    open(f"{a.out}/w{t:07.2f}-{label}.ppm", "wb").write(data)

def proc_cpu():
    """Per-thread CPU seconds of the QEMU process on the target, keyed by thread name."""
    pid = "$(pidof qemu-system-arm)"
    txt = sh_target(f"for t in /proc/{pid}/task/*; do echo $(cat $t/comm)@$(cut -d\\) -f2 $t/stat | cut -d\\  -f13,14); done").decode()
    out = {}
    for line in txt.splitlines():
        if "@" not in line: continue
        name, ticks = line.rsplit("@", 1)
        u, s_ = (int(x) for x in ticks.split())
        out[name] = out.get(name, 0) + (u + s_) / 100.0
    return out

def proc_ctx():
    pid = "$(pidof qemu-system-arm)"
    txt = sh_target(f"for t in /proc/{pid}/task/*; do echo $(cat $t/comm)@$(grep -h ^voluntary_ctxt $t/status | cut -f2); done").decode()
    out = {}
    for line in txt.splitlines():
        if "@" not in line: continue
        name, n = line.rsplit("@", 1)
        out[name] = out.get(name, 0) + int(n.strip() or 0)
    return out

def grab():
    if "error" in qmp("screendump", filename=dump):
        return None
    return sh_target(f"cat {dump}")

def settle(label, quiet=1.2, limit=30.0):
    """Seconds from now until the screen last changed, once it has been
    unchanged for `quiet` seconds (or `limit` passes)."""
    start = time.monotonic(); prev = grab(); last_change = None
    while True:
        cur = grab(); now = time.monotonic()
        if cur != prev:
            last_change = now; prev = cur
        if last_change is None:
            if now - start > 15.0:   # never reacted
                last_change = start; break
            continue
        if now - last_change >= quiet or now - start > limit:
            break
    open(f"{a.out}/s-{label}.ppm", "wb").write(prev or b"")
    return round(last_change - start, 2)

def is_black(ppm):
    return ppm is not None and max(ppm[-460800::997]) < 16

def unlock(tries=6):
    """Wake, wait for the lock screen to be drawn, slide, confirm it left."""
    for attempt in range(tries):
        key("h")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and not is_lock(grab() or b""):
            time.sleep(0.2)
        drag(0.12, 0.93, 0.95, 0.93, secs=0.6)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            f = grab()
            if f and not is_lock(f) and not is_black(f):
                print("UNLOCKED attempt", attempt, flush=True)
                return True
            time.sleep(0.3)
    return False

def run_workload():
    """Interactive load: unlock, page swipes, app open/close."""
    print("WORKLOAD start", round(time.monotonic() - t0, 2), flush=True)
    time.sleep(1)
    if a.workload == "apps":
        if not unlock():
            print("UNLOCK FAILED", flush=True); return
        settle("unlock", limit=10)
        apps = [("Settings", 0.197, 0.678), ("Notes", 0.40, 0.495), ("Calendar", 0.40, 0.128),
                ("Calculator", 0.80, 0.495), ("Photos", 0.60, 0.128)]
        results = []
        for rnd in ("cold", "warm"):
            for name, x, y in apps:
                tap(x, y); lt = settle(f"{rnd}-{name}")
                key("h"); ht = settle(f"{rnd}-{name}-home")
                results.append((rnd, name, lt, ht))
                print("APP", rnd, name, "open", lt, "home", ht, flush=True)
        for rnd in ("cold", "warm"):
            o = [r[2] for r in results if r[0] == rnd]; h = [r[3] for r in results if r[0] == rnd]
            print("SUMMARY", rnd, "open_total", round(sum(o), 2), "home_total", round(sum(h), 2), flush=True)
        json.dump(results, open(a.out + "/apps.json", "w"))
        return
    if a.workload == "scroll":
        if not unlock():
            print("UNLOCK FAILED", flush=True); return
        settle("unlock", limit=10)
        tap(0.197, 0.678); settle("settings", limit=15)
        flips = lambda: int((sh_target(f"grep -c FT..flip {stderr_path}") or b"0").strip() or 0)
        n0 = flips(); t1 = time.monotonic()
        up = True
        while time.monotonic() - t1 < a.workload_seconds:
            if a.fling:
                y0, y1 = (0.8, 0.3) if up else (0.3, 0.8)
                drag(0.5, y0, 0.5, y1, secs=0.08, steps=4); up = not up
                time.sleep(1.2)
            else:
                drag(0.5, 0.85, 0.5, 0.25, secs=0.8, steps=24)
                drag(0.5, 0.25, 0.5, 0.85, secs=0.8, steps=24)
        dt = time.monotonic() - t1; n1 = flips()
        print("SCROLL fps", round((n1 - n0) / dt, 1), "flips", n1 - n0, "secs", round(dt, 1), flush=True)
        return
    if a.workload == "idle":
        key("h"); time.sleep(1); drag(0.12, 0.93, 0.95, 0.93, secs=0.6); time.sleep(5); snap("home")
        cpu0 = proc_cpu(); cs0 = proc_ctx(); t1 = time.monotonic()
        time.sleep(a.workload_seconds)
        cpu1 = proc_cpu(); cs1 = proc_ctx(); dt = time.monotonic() - t1
        print("CTXSW/s", {k: round((cs1[k] - cs0.get(k, 0)) / dt) for k in cs1 if cs1[k] - cs0.get(k, 0) > dt}, flush=True)
        print("IDLE", {k: round((cpu1[k] - cpu0.get(k, 0)) / dt * 100, 1) for k in cpu1}, flush=True)
        snap("idle-end")
        return
    if a.workload == "probe":
        for flip in (False, True):
            y = 0.93 if not flip else 0.07
            key("h"); time.sleep(1); snap(f"wake{int(flip)}")
            mtt("begin", 0.12, y); time.sleep(0.3); mtt("update", 0.4, y); time.sleep(0.2); snap(f"mid{int(flip)}")
            mtt("update", 0.95, y); time.sleep(0.1); mtt("end", 0.95, y); time.sleep(2); snap(f"after{int(flip)}")
        return
    key("h"); time.sleep(1)
    drag(0.12, 0.93, 0.95, 0.93, secs=0.6)
    time.sleep(4); snap("home")
    end = time.monotonic() + a.workload_seconds
    n = 0
    while time.monotonic() < end:
        drag(0.85, 0.5, 0.15, 0.5, secs=0.3); time.sleep(1.5); snap(f"p{n}a")
        drag(0.15, 0.5, 0.85, 0.5, secs=0.3); time.sleep(1.5); snap(f"p{n}b")
        tap(0.14 + 0.24 * (n % 4), 0.12); time.sleep(4); snap(f"app{n}")
        key("h"); time.sleep(2.0)
        n += 1
    print("WORKLOAD end", round(time.monotonic() - t0, 2), "cycles", n, flush=True)

log = open(a.out + "/frames.jsonl", "w")
try:
    while time.monotonic() - t0 < a.seconds and proc.poll() is None:
        time.sleep(max(0, a.poll - (time.monotonic() - t0) % a.poll))
        t = time.monotonic() - t0
        r = qmp("screendump", filename=dump)
        if "error" in r:
            continue
        data = sh_target(f"cat {dump}")
        if not data:
            continue
        h = hashlib.sha256(data).hexdigest()[:16]
        if h != last:
            open(f"{a.out}/f{t:07.2f}-{h}.ppm", "wb").write(data)
            rec = {"t": round(t, 2), "sha": h}
            log.write(json.dumps(rec) + "\n"); log.flush()
            print(rec, flush=True)
            last = h
            if h in stops or (a.stop_lock and is_lock(data)):
                print("LOCKSCREEN", round(t, 2), flush=True)
                lock_seen = True
                json.dump({"lock_t": round(t, 2)}, open(a.out + "/result.json", "w"))
                break
    if a.workload and (a.stop_lock and lock_seen):
        run_workload()
finally:
    try:
        qmp("quit")
    except Exception:
        pass
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
    print("elapsed", round(time.monotonic() - t0, 2))
