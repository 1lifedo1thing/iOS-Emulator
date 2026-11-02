#!/usr/bin/env python3
"""Guest timeline from an engine LT_GTRACE file (TEMP diagnostic; engine util/lt-gtrace.c).

usage: gtrace.py TRACE [--frames] [--json out.json [--from S --to S]] [--present-ctx HEX]

--frames  per-frame breakdown, frames cut at the game's presents (GL slot 4097 on the busiest
          non-compositor context, or --present-ctx): wall, idle, per-process user/kernel time,
          system calls, Mach messages, IRQ lines, GL time; then the slowest and median frames.
--json    a Chrome/Perfetto trace (open in ui.perfetto.dev): one track per guest thread under its
          process, kernel episodes named by their system call, GL calls, idle, and device tracks
          (VBL, display latch, scaler, host publish, IRQ lines). --from/--to (s from the start) cut it.
"""
import argparse, collections, json, os, struct, sys

LTG = dict(EXC=1, ERET=2, THREAD=3, ASID=4, HALT=5, GL_BEGIN=6, GL_END=7, MACH_MSG=8, VBL=9,
           LATCH=10, SCALER=11, PUBLISH=12, IRQ_LINE=13, MARK=14)
T = {v: k for k, v in LTG.items()}
USR = 0x10
EXC_NAME = {1: "undef", 2: "svc", 3: "prefetch-abort", 4: "data-abort", 5: "irq", 6: "fiq", 7: "bkpt"}
MACH = {-10: "vm_allocate", -12: "vm_deallocate", -14: "vm_protect", -15: "vm_map", -16: "port_allocate",
        -18: "port_deallocate", -19: "port_mod_refs", -26: "mach_reply_port", -27: "thread_self",
        -28: "task_self", -29: "host_self", -31: "mach_msg", -32: "mach_msg_overwrite",
        -33: "semaphore_signal", -34: "semaphore_signal_all", -35: "semaphore_signal_thread",
        -36: "semaphore_wait", -37: "semaphore_wait_signal", -38: "semaphore_timedwait",
        -39: "semaphore_timedwait_signal", -59: "swtch_pri", -60: "swtch", -61: "thread_switch",
        -89: "mach_timebase_info", -90: "mach_wait_until", -91: "mk_timer_create",
        -92: "mk_timer_destroy", -93: "mk_timer_arm", -94: "mk_timer_cancel"}
BSD = {3: "read", 4: "write", 5: "open", 6: "close", 20: "getpid", 54: "ioctl", 73: "munmap", 74: "mprotect",
       75: "madvise", 92: "fcntl", 93: "select", 116: "gettimeofday", 153: "pread", 197: "mmap",
       202: "sysctl", 230: "poll", 301: "psynch_mutexwait", 302: "psynch_mutexdrop", 303: "psynch_cvbroad",
       304: "psynch_cvsignal", 305: "psynch_cvwait", 306: "psynch_rw_rdlock", 307: "psynch_rw_wrlock",
       308: "psynch_rw_unlock", 334: "semwait_signal", 338: "stat64", 339: "fstat64", 340: "lstat64",
       344: "getdirentries64", 360: "bsdthread_create", 361: "bsdthread_terminate", 362: "kqueue",
       363: "kevent", 366: "bsdthread_register", 367: "workq_open", 368: "workq_kernreturn",
       369: "kevent64", 372: "thread_selfid", 396: "read_nocancel", 397: "write_nocancel",
       423: "semwait_signal_nocancel"}
GL = {4097: "presentSurface", 4099: "bindSurface", 89: "glFinish", 65: "glDrawArrays", 67: "glDrawElements",
      672: "glBindFramebuffer"}


def svc_name(nr):
    nr = struct.unpack("<i", struct.pack("<I", nr & 0xffffffff))[0]
    if nr == -(1 << 31):
        return "platform"
    if nr < 0:
        return MACH.get(nr, f"mach{nr}")
    return BSD.get(nr, f"sys{nr}")


def load(path):
    with open(path, "rb") as f:
        assert f.read(9) == b"LTGTRACE1"
        n = struct.unpack("<Q", f.read(8))[0]
        data = f.read(n * 24)
    ev = [struct.unpack_from("<QBBHIII", data, i * 24) for i in range(len(data) // 24)]
    ev.sort(key=lambda e: e[0])
    names = {}
    np = path + ".names"
    if os.path.exists(np):
        for l in open(np):
            p = l.split()
            if p and p[0] == "thread" and len(p) >= 3:
                names[int(p[1], 16)] = p[-1]
    return ev, names


class Model:
    """Walks the events: which thread runs, in which mode, what kernel episode it is in."""

    def __init__(self, ev, names):
        self.ev, self.names = ev, names
        self.t0 = ev[0][0]
        self.thread_asid = collections.defaultdict(collections.Counter)
        # thread -> process name: the names file, else the ASID most seen while it ran
        cur = None
        for t, ty, mode, asid, a, b, c in ev:
            if ty == LTG["THREAD"]:
                cur = a
            elif cur is not None and ty == LTG["EXC"] and mode == USR:
                self.thread_asid[cur][asid] += 1
        self.asid_name = {}
        for th, cnt in self.thread_asid.items():
            nm = names.get(th)
            if nm and nm != "?":
                asid = cnt.most_common(1)[0][0]
                self.asid_name.setdefault(asid, nm)

    def proc(self, th):
        nm = self.names.get(th)
        if nm and nm != "?":
            return nm
        cnt = self.thread_asid.get(th)
        if cnt:
            asid = cnt.most_common(1)[0][0]
            return self.asid_name.get(asid, f"asid{asid}")
        return "kernel"

    def slices(self):
        """Yields (start, end, thread, kind, label): kind user|kernel|idle."""
        cur, mode, since, label, kind = None, None, None, None, None
        for t, ty, m, asid, a, b, c in self.ev:
            if ty == LTG["THREAD"]:
                if since is not None and cur is not None and t > since:
                    yield since, t, cur, kind, label
                cur, since, kind, label = a, t, "kernel", "resume"
            elif ty == LTG["EXC"]:
                if since is not None and t > since:
                    yield since, t, cur, kind, label
                since = t
                if kind == "idle" or m == USR or kind is None or kind == "user":
                    exc = EXC_NAME.get(a, f"exc{a}")
                    label = svc_name(b) if a == 2 else exc
                kind = "kernel"
            elif ty == LTG["ERET"]:
                if since is not None and t > since:
                    yield since, t, cur, kind, label
                since = t
                kind = "user" if m == USR else "kernel"
                if kind == "user":
                    label = "user"
            elif ty == LTG["HALT"]:
                if since is not None and t > since:
                    yield since, t, cur, kind, label
                since, kind, label = t, "idle", "idle"


def frames(model, present_ctx):
    ev = model.ev
    pres = collections.Counter(e[5] for e in ev if e[1] == LTG["GL_END"] and e[4] == 4097)
    if not pres:
        print("no presents in the trace"); return
    if present_ctx is None:
        # the compositor (backboardd, context 0x80000002 on the n81) presents too; pick the busiest other
        cands = [c for c, _ in pres.most_common() if c != 0x80000002] or list(pres)
        present_ctx = cands[0]
    print("presents by context:", {hex(c): n for c, n in pres.items()}, "-> frames at", hex(present_ctx))
    cuts = [e[0] for e in ev if e[1] == LTG["GL_END"] and e[4] == 4097 and e[5] == present_ctx]
    F = []
    for i in range(len(cuts) - 1):
        F.append(dict(t0=cuts[i], t1=cuts[i + 1], cpu=collections.Counter(), kern=collections.Counter(),
                      sys=collections.Counter(), msg=collections.Counter(), irq=collections.Counter(),
                      gl=collections.Counter(), idle=0, vbl=0, latch=0, scaler=0))
    import bisect
    starts = [f["t0"] for f in F]

    def frame_of(t):
        i = bisect.bisect_right(starts, t) - 1
        return F[i] if 0 <= i < len(F) and t < F[i]["t1"] else None

    for s, e, th, kind, label in model.slices():
        # split across frame boundaries
        while s < e:
            f = frame_of(s)
            if not f:
                i = bisect.bisect_right(starts, s)
                if i >= len(F): break
                s = F[i]["t0"]; continue
            end = min(e, f["t1"])
            d = end - s
            p = model.proc(th)
            if kind == "idle":
                f["idle"] += d
            elif kind == "user":
                f["cpu"][p] += d
            else:
                f["kern"][(p, label)] += d
            s = end
    glstack = {}
    for t, ty, m, asid, a, b, c in ev:
        f = frame_of(t)
        if ty == LTG["GL_BEGIN"]:
            glstack[b] = (t, a)
        elif ty == LTG["GL_END"] and b in glstack:
            t0, slot = glstack.pop(b)
            if f: f["gl"][GL.get(slot, f"gl{slot}")] += t - t0
        if not f: continue
        if ty == LTG["EXC"]:
            if a == 2: f["sys"][svc_name(b)] += 1
        elif ty == LTG["MACH_MSG"]:
            f["msg"][a] += 1
        elif ty == LTG["IRQ_LINE"] and b:
            f["irq"][(c & 0xfff, a)] += 1
        elif ty == LTG["VBL"]:
            f["vbl"] += 1
        elif ty == LTG["LATCH"]:
            f["latch"] += 1
        elif ty == LTG["SCALER"] and a == 1:
            f["scaler"] += 1
    walls = sorted(range(len(F)), key=lambda i: F[i]["t1"] - F[i]["t0"])
    hist = collections.Counter(round((f["t1"] - f["t0"]) / 1e6 / 16.67) for f in F)
    print(f"{len(F)} frames, mean {sum(f['t1'] - f['t0'] for f in F) / len(F) / 1e6:.1f} ms;",
          "in 60 Hz periods:", dict(sorted(hist.items())))

    def show(i, tag):
        f = F[i]; wall = (f["t1"] - f["t0"]) / 1e6
        user = sum(f["cpu"].values()) / 1e6; kern = sum(f["kern"].values()) / 1e6
        print(f"\n== {tag}: frame {i} at {(f['t0'] - model.t0) / 1e9:.3f} s: wall {wall:.1f} ms = user {user:.1f}"
              f" + kernel {kern:.1f} + idle {f['idle'] / 1e6:.1f};  vbl {f['vbl']} latch {f['latch']} scaler {f['scaler']}")
        procs = collections.Counter()
        for p, d in f["cpu"].items(): procs[p] += d
        for (p, l), d in f["kern"].items(): procs[p] += d
        print("  by process (user+kernel ms):", ", ".join(f"{p} {d / 1e6:.1f} (u {f['cpu'][p] / 1e6:.1f})"
                                                       for p, d in procs.most_common(8)))
        print("  kernel by entry (ms):", ", ".join(f"{p}:{l} {d / 1e6:.2f}" for (p, l), d in f["kern"].most_common(10)))
        print("  syscalls:", ", ".join(f"{k} {n}" for k, n in f["sys"].most_common(12)), f"(total {sum(f['sys'].values())})")
        print("  mach msg ids:", ", ".join(f"{k} {n}" for k, n in f["msg"].most_common(10)), f"(total {sum(f['msg'].values())})")
        print("  irq lines (vic,line):", ", ".join(f"{k} {n}" for k, n in f["irq"].most_common(10)))
        print("  gl (ms):", ", ".join(f"{k} {d / 1e6:.2f}" for k, d in f["gl"].most_common(6)))

    for i in walls[-3:][::-1]:
        show(i, "slowest")
    show(walls[len(walls) // 2], "median")
    show(walls[0], "fastest")
    # whole-window totals per process
    tot = collections.Counter(); totk = collections.Counter()
    for f in F:
        for p, d in f["cpu"].items(): tot[p] += d
        for (p, l), d in f["kern"].items(): totk[(p, l)] += d
    span = (F[-1]["t1"] - F[0]["t0"]) / 1e9
    print(f"\n== window {span:.1f} s: user ms/frame by process:",
          ", ".join(f"{p} {d / 1e6 / len(F):.2f}" for p, d in tot.most_common(8)))
    print("   kernel ms/frame by process:entry:", ", ".join(f"{p}:{l} {d / 1e6 / len(F):.2f}" for (p, l), d in totk.most_common(14)))
    print(f"   idle ms/frame {sum(f['idle'] for f in F) / 1e6 / len(F):.2f}")


def to_json(model, out, lo, hi):
    t0 = model.t0
    tr = []
    pids = {}

    def pid(p):
        if p not in pids:
            pids[p] = len(pids) + 10
            tr.append(dict(ph="M", name="process_name", pid=pids[p], args=dict(name=p)))
        return pids[p]

    def us(t): return (t - t0) / 1e3
    ok = lambda t: (lo is None or (t - t0) / 1e9 >= lo) and (hi is None or (t - t0) / 1e9 < hi)
    seen = set()
    for s, e, th, kind, label in model.slices():
        if not ok(s) or kind == "user" and e - s < 2000: continue
        p = model.proc(th) if kind != "idle" else "idle"
        tid = th if kind != "idle" else 1
        if (p, tid) not in seen:
            seen.add((p, tid)); tr.append(dict(ph="M", name="thread_name", pid=pid(p), tid=tid, args=dict(name=f"{th:08x}" if kind != "idle" else "idle")))
        tr.append(dict(ph="X", name=label, cat=kind, ts=us(s), dur=(e - s) / 1e3, pid=pid(p), tid=tid))
    dev = pid("devices")
    glopen = {}
    for t, ty, m, asid, a, b, c in model.ev:
        if not ok(t): continue
        if ty == LTG["VBL"]: tr.append(dict(ph="i", s="p", name="VBL", ts=us(t), pid=dev, tid=1))
        elif ty == LTG["LATCH"]: tr.append(dict(ph="i", s="t", name=f"latch {b}", ts=us(t), pid=dev, tid=2))
        elif ty == LTG["PUBLISH"]: tr.append(dict(ph="i", s="t", name=f"publish {a}", ts=us(t), pid=dev, tid=4))
        elif ty == LTG["SCALER"]:
            if a == 1: glopen["sc"] = t
            elif a == 0 and "sc" in glopen:
                s0 = glopen.pop("sc"); tr.append(dict(ph="X", name="scale", ts=us(s0), dur=(t - s0) / 1e3, pid=dev, tid=3))
            elif a == 2: tr.append(dict(ph="i", s="t", name="scaler done", ts=us(t), pid=dev, tid=3))
        elif ty == LTG["IRQ_LINE"] and b: tr.append(dict(ph="i", s="t", name=f"irq {c & 0xfff:x}:{a}", ts=us(t), pid=dev, tid=5))
        elif ty == LTG["GL_BEGIN"]: glopen[("gl", b)] = (t, a)
        elif ty == LTG["GL_END"] and ("gl", b) in glopen:
            s0, slot = glopen.pop(("gl", b))
            tr.append(dict(ph="X", name=GL.get(slot, f"gl{slot}"), ts=us(s0), dur=(t - s0) / 1e3, pid=pid("GL"), tid=b))
    for tid, nm in [(1, "VBL"), (2, "display latch"), (3, "scaler"), (4, "host publish"), (5, "IRQ lines")]:
        tr.append(dict(ph="M", name="thread_name", pid=dev, tid=tid, args=dict(name=nm)))
    json.dump(dict(traceEvents=tr, displayTimeUnit="ms"), open(out, "w"))
    print("wrote", out, len(tr), "events")


ap = argparse.ArgumentParser()
ap.add_argument("trace"); ap.add_argument("--frames", action="store_true"); ap.add_argument("--json")
ap.add_argument("--from", dest="lo", type=float); ap.add_argument("--to", dest="hi", type=float)
ap.add_argument("--present-ctx", type=lambda s: int(s, 16))
o = ap.parse_args()
ev, names = load(o.trace)
m = Model(ev, names)
print(f"{len(ev)} events over {(ev[-1][0] - ev[0][0]) / 1e9:.2f} s; types:",
      dict(collections.Counter(T.get(e[1], e[1]) for e in ev)))
print("named threads:", collections.Counter(v for v in names.values()).most_common(12))
if o.frames: frames(m, o.present_ctx)
if o.json: to_json(m, o.json, o.lo, o.hi)
