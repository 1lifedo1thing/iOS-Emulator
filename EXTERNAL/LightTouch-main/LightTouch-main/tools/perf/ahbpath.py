#!/usr/bin/env python3
"""Join optional Android AHardwareBuffer stages with host scheduling.

Requires LT_GLES_AHB_PATH=on, LT_FRAME_PATH and a clean Stop. Pass the matching
hitchsched.py --all-states --runnable-details report. Wall times include host
scheduling; enclosing a wait identifies an operation, not a driver defect.
"""
import argparse
import bisect
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('events', type=Path)
    ap.add_argument('engine_log', type=Path)
    ap.add_argument('scheduler', type=Path)
    ap.add_argument('--engine-pid', type=int, required=True)
    ap.add_argument('--json', type=Path, required=True)
    args = ap.parse_args()
    sched = json.loads(args.scheduler.read_text())
    if sched['engine_pid'] != args.engine_pid or sched['trace_errors']:
        ap.error('scheduler worker mismatch or trace loss/errors')
    if 'vcpu_states' not in sched or 'runnable_details' not in sched:
        ap.error('need scheduler --all-states --runnable-details')
    log = args.engine_log.read_text()
    if '[gles-ahb-path] requested=on active=on' not in log:
        ap.error('AHardwareBuffer stage markers were not enabled')
    lines = args.events.read_text().splitlines()
    if not lines or not lines[0].startswith('# LT_FRAME_PATH 1 '):
        ap.error('missing frame-path header')
    footer = re.fullmatch(r'# end records=(\d+) dropped=(\d+) incomplete=(\d+) end-ns=(\d+)',
                          lines[-1])
    if not footer or int(footer[2]) or int(footer[3]) or len(lines)-2 != int(footer[1]):
        ap.error('missing completion, frame-path loss/incomplete or count mismatch')
    completion = (f'[frame-path] finished records={footer[1]} dropped=0 '
                  'incomplete=0 write-error=no')
    if completion not in log:
        ap.error('frame-path engine completion mismatch or write error')
    names = {1: 'glFinish', 2: 'AHardwareBuffer_describe',
             3: 'AHardwareBuffer_lock', 4: 'AHardwareBuffer_unlock'}
    spans, presents, pending, present_pending = [], [], None, {}
    for line in lines[1:-1]:
        fields = line.split()
        if len(fields) != 5 or not all(fields[i].isdigit() for i in (0, 2, 3, 4)):
            ap.error('invalid frame-path record')
        ns, a, b, c = (int(fields[i]) for i in (0, 2, 3, 4))
        kind = fields[1]
        if kind == 'present-begin':
            if (a, b) in present_pending:
                ap.error('overlapping surface presents')
            present_pending[(a, b)] = ns
        elif kind == 'present-end':
            start = present_pending.pop((a, b), None)
            if start is None or start > ns:
                ap.error('surface present end without valid begin')
            presents.append(dict(begin_ns=start, end_ns=ns, context=a, target=b))
        elif kind == 'ahb-stage-begin':
            if pending is not None or b not in names or c != 0:
                ap.error('overlapping/invalid AHardwareBuffer stage')
            pending = dict(begin_ns=ns, buffer=a, stage=b)
        elif kind == 'ahb-stage-end':
            if (pending is None or pending['buffer'] != a or pending['stage'] != b
                    or pending['begin_ns'] > ns):
                ap.error('AHardwareBuffer stage end without valid begin')
            spans.append(dict(pending, end_ns=ns, operation=names[b],
                              result=c if c < 2**31 else c-2**32))
            pending = None
    if pending is not None or present_pending or not spans:
        ap.error('incomplete/absent AHardwareBuffer stages or surface presents')
    spans.sort(key=lambda s: s['begin_ns'])
    if any(a['end_ns'] > b['begin_ns'] for a, b in zip(spans, spans[1:])):
        ap.error('stages overlap on the selected vCPU')
    offset, window = sched['clock_offset_ns'], sched['window']
    states = sched['vcpu_states']
    starts = [int(s['ts'])-offset for s in states]

    def state_ms(begin, end):
        total = Counter()
        index = max(0, bisect.bisect_right(starts, begin)-1)
        while index < len(states) and starts[index] < end:
            row = states[index]
            a, b = max(begin, starts[index]), min(end, int(row['end_ns'])-offset)
            if b > a:
                total[row['state']] += (b-a)/1e6
            index += 1
        if abs(sum(total.values())-(end-begin)/1e6) > 0.001:
            ap.error('scheduler coverage missing within a stage')
        return dict(total)

    summary, selected = {}, []
    for span in spans:
        a = max(span['begin_ns'], window['begin_ns'])
        b = min(span['end_ns'], window['end_exclusive_ns'])
        if b <= a:
            continue
        span['in_window'] = dict(begin_ns=a, end_ns=b, state_ms=state_ms(a, b))
        selected.append(span)
        group = summary.setdefault(span['operation'], dict(calls=0, errors=0,
                                   wall_ms=0, max_wall_ms=0, state_ms=Counter()))
        group['calls'] += 1
        group['errors'] += span['result'] != 0
        group['wall_ms'] += (b-a)/1e6
        group['max_wall_ms'] = max(group['max_wall_ms'], (b-a)/1e6)
        group['state_ms'].update(span['in_window']['state_ms'])
    span_starts = [s['begin_ns'] for s in spans]
    matches = []
    for wake in sched['runnable_details']['waits']:
        a, b = wake['begin_ns']-offset, wake['end_ns']-offset
        index = bisect.bisect_right(span_starts, a)-1
        match = spans[index] if index >= 0 and spans[index]['end_ns'] >= b else None
        matches.append(dict(hitch_end_s=wake['hitch_end_s'], begin_ns=a, end_ns=b,
                            runnable_ms=(b-a)/1e6, stage=match,
                            surface_presents=[p for p in presents
                                              if p['begin_ns'] <= a and p['end_ns'] >= b]))
    hitches = []
    for hitch in sched['hitches']:
        detail = dict(begin_ns=hitch['begin_ns'], end_ns=hitch['end_ns'],
                      relative_end_s=hitch['relative_end_s'], ms=hitch['ms'], stages=[])
        for span in selected:
            a, b = max(span['begin_ns'], hitch['begin_ns']), min(span['end_ns'], hitch['end_ns'])
            if b > a:
                detail['stages'].append(dict(operation=span['operation'],
                                             begin_ns=a, end_ns=b, state_ms=state_ms(a, b)))
        hitches.append(detail)
    report = dict(engine_pid=args.engine_pid, vcpu=sched['vcpu'], window=window,
                  input_sha256={name: hashlib.sha256(path.read_bytes()).hexdigest()
                                for name, path in [('events', args.events),
                                                   ('engine_log', args.engine_log),
                                                   ('scheduler', args.scheduler)]},
                  summary=summary, stages=selected, hitches=hitches,
                  hitch_runnable_matches=matches)
    args.json.write_text(json.dumps(report, indent=2)+'\n')
    for operation, group in summary.items():
        print(operation, group)
    for match in matches:
        print(f"{match['hitch_end_s']:.3f}s runnable {match['runnable_ms']:.3f}ms:",
              match['stage']['operation'] if match['stage'] else 'outside marked stages')
    print('Stage wall times include scheduling; this is attribution, not an FPS comparison.')


if __name__ == '__main__':
    main()
