#!/usr/bin/env python3
"""Compare two retained x-query-jit snapshots offline; never contact an engine."""
import argparse
import hashlib
import json
from pathlib import Path
import re


PATTERNS = {
    'generated_code_bytes': r'gen code size\s+(\d+)/(\d+)',
    'live_tb_count': r'TB count\s+(\d+)',
    'tb_flush_count': r'TB flush count\s+(\d+)',
    'tb_invalidate_count': r'TB invalidate count\s+(\d+)',
    'tlb_full_flushes': r'TLB full flushes\s+(\d+)',
    'tlb_partial_flushes': r'TLB partial flushes\s+(\d+)',
    'tlb_elided_flushes': r'TLB elided flushes\s+(\d+)',
}
CUMULATIVE = tuple(k for k in PATTERNS if k not in ('generated_code_bytes', 'live_tb_count'))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def parse(snapshot):
    require(snapshot.get('schema') == 1, 'unsupported snapshot schema')
    identity = snapshot['identity']
    require(isinstance(identity.get('host_boot_id'), str) and identity['host_boot_id'],
            'host boot identity required')
    require(type(identity.get('pid')) is int and identity['pid'] > 0, 'worker PID required')
    require(isinstance(identity.get('start_ticks'), str) and
            re.fullmatch(r'[0-9]+', identity['start_ticks']), 'worker start ticks required')
    require(isinstance(identity.get('engine_sha256'), str) and
            re.fullmatch(r'[a-f0-9]{64}', identity['engine_sha256']), 'engine hash required')
    require(isinstance(snapshot.get('label'), str) and snapshot['label'], 'phase label required')
    begin, end = snapshot['begin_monotonic_ns'], snapshot['end_monotonic_ns']
    require(type(begin) is int and type(end) is int and 0 <= begin <= end,
            'invalid monotonic observation bounds')
    response = snapshot['response']
    require('error' not in response, 'QMP command returned an error')
    text = response['return']['human-readable-text']
    require(isinstance(text, str), 'QMP human-readable text required')
    values = {}
    for name, pattern in PATTERNS.items():
        matches = re.findall(r'^' + pattern + r'\s*$', text, re.MULTILINE)
        require(len(matches) == 1, 'missing or duplicate JIT field: ' + name)
        if name == 'generated_code_bytes':
            values[name], values['code_capacity_bytes'] = map(int, matches[0])
        else:
            values[name] = int(matches[0])
    require(0 <= values['generated_code_bytes'] <= values['code_capacity_bytes'] and
            values['code_capacity_bytes'] > 0, 'invalid generated code/capacity bounds')
    return values


def compare(before, after):
    first, last = parse(before), parse(after)
    require(before['identity'] == after['identity'], 'different worker/engine epochs')
    require(before['end_monotonic_ns'] <= after['begin_monotonic_ns'],
            'reversed or overlapping snapshot bounds')
    require(first['code_capacity_bytes'] == last['code_capacity_bytes'],
            'code buffer capacity changed within worker epoch')
    for name in CUMULATIVE:
        require(last[name] >= first[name], 'counter decreased (reset/wrap/epoch): ' + name)
    return {
        'schema': 1,
        'identity': before['identity'],
        'labels': [before['label'], after['label']],
        'observation_gap_bounds_ns': [after['begin_monotonic_ns'] - before['end_monotonic_ns'],
                                      after['end_monotonic_ns'] - before['begin_monotonic_ns']],
        'query_duration_ns': [before['end_monotonic_ns'] - before['begin_monotonic_ns'],
                              after['end_monotonic_ns'] - after['begin_monotonic_ns']],
        'before': first,
        'after': last,
        'delta': {name: last[name] - first[name] for name in PATTERNS},
        'limits': [
            'Snapshots read multiple non-atomic statistics and walk TB/QHT metadata.',
            'Code bytes and live TB deltas are net retained state, not total translations.',
            'Flush/invalidation can hide generated work; counters can eventually wrap.',
            'No translation CPU time, MMIO time, disk I/O or graphics initialization attribution.',
            'Capacity is virtual code-buffer space, not resident RAM.',
            'Identity assertions are retained owner evidence, not independently live-verified here.',
        ],
    }


def load(path):
    with path.open('rb') as stream:
        data = stream.read(1024 * 1024 + 1)
    require(len(data) <= 1024 * 1024, 'snapshot exceeds 1MiB bound')
    return json.loads(data), hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    before, before_hash = load(args.before)
    after, after_hash = load(args.after)
    result = compare(before, after)
    result['inputs'] = [{'path': str(args.before), 'sha256': before_hash},
                        {'path': str(args.after), 'sha256': after_hash}]
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
