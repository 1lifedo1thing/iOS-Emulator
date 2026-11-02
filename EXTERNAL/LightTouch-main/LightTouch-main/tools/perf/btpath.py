#!/usr/bin/env python3
"""Decode deferred LT_BT_PATH commands, reply queues and UART injections.

No phone access. Injection into a UART is not proof that guest userspace read
the reply. Reply bytes beyond the retained eight-byte prefix stay unknown.
"""
import argparse
from collections import Counter, deque
import hashlib
import json
from pathlib import Path
import re
from statistics import median


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('events', type=Path)
    ap.add_argument('--json', type=Path, required=True)
    args = ap.parse_args()
    raw = args.events.read_bytes()
    lines = raw.decode().splitlines()
    if not lines or not lines[0].startswith('# LT_FRAME_PATH 1 '):
        ap.error('missing frame-path header')
    footer = re.fullmatch(
        r'# end records=(\d+) dropped=(\d+) incomplete=(\d+) end-ns=(\d+)', lines[-1])
    if (not footer or int(footer[2]) or int(footer[3]) or
            int(footer[1]) != len(lines) - 2):
        ap.error('need a complete, lossless deferred trace')
    rows = []
    for index, line in enumerate(lines[1:-1]):
        fields = line.split()
        if len(fields) != 5 or not all(fields[i].isdigit() for i in (0, 2, 3, 4)):
            ap.error('invalid frame-path record')
        ns, a, b, c = (int(fields[i]) for i in (0, 2, 3, 4))
        if fields[1].startswith('bt-'):
            rows.append(dict(ns=ns, kind=fields[1], a=a, b=b, c=c, index=index))
    # These producers run under the BQL. Original record order breaks ties.
    rows.sort(key=lambda r: (r['ns'], r['index']))
    if not rows:
        ap.error('no Bluetooth path events')
    pending = deque()
    commands, replies, states = [], [], []

    def prefix(value, count):
        if value >= 1 << (8 * min(count, 8)):
            ap.error('reply prefix extends past its recorded length')
        return [(value >> (8 * i)) & 255 for i in range(min(count, 8))]

    for row in rows:
        ns, kind, a, b, c = (row[k] for k in ('ns', 'kind', 'a', 'b', 'c'))
        if kind == 'bt-command':
            if a > 0xffff or b > 255:
                ap.error('invalid HCI command metadata')
            commands.append(dict(id=len(commands), ns=ns, opcode=a,
                                 parameter_bytes=b, configured_latency_ns=c,
                                 reply_ids=[]))
        elif kind == 'bt-queue':
            if not 0 < a <= 1024 or c != len(pending) + a:
                ap.error('Bluetooth queue occupancy mismatch')
            known = prefix(b, a)
            reply = dict(id=len(replies), ns=ns, bytes=a, prefix=known,
                         command_id=None, first_injection_ns=None,
                         last_injection_ns=None, discarded_by_reset=False)
            # H4 Command Complete begins with 04 0e plen ncmd opcode status.
            if len(known) >= 7 and known[:2] == [4, 14]:
                if known[2] + 3 != a:
                    ap.error('Command Complete packet length mismatch')
                opcode = known[4] | known[5] << 8
                reply.update(opcode=opcode, status=known[6])
                matches = [cmd for cmd in commands if cmd['opcode'] == opcode
                           and not any('opcode' in replies[i] for i in cmd['reply_ids'])]
                if not matches:
                    ap.error('Command Complete without an observed command')
                cmd = matches[-1]
                reply['command_id'] = cmd['id']
                cmd['reply_ids'].append(reply['id'])
            elif a == 2 and all(48 <= x <= 57 for x in known):
                reply['launch_announcement'] = True
                if not commands or commands[-1]['opcode'] != 0xfc2e:
                    ap.error('launch announcement without download-minidriver command')
                reply['command_id'] = commands[-1]['id']
                commands[-1]['reply_ids'].append(reply['id'])
            replies.append(reply)
            pending.extend((reply['id'], i, known[i] if i < len(known) else None)
                           for i in range(a))
        elif kind == 'bt-flush':
            if c != len(pending) or not 0 < a <= c:
                ap.error('UART injection occupancy mismatch')
            known = prefix(b, a)
            for i in range(a):
                ident, offset, value = pending.popleft()
                if i < len(known) and value is not None and known[i] != value:
                    ap.error('injected prefix differs from queued reply')
                reply = replies[ident]
                if reply['first_injection_ns'] is None:
                    reply['first_injection_ns'] = ns
                if offset + 1 == reply['bytes']:
                    reply['last_injection_ns'] = ns
        elif kind == 'bt-state':
            states.append(row)
            if a == 0:  # machine reset, including any discarded partial data
                if c != len(pending):
                    ap.error('reset queue occupancy mismatch')
                for ident, _, _ in pending:
                    replies[ident]['discarded_by_reset'] = True
                pending.clear()
            elif a == 1:
                if b != len(pending):
                    ap.error('backpressure queue occupancy mismatch')
            elif a == 2:
                if c != len(pending):
                    ap.error('overflow queue occupancy mismatch')
            else:
                ap.error('unknown Bluetooth state action')
        else:
            ap.error('unknown Bluetooth path event')
    for reply in replies:
        first, last = reply['first_injection_ns'], reply['last_injection_ns']
        reply['queue_to_first_injection_ms'] = (
            (first - reply['ns']) / 1e6 if first is not None else None)
        reply['queue_to_last_injection_ms'] = (
            (last - reply['ns']) / 1e6 if last is not None else None)
    counts = Counter(row['kind'] for row in rows)
    opcodes = Counter(cmd['opcode'] for cmd in commands)
    delays = [r['queue_to_first_injection_ms'] for r in replies
              if r['queue_to_first_injection_ms'] is not None]
    summary = dict(
        events=dict(counts), commands=len(commands), reply_chunks=len(replies),
        resets=sum(row['a'] == 0 for row in states),
        backpressure=sum(row['a'] == 1 for row in states),
        overflows=sum(row['a'] == 2 for row in states), pending_bytes=len(pending),
        command_counts={hex(k): v for k, v in opcodes.most_common()},
        command_span_ms=((commands[-1]['ns'] - commands[0]['ns']) / 1e6
                         if commands else None),
        queue_to_first_injection_ms=dict(
            count=len(delays), median=median(delays) if delays else None,
            maximum=max(delays) if delays else None),
        commands_without_observed_reply=[cmd['id'] for cmd in commands
                                         if not cmd['reply_ids']],
        replies_without_complete_injection=[r['id'] for r in replies
                                            if r['last_injection_ns'] is None],
        nonzero_statuses=[r['id'] for r in replies if r.get('status', 0)],
        announcements=[r['id'] for r in replies if r.get('launch_announcement')])
    report = dict(
        input_sha256=hashlib.sha256(raw).hexdigest(), summary=summary,
        commands=commands, replies=replies, states=states,
        limits=[
            'UART injection does not prove that guest DMA or userspace consumed a reply.',
            'Only the first eight reply bytes are retained; remaining bytes are unknown.',
            'No guest command payload or firmware bytes are recorded.',
            'Controller events alone do not identify guest service failure or a frame cause.'])
    args.json.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
