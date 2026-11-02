#!/usr/bin/env python3
"""Follow physical surface backing from successful presents to n72 LCD flips.

Requires LT_GLES_SURFACE_PATH=on and a complete LT_FRAME_PATH trace. Matching
backing is an observation, not pixel-generation identity or a proven dependency.
Other guest/CPU/DMA writers are not traced. No phone access.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import statistics

from framestats import analyse, parse


UNKNOWN = (1 << 64) - 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('events', type=Path)
    ap.add_argument('frames', type=Path)
    ap.add_argument('--lcd-address-mode', required=True, choices=['n72-physical'],
                    help='explicitly confirm n72 LCD registers use physical addresses')
    ap.add_argument('--secs', type=float, default=60)
    ap.add_argument('--hitch-ms', type=float, default=30)
    ap.add_argument('--scheduler', type=Path, help='matching hitchsched --all-states JSON')
    ap.add_argument('--engine-pid', type=int)
    ap.add_argument('--json', required=True, type=Path)
    args = ap.parse_args()
    frames = parse(args.frames.read_text())
    if args.secs <= 0 or args.hitch_ms <= 0 or len(frames) < 2:
        ap.error('need multiple frames and positive duration/threshold')
    if any(y[0] <= x[0] or y[1] <= x[1] for x, y in zip(frames, frames[1:])):
        ap.error('frame timestamps and sequences must strictly increase')
    begin, end = frames[0][0], frames[0][0] + int(args.secs * 1e9)
    frames = [f for f in frames if f[0] < end]
    if len(frames) < 2:
        ap.error('need multiple frames in selected window')
    lines = args.events.read_text().splitlines()
    header = re.fullmatch(r'# LT_FRAME_PATH 1 capacity=(\d+) begin-ns=(\d+)', lines[0]) if lines else None
    footer = re.fullmatch(r'# end records=(\d+) dropped=(\d+) incomplete=(\d+) end-ns=(\d+)', lines[-1]) if lines else None
    if (not header or not footer or int(footer[1]) != len(lines) - 2 or
            int(footer[2]) or int(footer[3]) or int(footer[1]) > int(header[1]) or
            int(header[2]) > begin or int(footer[4]) < end):
        ap.error('need a complete lossless trace spanning the frame window')
    events = []
    for line in lines[1:-1]:
        fields = line.split()
        if len(fields) != 5 or not all(fields[i].isdigit() for i in (0, 2, 3, 4)):
            ap.error('invalid frame-path record')
        ns, a, b, c = (int(fields[i]) for i in (0, 2, 3, 4))
        if not int(header[2]) <= ns <= int(footer[4]) or max(a, b, c) > UNKNOWN:
            ap.error('invalid timestamp or event payload')
        events.append(dict(ns=ns, kind=fields[1], a=a, b=b, c=c))
    events.sort(key=lambda e: e['ns'])

    # Relevant events are serial on the vCPU; consume each mapping exactly once.
    calls, current, phase = [], None, 'surface-map-begin'
    cycle = ['surface-map-begin', 'surface-map-end', 'present-begin', 'present-end']
    ticks, tick = [], None
    publications = {}
    for e in events:
        kind, ns, a, b, c = (e[k] for k in ('kind', 'ns', 'a', 'b', 'c'))
        if kind in cycle:
            if kind != phase:
                ap.error(f'unpaired/interleaved mapping/present: expected {phase}, got {kind}')
            if kind == cycle[0]:
                current = dict(id=len(calls), context=a, virtual_base=b, span_bytes=c,
                               map_begin_ns=ns)
                if c > 16 * 1024 * 1024 or b > 0xffffffff:
                    ap.error('invalid bounded mapping metadata')
            else:
                if (a, b) != (current['context'], current['virtual_base']):
                    ap.error('mapping/present context or virtual base mismatch')
                if kind == cycle[1]:
                    current.update(map_end_ns=ns, physical_base=c if c != UNKNOWN else None)
                    if c != UNKNOWN and (not current['span_bytes'] or b + current['span_bytes'] > 1 << 32):
                        ap.error('known physical mapping with empty span')
                elif kind == cycle[2]:
                    current.update(begin_ns=ns, width=c >> 32, height=c & 0xffffffff)
                else:
                    current.update(end_ns=ns, result=c,
                                   present_ms=(ns - current['begin_ns']) / 1e6,
                                   mapping_ms=(current['map_end_ns'] - current['map_begin_ns']) / 1e6)
                    calls.append(current)
                    current = None
            phase = cycle[(cycle.index(kind) + 1) % len(cycle)]
        elif kind == 'vsync-begin':
            if tick is not None:
                ap.error('overlapping vsync calls')
            tick = dict(begin_ns=ns, previous_base=a, pending_base=b, lateness_ns=c)
        elif kind == 'vsync-end':
            if tick is None:
                ap.error('vsync end without begin')
            tick.update(end_ns=ns, latched_base=a, held=a == tick['previous_base'])
            ticks.append(tick)
            tick = None
        elif kind == 'publish':
            if a in publications:
                ap.error('reused publication sequence')
            publications[a] = e
    if current is not None or tick is not None or not calls:
        ap.error('incomplete mapping/present/vsync calls or no surface mappings')
    for ns, seq, *_ in frames:
        if seq not in publications or publications[seq]['ns'] > ns:
            ap.error('missing or late engine publication marker')

    states, state_times = [], []
    if args.scheduler:
        sched = json.loads(args.scheduler.read_text())
        if (args.engine_pid is None or sched['engine_pid'] != args.engine_pid or
                sched.get('trace_errors') or not sched.get('vcpu_states') or
                (sched['window']['begin_ns'], sched['window']['end_exclusive_ns']) != (begin, end)):
            ap.error('need matching worker/window and complete whole-window scheduler states')
        states = sched['vcpu_states']
        state_times = [s['ts'] - sched['clock_offset_ns'] for s in states]

    def host_states(left, right):
        if not states:
            return None
        # Calls may precede the selected scheduler window. Do not extrapolate.
        lo, hi = max(begin, left), min(end, right)
        totals, cursor = Counter(), lo
        i = max(0, bisect.bisect_right(state_times, lo) - 1)
        while i < len(states) and state_times[i] < hi:
            s = states[i]
            a, b = max(lo, state_times[i]), min(hi, s['end_ns'] - sched['clock_offset_ns'])
            if b > a:
                if a != cursor:
                    ap.error('scheduler gap/overlap within selected interval')
                totals[s['state']] += (b - a) / 1e6
                cursor = b
            i += 1
        if hi > lo and cursor != hi:
            ap.error('incomplete scheduler coverage')
        return dict(covered_ms=max(0, hi - lo) / 1e6,
                    outside_window_ms=(right - left) / 1e6 - max(0, hi - lo) / 1e6,
                    ms=dict(totals))

    known = defaultdict(list)
    for call in calls:
        if call['result'] == 0 and call['physical_base'] is not None:
            known[call['physical_base']].append(call)
    completion_times = {pa: [c['end_ns'] for c in rows] for pa, rows in known.items()}
    tick_times = [t['begin_ns'] for t in ticks]
    raw_flips = [e for e in events if e['kind'] == 'lcd-flip']
    last_use, flips = {}, []
    for i, e in enumerate(raw_flips):
        # LCD_FLIP.a is OLD; b is NEW. This join is only valid for n72 physical mode.
        ns, pa = e['ns'], e['b']
        previous = raw_flips[i - 1]['ns'] if i else None
        prior_use = last_use.get(pa)
        rows, times = known.get(pa, []), completion_times.get(pa, [])
        first = bisect.bisect_right(times, prior_use) if prior_use is not None else 0
        stop = bisect.bisect_right(times, ns)
        candidates = rows[first:stop]
        call = candidates[-1] if candidates else None
        status = ('prehistory-unknown' if prior_use is None else
                  'no-new-successful-write' if not candidates else
                  'write-crossed-previous-selection' if call['begin_ns'] <= prior_use else
                  'fresh-multiple-writes' if len(candidates) > 1 else 'fresh-single-write')
        lo = bisect.bisect_right(tick_times, previous) if previous is not None else 0
        hi = bisect.bisect_right(tick_times, ns)
        held = [dict(t) for t in ticks[lo:hi] if t['held']]
        fresh = status.startswith('fresh-')
        for t in held:
            t['write_completed_before_tick'] = call['end_ns'] < t['begin_ns'] if fresh else None
            t['write_end_minus_tick_ms'] = (call['end_ns'] - t['begin_ns']) / 1e6 if fresh else None
        next_flip_ns = raw_flips[i + 1]['ns'] if i + 1 < len(raw_flips) else int(footer[4])
        next_ticks = ticks[hi:bisect.bisect_left(tick_times, next_flip_ns)]
        latched = next((t for t in next_ticks if t['latched_base'] == pa), None)
        flip = dict(id=i, ns=ns, old_physical_base=e['a'], new_physical_base=pa,
                    previous_flip_ns=previous, previous_selection_of_buffer_ns=prior_use,
                    cycle_ms=(ns - previous) / 1e6 if previous else None,
                    match_status=status, candidate_call_ids=[c['id'] for c in candidates],
                    last_matching_call_id=call['id'] if call else None,
                    write_end_to_flip_ms=(ns - call['end_ns']) / 1e6 if call else None,
                    held_refreshes=held, next_latch=latched,
                    write_end_to_flip_states=host_states(call['end_ns'], ns) if call else None,
                    cycle_states=host_states(previous, ns) if previous is not None else None)
        flips.append(flip)
        last_use[pa] = ns

    flip_times = [f['ns'] for f in flips]
    gaps = []
    for first, last in zip(frames, frames[1:]):
        if (last[0] - first[0]) / 1e6 <= args.hitch_ms:
            continue
        a, b = publications[first[1]]['ns'], publications[last[1]]['ns']
        selected = flips[bisect.bisect_right(flip_times, a):bisect.bisect_right(flip_times, b)]
        # Retain the preceding flip cycle and write even when it crosses a frame boundary.
        gaps.append(dict(begin_ns=first[0], end_ns=last[0], ms=(last[0] - first[0]) / 1e6,
                         relative_end_s=(last[0] - begin) / 1e9,
                         skipped=max(0, last[1] - first[1] - 1),
                         engine_begin_ns=a, engine_end_ns=b, flip_ids=[f['id'] for f in selected],
                         frontend_end_lag_ms=(last[0] - b) / 1e6,
                         host_states=host_states(first[0], last[0])))
    chosen_flips = [f for f in flips if begin <= f['ns'] < end]
    chosen_calls = [c for c in calls if begin <= c['begin_ns'] < end]
    mapping_times = [c['mapping_ms'] for c in chosen_calls]
    holds = [t for f in chosen_flips for t in f['held_refreshes']]
    summary = dict(calls=len(chosen_calls), successful=sum(c['result'] == 0 for c in chosen_calls),
                   known_successful=sum(c['result'] == 0 and c['physical_base'] is not None for c in chosen_calls),
                   flips=len(chosen_flips), match_status=dict(Counter(f['match_status'] for f in chosen_flips)),
                   held_refresh_write_status=dict(Counter('completed-before-tick' if t['write_completed_before_tick'] is True else
                       'completed-after-or-at-tick' if t['write_completed_before_tick'] is False else 'unknown' for t in holds)),
                   mapping_total_ms=sum(mapping_times),
                   mapping_median_ms=statistics.median(mapping_times) if mapping_times else None,
                   mapping_max_ms=max(mapping_times, default=None))
    report = dict(window=dict(begin_ns=begin, end_exclusive_ns=end), lcd_address_mode=args.lcd_address_mode,
                  inputs={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                          [args.events, args.frames] + ([args.scheduler] if args.scheduler else [])},
                  frames={k: v for k, v in analyse(frames, secs=args.secs).items() if k in
                          ('mean', 'ceiling', 'median', 'low', 'low1', 'hitches', 'hitch_ms', 'skipped')},
                  summary=summary, calls=calls, flips=flips, gaps=gaps,
                  limits=['Physical backing is not pixel-generation identity; other writers are untraced.',
                          'Only successful complete contiguous mappings are joined.',
                          'Fresh means a completed observed write after the previous selection of that buffer.',
                          'Multiple writes, prehistory and unknown mappings remain explicit.',
                          'A completed-before-tick association does not prove that frame should have been displayed.'])
    args.json.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(frames=report['frames'], summary=summary), indent=2))
    for gap in sorted(gaps, key=lambda g: -g['ms'])[:15]:
        print(f"gap ending {gap['relative_end_s']:.3f}s: {gap['ms']:.3f}ms, flip IDs {gap['flip_ids']}")
        for ident in gap['flip_ids']:
            f = flips[ident]
            print(f"  {f['match_status']}, write→flip {f['write_end_to_flip_ms']}ms, "
                  f"held refresh write status {[t['write_completed_before_tick'] for t in f['held_refreshes']]}")


if __name__ == '__main__':
    main()
