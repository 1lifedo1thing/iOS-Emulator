#!/usr/bin/env python3
"""Join complete LT_FMSS_PATH calls to a matching host scheduler/frame report.

Nested fsyncs/erases are included in parent durations. Exclusive totals subtract
direct children. A returned call is not proof of successful or durable I/O;
retain the engine log and reject storage errors separately.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import statistics


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('events', type=Path)
    ap.add_argument('scheduler', type=Path, help='hitchsched.py --all-states JSON')
    ap.add_argument('--engine-pid', required=True, type=int)
    ap.add_argument('--json', required=True, type=Path)
    args = ap.parse_args()
    sched = json.loads(args.scheduler.read_text())
    if sched['engine_pid'] != args.engine_pid or sched.get('trace_errors'):
        ap.error('worker mismatch or scheduler trace errors')
    begin, end = (sched['window'][k] for k in ('begin_ns', 'end_exclusive_ns'))
    offset = sched['clock_offset_ns']
    states = sched.get('vcpu_states')
    if not states:
        ap.error('need whole-window scheduler states')
    starts = [row['ts'] - offset for row in states]

    def split(a, b):
        counts, cursor = Counter(), a
        i = max(0, bisect.bisect_right(starts, a) - 1)
        while i < len(states) and starts[i] < b:
            row = states[i]
            lo, hi = max(a, starts[i]), min(b, row['end_ns'] - offset)
            if hi > lo:
                if lo != cursor:
                    raise ValueError('scheduler gap/overlap in NAND call')
                counts[row['state']] += hi - lo
                cursor = hi
            i += 1
        if cursor != b:
            raise ValueError('incomplete scheduler coverage of NAND call')
        return counts

    lines = args.events.read_text().splitlines()
    if not lines or not lines[0].startswith('# LT_FRAME_PATH 1 '):
        ap.error('unsupported/missing frame-path header')
    footer = re.fullmatch(r'# end records=(\d+) dropped=(\d+) incomplete=(\d+) end-ns=(\d+)', lines[-1])
    if (not footer or int(footer[1]) != len(lines) - 2 or int(footer[2]) or
            int(footer[3]) or int(footer[4]) < end):
        ap.error('need complete, lossless trace covering scheduler window')
    calls, stack, seen = {}, [], set()
    names = {1: 'load', 2: 'store', 3: 'erase', 4: 'fsync'}
    previous = 0
    for line in lines[1:-1]:
        fields = line.split()
        if len(fields) != 5 or not all(fields[i].isdigit() for i in (0, 2, 3, 4)):
            ap.error('invalid frame-path record')
        if fields[1] not in ('fmss-begin', 'fmss-end'):
            continue
        ns, ident, packed, page = (int(fields[i]) for i in (0, 2, 3, 4))
        operation, cs = packed >> 32, packed & 0xffffffff
        if (not ident or operation not in names or cs > 3 or page > 0xffffffff or
                ns < previous):
            ap.error('invalid NAND metadata/timestamp')
        previous = ns
        if fields[1] == 'fmss-begin':
            if ident in seen:
                ap.error('reused NAND call ID')
            seen.add(ident)
            parent = stack[-1] if stack else None
            calls[ident] = dict(id=ident, operation=names[operation], cs=cs,
                                page_or_block=page, begin_ns=ns, parent=parent,
                                children=[])
            if parent:
                calls[parent]['children'].append(ident)
            stack.append(ident)
        else:
            if not stack or stack.pop() != ident:
                ap.error('missing/non-nested NAND begin/end')
            row = calls[ident]
            if (row['operation'], row['cs'], row['page_or_block']) != (names[operation], cs, page):
                ap.error('NAND begin/end coordinates disagree')
            row.update(end_ns=ns, wall_ms=(ns - row['begin_ns']) / 1e6)
    if stack or not calls:
        ap.error('incomplete NAND calls or no NAND events')
    selected, totals, walls = [], defaultdict(lambda: defaultdict(Counter)), defaultdict(list)
    for row in calls.values():
        if not (row['begin_ns'] < end and row['end_ns'] > begin or
                row['begin_ns'] == row['end_ns'] and begin <= row['begin_ns'] < end):
            continue
        a, b = max(begin, row['begin_ns']), min(end, row['end_ns'])
        inclusive = split(a, b)
        exclusive = inclusive.copy()
        for child_id in row['children']:
            child = calls[child_id]
            ca, cb = max(a, child['begin_ns']), min(b, child['end_ns'])
            if cb > ca:
                exclusive.subtract(split(ca, cb))
        if any(value < 0 for value in exclusive.values()):
            ap.error('negative exclusive scheduler duration')
        row.update(overlap_begin_ns=a, overlap_end_ns=b,
                   inclusive_ms={k: v / 1e6 for k, v in inclusive.items()},
                   exclusive_ms={k: v / 1e6 for k, v in exclusive.items()},
                   complete_in_window=a == row['begin_ns'] and b == row['end_ns'])
        selected.append(row)
        totals[row['operation']]['inclusive'].update(inclusive)
        totals[row['operation']]['exclusive'].update(exclusive)
        if row['complete_in_window']:
            walls[row['operation']].append(row['wall_ms'])
    summaries = {}
    for operation, sums in totals.items():
        durations = sorted(walls[operation])
        summaries[operation] = dict(
            overlapping_calls=sum(c['operation'] == operation for c in selected),
            complete_calls=len(durations),
            median_ms=statistics.median(durations) if durations else None,
            p99_ms=durations[round(.99 * (len(durations) - 1))] if durations else None,
            max_ms=max(durations) if durations else None,
            **{key + '_ms': {k: v / 1e6 for k, v in counts.items()}
               for key, counts in sums.items()})
    waits = []
    for hitch in sched['hitches']:
        for state in hitch['vcpu']['events']:
            a, b = (state[k] - offset for k in ('overlap_begin_ns', 'overlap_end_ns'))
            if state['state'] != 'D' or b - a < 1_000_000:
                continue
            overlaps = [c for c in selected if c['begin_ns'] < b and c['end_ns'] > a]
            enclosed = [c for c in overlaps if c['begin_ns'] <= a and c['end_ns'] >= b]
            inner = min(enclosed, key=lambda c: c['wall_ms']) if enclosed else None
            waits.append(dict(hitch_end_s=hitch['relative_end_s'], begin_ns=a,
                              end_ns=b, ms=(b-a)/1e6,
                              enclosing_call_id=inner['id'] if inner else None,
                              enclosing_operation=inner['operation'] if inner else None,
                              overlapping_call_ids=[c['id'] for c in overlaps]))
    selected_ids = {row['id'] for row in selected}
    outside = [dict(row, phase='before' if row['end_ns'] <= begin else 'after')
               for row in calls.values() if row['id'] not in selected_ids]
    outside_summary = {}
    for phase in ('before', 'after'):
        outside_summary[phase] = {}
        for operation in names.values():
            durations = [r['wall_ms'] for r in outside
                         if r['phase'] == phase and r['operation'] == operation]
            if durations:
                outside_summary[phase][operation] = dict(calls=len(durations),
                                                         max_ms=max(durations))
    report = dict(engine_pid=args.engine_pid,
                  window=dict(begin_ns=begin, end_exclusive_ns=end),
                  trace_calls=len(calls), operations=summaries, calls=selected,
                  outside_window=outside, outside_operations=outside_summary,
                  hitch_uninterruptible_spans=waits,
                  limitation='Inclusive spans overlap nested operations. Enclosure locates the sleep within a call; it does not identify the kernel wait mechanism or prove I/O success.',
                  input_sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in (args.events, args.scheduler)})
    args.json.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(summaries, indent=2))
    print('Outside frame window:', json.dumps(outside_summary))
    for wait in waits:
        print('HITCH D:', json.dumps(wait))


if __name__ == '__main__':
    main()
