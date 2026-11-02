#!/usr/bin/env python3
"""Follow pinned n72-7E18 compositor queue selections through guest LCD register writes.

Consumes matching swapcalls/guestpoints/surfacemap/hitchsched reports. The insert
serial is derived from the pinned native caller, not directly observed after
increment. Shared-memory identity and causal dependencies remain unproven.
"""

from pathlib import Path
import argparse
import bisect
import collections
import hashlib
import json


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("capture", type=Path)
    ap.add_argument("--json", type=Path, required=True)
    args = ap.parse_args()
    p = args.capture
    g = json.loads((p / "guest-points.json").read_text())
    q = json.loads((p / "swap-report.json").read_text())
    s = json.loads((p / "surfaces.json").read_text())
    if not g["window"] == q["window"] == s["window"]:
        ap.error("need matching guest/API/surface windows")
    if g["engine_pid"] != q["engine_pid"]:
        ap.error("need matching guest/API workers")
    for name, digest in q["input_sha256"].items():
        if hashlib.sha256((p / name).read_bytes()).hexdigest() != digest:
            ap.error("input identity mismatch: " + name)
    if q["config"] != json.loads((p / "swap-points.json").read_text()):
        ap.error("selected manifest differs from paired API report")
    reference = json.loads(
        Path(__file__).with_name("n72-7e18-queue-points.json").read_text()
    )
    if q["config"]["symbol_sha256"] != reference["symbol_sha256"]:
        ap.error("need pinned n72-7E18 kernel/cache")
    required = {
        "ca-native-swap",
        "ca-image-queue-insert",
        "ca-insert-serial",
        "ca-render-queue-entry",
        "ca-render-queue-done",
        "ca-render-generation",
        "ca-render-image-serial",
        "ca-copy-image-info",
        "ca-copy-selected-serial",
    }
    expected = {
        (x["pc"], x["name"], x["role"])
        for x in reference["points"]
        if x["name"] in required
    }
    observed = {
        (x["pc"], x["name"], x["role"])
        for x in q["config"]["points"]
        if x["name"] in required
    }
    if expected != observed:
        ap.error("missing or altered pinned queue observations")
    points = {x["pc"]: x["name"] for x in q["config"]["points"]}
    b, e = (g["window"][k] for k in ("begin_ns", "end_exclusive_ns"))
    h = json.loads((p / "hitches.json").read_text())
    if (
        h["engine_pid"] != g["engine_pid"]
        or (h["window"]["begin_ns"], h["window"]["end_exclusive_ns"]) != (b, e)
        or h.get("trace_errors")
        or not h.get("vcpu_states")
    ):
        ap.error("need matching worker/window and complete scheduler states")
    states = h["vcpu_states"]
    offset = h["clock_offset_ns"]
    st = [a["ts"] - offset for a in states]
    rows = sorted(g["samples"] + g["outside_window"], key=lambda x: x["begin_ns"])
    byname = collections.defaultdict(list)
    for row in rows:
        byname[points[row["pc"]]].append(row)
    changes = byname["scheduled-thread"]
    ct = [r["begin_ns"] for r in changes]

    def split(a, z):
        lo, hi = max(a, b), min(z, e)
        out = collections.Counter()
        cursor = lo
        i = max(0, bisect.bisect_right(st, lo) - 1)
        while i < len(states) and st[i] < hi:
            x, y = max(lo, st[i]), min(hi, states[i]["end_ns"] - offset)
            if y > x:
                if x != cursor:
                    raise RuntimeError("Host state gap/overlap")
                out[states[i]["state"]] += (y - x) / 1e6
                cursor = y
            i += 1
        if hi > lo and cursor != hi:
            raise RuntimeError("Incomplete host span")
        return dict(out)

    def residency(a, z):
        lo, hi = max(a, b), min(z, e)
        out = collections.defaultdict(collections.Counter)
        i = bisect.bisect_right(ct, lo) - 1
        if hi > lo and i < 0:
            raise RuntimeError("Missing guest thread")
        while i < len(changes) and ct[i] < hi:
            x, y = max(lo, ct[i]), min(hi, ct[i + 1] if i + 1 < len(changes) else hi)
            if y > x:
                out[changes[i]["thread"]].update(split(x, y))
            i += 1
        return [
            dict(thread=t, owner=q["owners"].get(str(t)), host_ms=dict(v))
            for t, v in sorted(out.items(), key=lambda item: -item[1].get("Running", 0))
        ]

    api = q["calls"]
    byapi = collections.defaultdict(list)
    for c in api:
        byapi[c["name"]].append(c)
    # Pair void render updates by thread, preserving partial boundary observations.
    active = collections.defaultdict(list)
    updates = []
    initial = []
    for row in rows:
        name = points[row["pc"]]
        t = row["thread"]
        if name == "ca-render-queue-entry":
            active[t].append(
                dict(
                    thread=t,
                    object=row["r0"],
                    begin_ns=row["begin_ns"],
                    generations=[],
                    serials=[],
                    shmem_entry=[],
                    shmem_done=[],
                )
            )
        elif name in (
            "ca-render-queue-done",
            "ca-render-generation",
            "ca-render-image-serial",
            "ca-shmem-update-entry",
            "ca-shmem-update-done",
        ):
            if not active[t]:
                if row["begin_ns"] >= b:
                    raise RuntimeError("Unmatched render boundary")
                initial.append(row)
                continue
            u = active[t][-1]
            if name == "ca-render-queue-done":
                u.update(end_ns=row["begin_ns"], id=len(updates))
                updates.append(u)
                active[t].pop()
            elif name == "ca-render-generation":
                if row["r0"] != u["object"]:
                    raise RuntimeError("Render update object mismatch")
                u["generations"].append(
                    dict(
                        ns=row["begin_ns"],
                        previous=row["r2"],
                        current=row["r3"],
                        update_object=row["r1"],
                    )
                )
            elif name == "ca-render-image-serial":
                u["serials"].append(
                    dict(ns=row["begin_ns"], selected=row["r1"], previous=row["r3"])
                )
            elif name == "ca-shmem-update-entry":
                u["shmem_entry"].append(
                    dict(ns=row["begin_ns"], queue=row["r0"], owner=row["r1"])
                )
            else:
                u["shmem_done"].append(row["begin_ns"])
    uc = collections.defaultdict(list)
    for u in updates:
        uc[u["thread"]].append(u)
    ut = {t: [u["begin_ns"] for u in us] for t, us in uc.items()}

    def enclosing_update(c):
        us = uc[c["thread"]]
        i = bisect.bisect_right(ut.get(c["thread"], []), c["begin_ns"]) - 1
        return us[i] if i >= 0 and c["end_ns"] <= us[i]["end_ns"] else None

    queries = []
    selected = byname["ca-copy-selected-serial"]
    selected_times = [r["begin_ns"] for r in selected]
    for c in byapi["ca-copy-image-info"]:
        u = enclosing_update(c)
        chosen = [
            r
            for r in selected[
                bisect.bisect_left(selected_times, c["begin_ns"]) : bisect.bisect_left(
                    selected_times, c["end_ns"]
                )
            ]
            if r["thread"] == c["thread"]
        ]
        if c["exit"]["r0"] == 1 and len(chosen) != 1:
            raise RuntimeError("Successful query needs one selected serial")
        if c["exit"]["r0"] == 0 and chosen:
            raise RuntimeError("Failed query selected a serial")
        queries.append(
            dict(
                id=len(queries),
                call_id=c["id"],
                update_id=u["id"] if u else None,
                queue=c["entry"]["r0"],
                thread=c["thread"],
                begin_ns=c["begin_ns"],
                end_ns=c["end_ns"],
                result=c["exit"]["r0"],
                selected_serial=chosen[0]["r0"] if len(chosen) == 1 else None,
                selected_ns=chosen[0]["begin_ns"] if len(chosen) == 1 else None,
            )
        )
    inserts = []
    serials = byname["ca-insert-serial"]
    serial_times = [r["begin_ns"] for r in serials]
    native_by_thread = collections.defaultdict(list)
    for c in byapi["ca-native-swap"]:
        native_by_thread[c["thread"]].append(c)
    nt = {t: [c["begin_ns"] for c in cs] for t, cs in native_by_thread.items()}
    for c in byapi["ca-image-queue-insert"]:
        candidates = [
            r
            for r in serials[
                bisect.bisect_left(serial_times, c["begin_ns"]) : bisect.bisect_left(
                    serial_times, c["end_ns"]
                )
            ]
            if r["thread"] == c["thread"]
        ]
        cs = native_by_thread[c["thread"]]
        i = bisect.bisect_right(nt.get(c["thread"], []), c["begin_ns"]) - 1
        native = cs[i] if i >= 0 and c["end_ns"] <= cs[i]["end_ns"] else None
        # The verified native caller always sets flag bit1 (0x10002/0x10003,
        # optionally OR 0x10/0x1). Insert increments the slot serial when flags&6
        # at0x31b84140, after our base-serial point. Preserve both, and infer the
        # published serial only for this pinned native caller, never arbitrary APIs.
        if native and (c["entry"]["pc"] != 0x31BED31C or c["entry"]["r3"] != 4):
            raise RuntimeError("Unexpected native insert callsite/type")
        inserts.append(
            dict(
                id=len(inserts),
                call_id=c["id"],
                native_id=native["id"] if native else None,
                queue=c["entry"]["r0"],
                thread=c["thread"],
                begin_ns=c["begin_ns"],
                end_ns=c["end_ns"],
                result=c["exit"]["r0"],
                surface_ids=native["surface_ids"] if native else [],
                serial_observations=[
                    dict(
                        ns=r["begin_ns"],
                        base_serial=r["r3"],
                        serial=(r["r3"] + 1) & 0xFFFFFFFF if native else None,
                        derivation="Pinned native caller sets bit1; conditional increment at0x31b84140",
                    )
                    for r in candidates
                ],
            )
        )
    byserial = collections.defaultdict(list)
    for a in inserts:
        if (
            a["result"]
            and len(a["serial_observations"]) == 1
            and a["serial_observations"][0]["serial"] is not None
        ):
            byserial[a["serial_observations"][0]["serial"]].append(a)
    for c in queries:
        c["preceding_insert_ids"] = (
            [
                a["id"]
                for a in byserial[c["selected_serial"]]
                if a["serial_observations"][0]["ns"] < c["selected_ns"]
            ]
            if c["selected_serial"] is not None
            else []
        )
    by_native = {a["native_id"]: a for a in inserts if a["native_id"] is not None}
    query_by_insert = collections.defaultdict(list)
    for c in queries:
        for i in c["preceding_insert_ids"]:
            query_by_insert[i].append(c)
    present_next = {}
    prs = collections.defaultdict(list)
    for c in s["calls"]:
        assoc = q["present_associations"].get(str(c["id"]))
        if assoc:
            prs[assoc["thread"]].append(c)
    for t, cs in prs.items():
        for i, c in enumerate(cs):
            present_next[c["id"]] = cs[i + 1]["begin_ns"] if i + 1 < len(cs) else e
    flips = []
    phases = collections.Counter()
    for f in q["flips"]:
        write = s["calls"][f["write_call_id"]]
        assoc = f["association"]
        t = assoc["thread"] if assoc else None
        cs = native_by_thread[t]
        ns = nt.get(t, [])
        candidates = cs[
            bisect.bisect_left(ns, write["end_ns"]) : bisect.bisect_left(
                ns, present_next.get(write["id"], e)
            )
        ]
        native = candidates[0] if len(candidates) == 1 else None
        a = by_native.get(native["id"]) if native else None
        qs = (
            [c for c in query_by_insert[a["id"]] if c["selected_ns"] < f["ns"]]
            if a
            else []
        )
        ticks = []
        for tick in f["held_ticks"]:
            now = tick["begin_ns"]
            if write["end_ns"] > now:
                phase = "write-pending"
            elif qs and qs[0]["selected_ns"] <= now:
                phase = "selected-before-held-tick"
            elif not native:
                phase = "native-ambiguous"
            elif native["end_ns"] > now:
                phase = "notification-pending"
            elif not qs:
                phase = "no-matched-query-before-flip"
            elif qs[0]["selected_ns"] > now:
                phase = "not-yet-selected"
            else:
                phase = "selected-before-held-tick"
            phases[phase] += 1
            ticks.append(dict(tick, queue_phase=phase))
        stages = []
        if native and qs:
            selected_ns = qs[0]["selected_ns"]
            bounds = [
                ("notification-to-selection", native["end_ns"], selected_ns),
                ("selection-to-flip", selected_ns, f["ns"]),
            ]
            for name, x, y in bounds:
                stages.append(
                    dict(
                        name=name,
                        begin_ns=x,
                        end_ns=y,
                        ms=(y - x) / 1e6,
                        host_ms=split(x, y) if y >= x else None,
                        guest_threads=residency(x, y) if y >= x else None,
                    )
                )
        flips.append(
            dict(
                flip_id=f["id"],
                ns=f["ns"],
                native_ids=[c["id"] for c in candidates],
                insert_id=a["id"] if a else None,
                query_ids=[c["id"] for c in qs],
                ticks=ticks,
                stages=stages,
            )
        )
    window_updates = [u for u in updates if b <= u["begin_ns"] < u["end_ns"] < e]
    summary = dict(
        updates=len(window_updates),
        update_objects=dict(
            collections.Counter(hex(u["object"]) for u in window_updates)
        ),
        generation_patterns=dict(
            collections.Counter(
                (
                    "equal"
                    if u["generations"][0]["previous"] == u["generations"][0]["current"]
                    else "changed"
                )
                for u in window_updates
                if len(u["generations"]) == 1
            )
        ),
        queries=len([c for c in queries if b <= c["begin_ns"] < c["end_ns"] < e]),
        query_results=dict(
            collections.Counter(
                c["result"] for c in queries if b <= c["begin_ns"] < c["end_ns"] < e
            )
        ),
        insert_serial_counts=dict(
            collections.Counter(
                len(a["serial_observations"])
                for a in inserts
                if b <= a["begin_ns"] < a["end_ns"] < e
            )
        ),
        tick_phases=dict(phases),
        flips=len(flips),
    )
    report = dict(
        window=g["window"],
        engine_pid=g["engine_pid"],
        summary=summary,
        updates=updates,
        queries=queries,
        inserts=inserts,
        flips=flips,
        initial=initial,
        pending=[u for us in active.values() for u in us],
        limits=q["limits"]
        + [
            "Void update boundaries have no success result.",
            "Producer/consumer pointers are process-local; equal serials give candidates, not independent queue identity.",
            "Reached serial store/selection points precede their execution; API epilogues precede completed returns.",
        ],
    )
    report["input_sha256"] = {
        n: hashlib.sha256((p / n).read_bytes()).hexdigest()
        for n in (
            "guest-points.json",
            "hitches.json",
            "swap-report.json",
            "surfaces.json",
            "swap-points.json",
        )
    }
    args.json.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
