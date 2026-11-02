#!/usr/bin/env python3
"""Pair pinned guest swap API observations and split their host/guest execution.

Input directory contains guest-points.json, hitches.json (with --all-states),
surfaces.json and a detached same-worker guest-processes.json. No phone access.
Entry/epilogue points and arguments require the matching firmware disassembly.
Thread ownership is a post-window observation, subject to lifetime continuity.
"""
import argparse
import bisect
import collections
import hashlib
import json
from pathlib import Path
import statistics


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--engine-pid', type=int, required=True)
    ap.add_argument('--points', type=Path, default=Path(__file__).with_name('n72-7e18-swap-points.json'))
    ap.add_argument('--json', type=Path, required=True)
    args = ap.parse_args()
    p = args.capture
    g = json.loads((p / 'guest-points.json').read_text())
    s = json.loads((p / 'hitches.json').read_text())
    surface = json.loads((p / 'surfaces.json').read_text())
    owner = json.loads((p / 'guest-processes.json').read_text())
    config = json.loads(args.points.read_text())
    begin, end = (g['window'][k] for k in ('begin_ns', 'end_exclusive_ns'))
    if (not owner.get('detached') or not owner.get('task_thread_enumeration') or
            {owner['engine_pid'], g['engine_pid'], s['engine_pid']} != {args.engine_pid} or
            g['window'] != surface['window'] or
            (begin, end) != (s['window']['begin_ns'], s['window']['end_exclusive_ns']) or
            s.get('trace_errors') or not s.get('vcpu_states')):
        ap.error('need matching worker/window, complete scheduler states and detached task/thread snapshot')
    if (g.get('symbol_sha256') != config['symbol_sha256'] or
            owner['kernel_sha256'] != config['symbol_sha256']['kernel'] or
            g['input_sha256']['processes'] != hashlib.sha256((p / 'guest-processes.json').read_bytes()).hexdigest() or
            hashlib.sha256((p / 'hitches.json').read_bytes()).hexdigest() not in surface['inputs'].values() or
            not {g['input_sha256']['events'], g['input_sha256']['frames']}.issubset(set(surface['inputs'].values()))):
        ap.error('firmware, process snapshot or raw milestone/frame identity mismatch')
    entries = config['points']
    if (not 1 <= len(entries) <= 32 or len({e['pc'] for e in entries}) != len(entries) or
            not any(e['role'] == 'switch' for e in entries) or
            any(e['role'] not in {'entry', 'exit', 'surface-id', 'switch', 'enqueue', 'observation'} or
                not 0 <= e['pc'] <= 0xffffffff or not e['name'] or
                (not e['pc'] & 1 and e['pc'] & 3) for e in entries)):
        ap.error('invalid selected point manifest')

    points = {e['pc']: e for e in config['points']}
    rows = sorted(g['samples'] + g['outside_window'], key=lambda e: e['begin_ns'])
    if any(row['pc'] not in points for row in rows):
        ap.error('capture contains points absent from the selected manifest')
    # These boundaries follow the kernel's TPIDRPRW write. Residency below
    # includes kernel and native work while that guest thread is selected.
    changes = [e for e in rows if points[e['pc']]['role'] == 'switch']
    ct = [e['begin_ns'] for e in changes]

    def threadat(ns):
        i = bisect.bisect_right(ct, ns) - 1
        return changes[i]['thread'] if i >= 0 else None
    for row in rows:
        if threadat(row['begin_ns']) is not None and threadat(row['begin_ns']) != row['thread']:
            raise RuntimeError('Guest switch continuity mismatch')
    states = s['vcpu_states']
    offset = s['clock_offset_ns']
    st = [e['ts'] - offset for e in states]

    def hostsplit(a, b):
        lo, hi = (max(a, begin), min(b, end))
        cursor = lo
        out = collections.Counter()
        i = max(0, bisect.bisect_right(st, lo) - 1)
        while i < len(states) and st[i] < hi:
            e = states[i]
            x, y = (max(lo, st[i]), min(hi, e['end_ns'] - offset))
            if y > x:
                if x != cursor:
                    raise RuntimeError('Host scheduler gap/overlap')
                out[e['state']] += (y - x) / 1000000.0
                cursor = y
            i += 1
        if hi > lo and cursor != hi:
            raise RuntimeError('Incomplete scheduler coverage')
        return dict(covered_ms=max(0, hi - lo) / 1000000.0, outside_ms=(b - a) / 1000000.0 - max(0, hi - lo) / 1000000.0, ms=dict(out))

    def residency(a, b):
        lo, hi = (max(a, begin), min(b, end))
        i = bisect.bisect_right(ct, lo) - 1
        out = collections.defaultdict(collections.Counter)
        if hi <= lo:
            return {}
        if hi > lo and i < 0:
            raise RuntimeError('Missing selected thread')
        while i < len(changes) and ct[i] < hi:
            x, y = (max(lo, ct[i]), min(hi, ct[i + 1] if i + 1 < len(changes) else hi))
            if y > x:
                out[changes[i]['thread']].update(hostsplit(x, y)['ms'])
            i += 1
        return {str(t): dict(c) for t, c in out.items()}
    owners = {t: dict(pid=o['pid'], name=o['name'], task=o['task']) for o in owner['task_thread_enumeration'] for t in o['threads']}
    calls = []
    # Per-thread stacks allow nested calls. A reached epilogue precedes the
    # return instruction; r0 is its observed result, not proof of return.
    stack = collections.defaultdict(list)
    initial = []
    id_orphans = []
    for row in rows:
        meta = points[row['pc']]
        name, role = (meta['name'], meta['role'])
        key = (row['thread'], name)
        if role == 'entry':
            stack[key].append(dict(name=name, thread=row['thread'], begin_ns=row['begin_ns'], entry=row, surface_ids=[]))
        elif role == 'exit':
            if not stack[key]:
                if row['begin_ns'] >= begin:
                    raise RuntimeError('Unmatched in-window API exit')
                initial.append(row)
                continue
            c = stack[key].pop()
            c.update(
                id=len(calls), end_ns=row['begin_ns'], exit=row,
                wall_ms=(row['begin_ns'] - c['begin_ns']) / 1000000.0,
                host=hostsplit(c['begin_ns'], row['begin_ns']),
                guest_threads=residency(c['begin_ns'], row['begin_ns']))
            calls.append(c)
        elif role == 'surface-id':
            if stack[key]:
                stack[key][-1]['surface_ids'].append(row['r0'])
            else:
                if row['begin_ns'] >= begin:
                    raise RuntimeError('Unmatched in-window surface ID point')
                id_orphans.append(row)
    window = [c for c in calls if begin <= c['begin_ns'] < c['end_ns'] < end]
    summary = {}
    for name in sorted({c['name'] for c in calls}):
        cs = [c for c in window if c['name'] == name]
        wall = [c['wall_ms'] for c in cs]
        summary[name] = dict(
            calls=len(cs), median_ms=statistics.median(wall) if wall else None,
            max_ms=max(wall, default=None),
            results=dict(collections.Counter(c['exit']['r0'] for c in cs)),
            threads=dict(collections.Counter(hex(c['thread']) for c in cs)))
    unlocks = collections.defaultdict(list)
    api_by_thread = collections.defaultdict(list)
    for c in sorted(calls, key=lambda e: e['begin_ns']):
        api_by_thread[c['thread']].append(c)
        if c['name'] == 'surface-unlock-nonnull':
            unlocks[c['thread']].append(c)
    ut = {t: [c['begin_ns'] for c in cs] for t, cs in unlocks.items()}
    at = {t: [c['begin_ns'] for c in cs] for t, cs in api_by_thread.items()}
    presents = collections.defaultdict(list)
    for c in surface['calls']:
        t = threadat(c['begin_ns'])
        if t is not None:
            if t != threadat(c['end_ns']):
                raise RuntimeError('Thread changed inside host present')
            presents[t].append(c)
    associations = {}
    # Pair only ordered same-thread observations between host presents.
    # Unlock/notification dependencies cannot be inferred from this alone.
    for thread, cs in presents.items():
        for i, c in enumerate(cs):
            after = cs[i + 1]['begin_ns'] if i + 1 < len(cs) else end
            j = bisect.bisect_left(ut.get(thread, []), c['end_ns'])
            u = unlocks[thread][j] if j < len(unlocks[thread]) and unlocks[thread][j]['begin_ns'] < after else None
            following = [
                a['id'] for a in api_by_thread[thread][
                    bisect.bisect_left(at[thread], c['end_ns']):
                    bisect.bisect_left(at[thread], after)]
                if a['name'] != 'surface-unlock-nonnull']
            associations[c['id']] = dict(
                thread=thread, unlock_call_id=u['id'] if u else None,
                write_to_unlock_begin_ms=(u['begin_ns'] - c['end_ns']) / 1000000.0 if u else None,
                following_same_thread_api_ids=following)
    api_sorted = sorted(calls, key=lambda c: c['begin_ns'])
    api_times = [c['begin_ns'] for c in api_sorted]
    max_ends = []
    # Prefix maxima let each overlap query stop without scanning all APIs.
    last_end = 0
    for c in api_sorted:
        last_end = max(last_end, c['end_ns'])
        max_ends.append(last_end)
    flip_reports = []
    for f in surface['flips']:
        if not begin <= f['ns'] < end:
            continue
        call = surface['calls'][f['last_matching_call_id']] if f['last_matching_call_id'] is not None else None
        if call is None:
            continue
        a, b = (call['end_ns'], f['ns'])
        inside = []
        i = bisect.bisect_left(api_times, b) - 1
        while i >= 0 and max_ends[i] > a:
            if api_sorted[i]['end_ns'] > a:
                inside.append(api_sorted[i]['id'])
            i -= 1
        inside.sort()
        flip_reports.append(dict(
            id=f['id'], ns=b, held_ticks=f['held_refreshes'],
            match_status=f['match_status'], write_call_id=call['id'],
            association=associations.get(call['id']),
            write_to_flip_ms=(b - a) / 1000000.0, host=hostsplit(a, b),
            guest_threads=residency(a, b), api_ids=inside))
    report = dict(
        window=g['window'], engine_pid=g['engine_pid'], config=config,
        summary=summary, owners={str(k): v for k, v in owners.items()},
        calls=calls, initial_exits=initial, initial_id_points=id_orphans,
        pending_entries=[c for cs in stack.values() for c in cs],
        present_associations=associations, flips=flip_reports,
        limits=[
            'Post-window task ownership requires lifetime continuity; do not apply another worker owners.',
            'API epilogue observations precede return execution.',
            'Following same-thread APIs are ordered observations, not a proven buffer/transaction dependency.',
            'Other physical buffer writers remain untraced.'])
    report['input_sha256'] = {name: hashlib.sha256((p / name).read_bytes()).hexdigest() for name in ('guest-points.json', 'hitches.json', 'surfaces.json', 'guest-processes.json')}
    args.json.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print('initial exits', len(initial), 'pending', len(report['pending_entries']), 'observed points', dict(collections.Counter((hex(x['pc']) for x in rows))))
if __name__ == "__main__":
    main()
