#!/usr/bin/env python3
"""Attribute host CPU samples of the vCPU thread to the guest timeline (TEMP diagnostic).

usage: gtracesamples.py TRACE PERFSCRIPT --kernel K --cache C [--top N] [--bucket SUBSTR ...]

PERFSCRIPT is `perf script -F tid,time,ip,sym --ns` (Linux, `perf record -k CLOCK_MONOTONIC`) or
`simpleperf report-sample`-style text with the same columns, from the vCPU thread. Each sample lands
in the timeline's state at its time: process, user/kernel/idle, and the kernel entry (system call,
irq, fault) it is inside. Prints each bucket's share and its top functions: guest code by name
(perf-map rows `guest-0xADDR`), host code as `host:symbol` (helpers, TLB refills, GL).
"""
import argparse, bisect, collections, os, re, sys

D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import guestsymbols  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("trace"); ap.add_argument("perfscript")
ap.add_argument("--kernel"); ap.add_argument("--cache", required=True)
ap.add_argument("--top", type=int, default=12); ap.add_argument("--bucket", action="append", default=[])
ap.add_argument("--buckets", type=int, default=14)
ap.add_argument("--threads", action="store_true", help="split kernel-only threads by thread pointer")
ap.add_argument("--kernmap", help="perf/n81/kernmap: name kernel code by its categories and best-guess names")
o = ap.parse_args()

sys.argv = [sys.argv[0], o.trace]
gt = {}
exec(open(os.path.join(D, "gtrace.py")).read().split("ap = argparse.ArgumentParser()")[0], gt)
ev, names = gt["load"](o.trace)
model = gt["Model"](ev, names)
def owner(th, kind):
    p = model.proc(th) if kind != "idle" else "idle"
    return f"{p}@{th:08x}" if o.threads and p == "kernel" and th is not None else p


spans = [(s, e, owner(th, kind), kind, label) for s, e, th, kind, label in model.slices()]
spans.sort()
starts = [s[0] for s in spans]
t_lo, t_hi = ev[0][0], ev[-1][0]

LINE = re.compile(r"^\s*(\d+)\s+(\d+\.\d+):\s+([0-9a-fA-F]+)\s+(.*)$")
samples = []
for l in open(o.perfscript, errors="replace"):
    m = LINE.match(l)
    if not m: continue
    t = int(round(float(m[2]) * 1e9))
    if t_lo <= t <= t_hi:
        samples.append((t, int(m[3], 16), m[4].strip()))
print(f"{len(samples)} samples inside the trace window ({(t_hi - t_lo) / 1e9:.1f} s)")
if not samples:
    sys.exit("no overlap: check perf record -k CLOCK_MONOTONIC and the windows")

syms = guestsymbols.GuestSymbols(o.kernel and __import__("pathlib").Path(o.kernel), __import__("pathlib").Path(o.cache))
GUEST = re.compile(r"guest-0x([0-9a-fA-F]+)")
user_rows = collections.Counter()
for t, ip, sym in samples:
    m = GUEST.search(sym)
    if m and int(m[1], 16) < 0x80000000: user_rows[int(m[1], 16)] += 1
slide, hit, second = guestsymbols.estimate_slide(syms, [(w, pc) for pc, w in user_rows.items()])
print(f"cache slide 0x{slide:x} ({hit} samples on entries, runner-up {second})")


KM = None
if o.kernmap:
    import pickle
    o.kernmap = os.path.abspath(o.kernmap); here = os.getcwd(); os.chdir(o.kernmap); sys.path.insert(0, o.kernmap)
    import classify, names as kmnames  # noqa: E402
    kstarts = sorted(pickle.load(open("starts.pkl", "rb"))[0])
    os.chdir(here)
    KM = (classify.cat, kmnames.NAMES, kstarts)


def kname(pc):
    if not KM:
        return f"{pc:08x}"
    cat, NAMES, ks = KM
    i = bisect.bisect_right(ks, pc) - 1
    start = ks[i] if i >= 0 else pc
    nm = NAMES.get(start, (None,))[0]
    return f"[{cat(pc)}] " + (nm or f"fn@{start:08x}")


def name(sym):
    m = GUEST.search(sym)
    if not m:
        return "host:" + (sym.split("+")[0] or "?")
    pc = int(m[1], 16)
    if pc >= 0x80000000:
        return "kern:" + kname(pc)
    if syms.cache_lo + slide <= pc < syms.cache_hi + slide:
        f = syms.locate(pc - slide)
        if f: return f"{f[0].rsplit('/', 1)[-1]}!{f[1] or '?'}"
    return "app" if pc < 0x2fe00000 else "dyld"


B = collections.defaultdict(collections.Counter)
for t, ip, sym in samples:
    i = bisect.bisect_right(starts, t) - 1
    if i >= 0 and t < spans[i][1]:
        s, e, p, kind, label = spans[i]
        key = f"{p}:{kind}" + (f":{label}" if kind == "kernel" else "")
    else:
        key = "?"
    B[key][name(sym)] += 1
tot = len(samples)
order = sorted(B.items(), key=lambda kv: -sum(kv[1].values()))
sel = [kv for kv in order if not o.bucket or any(b in kv[0] for b in o.bucket)][:o.buckets]
for key, c in sel:
    n = sum(c.values())
    print(f"\n== {key}: {100 * n / tot:.1f}% of vCPU samples")
    cats = collections.Counter()
    for f, k in c.items():
        cats["host" if f.startswith("host:") else f.split("] ")[0] + "]" if f.startswith("kern:[") else
             "kernel" if f.startswith("kern:") else "user"] += k
    print("   by kind:", ", ".join(f"{x} {100 * k / n:.0f}%" for x, k in cats.most_common(6)))
    for f, k in c.most_common(o.top):
        print(f"   {100 * k / n:5.1f}%  {f}")
# host-side totals: how much of all time is host code (helpers) vs guest TBs
kinds = collections.Counter("guest" if GUEST.search(s) else "host" for _, _, s in samples)
print("\nall samples:", {k: f"{100 * v / tot:.1f}%" for k, v in kinds.items()})
hostfn = collections.Counter(name(s) for _, _, s in samples if not GUEST.search(s))
print("top host functions:", ", ".join(f"{f[5:]} {100 * k / tot:.1f}%" for f, k in hostfn.most_common(15)))
