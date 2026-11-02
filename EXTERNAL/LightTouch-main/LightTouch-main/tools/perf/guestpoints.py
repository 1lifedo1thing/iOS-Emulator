#!/usr/bin/env python3
"""Decode selected guest PC snapshots and align them with published frames.

Uses LT_GUEST_TRACE_PC and a completed LT_FRAME_PATH trace. A snapshot means
execution reached that instruction boundary, before its condition/execution.
Function arguments and scheduling semantics require matching disassembly.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re

from framestats import parse
from guestsymbols import GuestSymbols


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('events', type=Path)
    ap.add_argument('frames', type=Path)
    ap.add_argument('--engine-pid', type=int, required=True)
    ap.add_argument('--guest-processes', type=Path, required=True)
    ap.add_argument('--guest-kernel', type=Path)
    ap.add_argument('--guest-cache', type=Path)
    ap.add_argument('--secs', type=float, default=60)
    ap.add_argument('--json', type=Path)
    args = ap.parse_args()
    if args.secs <= 0 or bool(args.guest_kernel) != bool(args.guest_cache):
        ap.error('need positive duration and both symbol inputs together')
    snapshot = json.loads(args.guest_processes.read_text())
    if snapshot.get('engine_pid') != args.engine_pid or not snapshot.get('detached'):
        ap.error('need this engine worker and confirmed debugger detach')
    bits = snapshot.get('ttbr0_alignment_bits')
    if not isinstance(bits, int) or not 7 <= bits <= 14:
        ap.error('invalid snapshot TTBR0 alignment')
    mask = ~((1 << bits) - 1)
    processes = {}
    for process in snapshot['processes']:
        table = process['ttbr0'] & mask
        if table in processes:
            ap.error('ambiguous process page table')
        processes[table] = process
    symbols = GuestSymbols(args.guest_kernel, args.guest_cache) if args.guest_kernel else None
    if symbols and snapshot.get('kernel_sha256') != symbols.hashes['kernel']:
        ap.error('snapshot and symbol kernel hashes differ')
    frames = parse(args.frames.read_text())
    if len(frames) < 2 or any(b[0] <= a[0] or b[1] <= a[1]
                              for a, b in zip(frames, frames[1:])):
        ap.error('need strictly increasing frames and sequences')
    begin, end = frames[0][0], frames[0][0] + int(args.secs * 1e9)
    frames = [f for f in frames if f[0] < end]
    intervals = [dict(begin_ns=a[0], end_ns=b[0], category=(
        'skipped' if b[1] > a[1] + 1 else 'hitch' if b[0] - a[0] > 50_000_000
        else 'late' if b[0] - a[0] > 20_000_000 else 'normal'))
        for a, b in zip(frames, frames[1:])]
    if not intervals:
        ap.error('no complete frame intervals in selected window')
    starts = [f['begin_ns'] for f in intervals]
    lines = args.events.read_text().splitlines()
    if not lines or not lines[0].startswith('# LT_FRAME_PATH 1 '):
        ap.error('unsupported/missing trace header')
    footer = re.fullmatch(r'# end records=(\d+) dropped=(\d+) incomplete=(\d+) end-ns=(\d+)', lines[-1])
    if not footer or int(footer[2]) or int(footer[3]) or int(footer[1]) != len(lines) - 2:
        ap.error('missing completion footer, dropped/incomplete records or wrong count')
    pending, rows, changes, previous = None, [], [], 0
    for line in lines[1:-1]:
        fields = line.split()
        if len(fields) != 5 or not all(fields[i].isdigit() for i in (0, 2, 3, 4)):
            ap.error('invalid frame-path record')
        ns, a, b, c = (int(fields[i]) for i in (0, 2, 3, 4))
        kind = fields[1]
        if kind in ('context-switch', 'ttbr0'):
            changes.append(dict(ns=ns, cidr=a, ttbr0=b))
        elif kind == 'guest-pc':
            if pending or a != previous + 1 or (rows and ns < rows[-1]['end_ns']):
                ap.error('overlapping, missing or reused PC snapshot sequence')
            previous = a
            pending = dict(id=a, begin_ns=ns, pc=b >> 32, thread=b & 0xffffffff,
                           cidr=c >> 32, ttbr0=c & 0xffffffff)
        elif kind in ('guest-regs', 'guest-stack'):
            if pending is None or pending['id'] != a or ns < pending['begin_ns']:
                ap.error('register record without matching preceding PC snapshot')
            if kind == 'guest-regs':
                if 'r0' in pending:
                    ap.error('duplicate register record')
                pending.update(r0=b >> 32, r1=b & 0xffffffff,
                               r2=c >> 32, r3=c & 0xffffffff, regs_ns=ns)
            else:
                if 'r0' not in pending or ns < pending['regs_ns']:
                    ap.error('missing/late argument register record')
                pending.update(lr=b >> 32, sp=b & 0xffffffff,
                               r12=c >> 32, mode=c & 0xffffffff, end_ns=ns)
                rows.append(pending)
                pending = None
    if pending or not rows:
        ap.error('incomplete snapshots or no selected guest PCs observed')
    changes.sort(key=lambda r: r['ns'])
    times = [r['ns'] for r in changes]
    selected, outside = [], []
    for row in rows:
        ns = row['begin_ns']
        row['symbol'] = symbols.lookup(row['pc'] & ~1) if symbols else None
        row['process'] = processes.get(row['ttbr0'] & mask)
        i = bisect.bisect_right(starts, ns) - 1
        if i < 0 or ns >= intervals[i]['end_ns']:
            outside.append(row)
            continue
        ci = bisect.bisect_right(times, ns) - 1
        if ci < 0 or any(row[k] != changes[ci][k] for k in ('cidr', 'ttbr0')):
            ap.error('PC snapshot differs from installed context timeline')
        row.update(frame_index=i, category=intervals[i]['category'])
        selected.append(row)
    if not selected:
        ap.error('no selected PC observations in frame window')
    groups = defaultdict(list)
    for row in selected:
        groups[row['pc']].append(row)
    summary = []
    for pc, group in sorted(groups.items(), key=lambda item: -len(item[1])):
        value = dict(pc=pc, symbol=group[0]['symbol'], observations=len(group),
                     categories=dict(Counter(r['category'] for r in group)),
                     threads=dict(Counter(hex(r['thread']) for r in group)))
        summary.append(value)
        print(json.dumps(value))
    print(f'{len(selected)} window snapshots, {len(outside)} outside; no incomplete triplets.')
    print('Reached-PC observations; infer arguments/wait states only from matching code.')
    if args.json:
        paths = dict(events=args.events, frames=args.frames, processes=args.guest_processes)
        args.json.write_text(json.dumps(dict(
            window=dict(begin_ns=begin, end_exclusive_ns=end), engine_pid=args.engine_pid,
            input_sha256={k: hashlib.sha256(p.read_bytes()).hexdigest() for k, p in paths.items()},
            symbol_sha256=symbols.hashes if symbols else None, summary=summary,
            frames=intervals, samples=selected, outside_window=outside), indent=2) + '\n')


if __name__ == '__main__':
    main()
