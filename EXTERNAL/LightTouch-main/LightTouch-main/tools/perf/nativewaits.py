#!/usr/bin/env python3
"""Match completed native waits to a validated vCPU scheduler capture.

Requires LT_NATIVE_WAIT_PROF=on or conditions with LT_FRAME_PATH, a clean Stop, the matching
engine log and hitchsched.py --all-states --runnable-details output. This is
offline attribution, not an optimization comparison or a complete stack trace.
With conditions mode, LT_BQL_FALLBACK_PROF=on can additionally record blocking
BQL acquisitions after the spin budget, preserving the BQL spin hook itself.
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
    finished = re.findall(r'\[native-wait\] finished tid=(\d+) calls=(\d+) '
                          r'retained=(\d+) site-overflow=(\d+) hooks=(\w+)', log)
    if len(finished) != 1:
        ap.error('missing/duplicate native-wait completion')
    tid, count, retained, overflow, hooks = finished[0]
    if int(tid) != int(sched['vcpu']['tid']) or int(overflow) or hooks != 'unchanged':
        ap.error('native wait TID mismatch, site overflow or replaced hooks')
    sites = {}
    pattern = (r'\[native-wait-site\] id=(\d+) kind=(\d+) object=(0x[0-9a-f]+) '
               r'mutex=(0x[0-9a-f]+) file=(\S+) line=(\d+)')
    kinds = {1: 'BQL mutex', 2: 'other QEMU mutex', 3: 'condition', 4: 'timed condition'}
    for site, kind, obj, mutex, file, line in re.findall(pattern, log):
        site, kind = int(site), int(kind)
        if site in sites or kind not in kinds:
            ap.error('duplicate/unknown native wait site')
        sites[site] = dict(kind=kinds[kind], object=obj, mutex=mutex,
                           file=file, line=int(line))
    lines = args.events.read_text().splitlines()
    if not lines or not lines[0].startswith('# LT_FRAME_PATH 1 '):
        ap.error('missing frame-path header')
    footer = re.fullmatch(r'# end records=(\d+) dropped=(\d+) incomplete=(\d+) end-ns=(\d+)',
                          lines[-1])
    if not footer or int(footer[2]) or int(footer[3]) or len(lines)-2 != int(footer[1]):
        ap.error('missing completion, frame-path loss/incomplete or count mismatch')
    waits, presents, pending = [], [], {}
    for line in lines[1:-1]:
        fields = line.split()
        if len(fields) != 5 or not all(fields[i].isdigit() for i in (0, 2, 3, 4)):
            ap.error('invalid frame-path record')
        marker, a, b = (int(fields[i]) for i in (0, 2, 3))
        if fields[1] == 'present-begin':
            if (a, b) in pending:
                ap.error('overlapping surface presents')
            pending[(a, b)] = marker
        elif fields[1] == 'present-end':
            begin = pending.pop((a, b), None)
            if begin is None or begin > marker:
                ap.error('surface present end without valid begin')
            presents.append(dict(begin_ns=begin, end_ns=marker, context=a, target=b))
        if fields[1] != 'native-wait':
            continue
        marker, begin, end, site = (int(fields[i]) for i in (0, 2, 3, 4))
        if site not in sites or not begin < end <= marker or end-begin < 10000:
            ap.error('invalid native wait interval/site')
        waits.append(dict(begin_ns=begin, end_ns=end, site=site, marker_ns=marker))
    if pending:
        ap.error('incomplete surface present')
    waits.sort(key=lambda w: w['begin_ns'])
    if len(waits) != int(retained) or int(count) < int(retained):
        ap.error('native-wait completion count mismatch')
    if any(a['end_ns'] > b['begin_ns'] for a, b in zip(waits, waits[1:])):
        ap.error('native waits overlap on the selected vCPU')
    offset = sched['clock_offset_ns']
    window = sched['window']
    states = sched['vcpu_states']
    starts = [s['ts']-offset for s in states]

    def state_ms(begin, end):
        total = Counter()
        index = max(0, bisect.bisect_right(starts, begin)-1)
        while index < len(states) and starts[index] < end:
            row = states[index]
            a, b = max(begin, starts[index]), min(end, row['end_ns']-offset)
            if b > a:
                total[row['state']] += (b-a)/1e6
            index += 1
        if abs(sum(total.values())-(end-begin)/1e6) > 0.001:
            ap.error('scheduler coverage missing within a native wait')
        return dict(total)

    summary = {}
    for wait in waits:
        a = max(wait['begin_ns'], window['begin_ns'])
        b = min(wait['end_ns'], window['end_exclusive_ns'])
        if b <= a:
            continue
        wait['in_window'] = dict(begin_ns=a, end_ns=b, state_ms=state_ms(a, b))
        group = summary.setdefault(wait['site'], dict(calls=0, wall_ms=0, state_ms=Counter()))
        group['calls'] += 1
        group['wall_ms'] += (b-a)/1e6
        group['state_ms'].update(wait['in_window']['state_ms'])
    wake_matches = []
    wait_starts = [w['begin_ns'] for w in waits]
    for wake in sched['runnable_details']['waits']:
        a, b = wake['begin_ns']-offset, wake['end_ns']-offset
        index = bisect.bisect_right(wait_starts, a)-1
        match = waits[index] if index >= 0 and waits[index]['end_ns'] >= b else None
        wake_matches.append(dict(hitch_end_s=wake['hitch_end_s'], begin_ns=a, end_ns=b,
                                  runnable_ms=(b-a)/1e6, scheduler_state=wake['state']['state'],
                                  wait=match, site=sites[match['site']] if match else None,
                                  surface_presents=[p for p in presents
                                                    if p['begin_ns'] <= a and p['end_ns'] >= b]))
    report = dict(engine_pid=args.engine_pid, vcpu=sched['vcpu'], window=window,
                  input_sha256={name: hashlib.sha256(path.read_bytes()).hexdigest()
                                for name, path in [('events', args.events),
                                                   ('engine_log', args.engine_log),
                                                   ('scheduler', args.scheduler)]},
                  total_completed_calls=int(count), retained_calls=int(retained),
                  threshold_ns=10000, sites=sites, in_window_by_site=summary,
                  waits=waits, hitch_runnable_matches=wake_matches)
    args.json.write_text(json.dumps(report, indent=2)+'\n')
    print(f'Validated {len(waits)} completed waits >=10us, TID {tid}; no buffer/site loss.')
    for site, group in sorted(summary.items(), key=lambda pair: -pair[1]['wall_ms']):
        print(sites[site], group)
    for match in wake_matches:
        print(f"{match['hitch_end_s']:.3f}s runnable {match['runnable_ms']:.3f}ms:",
              match['site'], 'enclosing presents:', len(match['surface_presents']))
    print('Durations include scheduling; condition waits include mutex reacquisition.')
    print('Unmatched runnable intervals and direct library/driver waits remain unattributed.')
    print('An enclosing surface present does not identify the exact driver wait function.')


if __name__ == '__main__':
    main()
