#!/usr/bin/env python3
"""Bounded offline deltas for retained experimental [tb-cache] records.

No process, QMP, guest, compiler or filesystem mutation. Boundary witnesses are
owner evidence, not an independently observed engine counter sampling clock.
"""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import re

MAX_LOG_BYTES = 16 * 1024 * 1024
MAX_U64 = (1 << 64) - 1
FIELDS = ('fast', 'empty', 'pc', 'state', 'slow_hit', 'slow_absent')
LINE = re.compile(rb'^\[tb-cache\]\s+(\d+\.\d)s entries=(4096|16384) '
                  rb'fast=(\d+) empty=(\d+) pc=(\d+) state=(\d+) '
                  rb'slow-hit=(\d+) slow-absent=(\d+)\s*$')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value):
    require(type(value) is int and 0 <= value <= MAX_U64, 'invalid unsigned integer')
    return value


def records(data):
    require(len(data) <= MAX_LOG_BYTES, 'log exceeds 16MiB bound')
    found, offset = {}, 0
    previous = None
    for line in data.splitlines(keepends=True):
        if b'[tb-cache]' in line:
            require(line.endswith(b'\n'), 'incomplete counter record')
            match = LINE.fullmatch(line)
            require(match is not None, 'malformed or interleaved counter record')
            seconds, entries, *counts = match.groups()
            row = {'offset': offset, 'elapsed_seconds': seconds.decode(),
                   'entries': int(entries), **dict(zip(FIELDS, map(int, counts)))}
            require(all(0 <= row[f] <= MAX_U64 for f in FIELDS), 'counter outside uint64')
            if previous:
                require(Decimal(row['elapsed_seconds']) > Decimal(previous['elapsed_seconds']),
                        'elapsed time repeated/decreased; mixed epochs or invalid cadence')
                require(row['entries'] == previous['entries'], 'cache geometry changed')
                require(all(row[f] >= previous[f] for f in FIELDS), 'counter reset/wrap/mixed epochs')
            found[offset] = row
            previous = row
        offset += len(line)
    require(found, 'no complete counter records')
    return found


def compare(selection, data):
    require(selection['schema'] == 1, 'unsupported selection schema')
    identity = selection['identity']
    require(isinstance(identity['host_boot_id'], str) and identity['host_boot_id'], 'host boot required')
    require(type(identity['pid']) is int and identity['pid'] > 0, 'worker PID required')
    require(isinstance(identity['start_ticks'], str) and
            re.fullmatch(r'\d+', identity['start_ticks']), 'worker start ticks required')
    require(isinstance(identity['engine_sha256'], str) and
            re.fullmatch(r'[a-f0-9]{64}', identity['engine_sha256']), 'engine hash required')
    require(isinstance(selection['label'], str) and selection['label'], 'phase label required')
    require(hashlib.sha256(data).hexdigest() == selection['log']['sha256'], 'retained log hash differs')
    rows = records(data)
    before, after, action = selection['before'], selection['after'], selection['action']
    for witness in [before, after]:
        for key in ['line_offset', 'read_begin_monotonic_ns', 'read_end_monotonic_ns']:
            integer(witness[key])
        require(witness['read_begin_monotonic_ns'] <= witness['read_end_monotonic_ns'],
                'reversed read bounds')
        require(witness['line_offset'] in rows, 'selected offset is not a counter line start')
    for key in ['baseline_eof_offset', 'baseline_read_begin_monotonic_ns', 'baseline_read_end_monotonic_ns']:
        integer(after[key])
    for key in ['down_begin_monotonic_ns', 'down_end_monotonic_ns',
                'last_transition_source_ns', 'first_complete_source_ns']:
        integer(action[key])
    require(before['line_offset'] < after['line_offset'], 'counter boundary order reversed')
    require(before['read_end_monotonic_ns'] <= action['down_begin_monotonic_ns'] <=
            action['down_end_monotonic_ns'] <= action['last_transition_source_ns'] <=
            action['first_complete_source_ns'], 'input/frame/before observation order differs')
    require(action['first_complete_source_ns'] <= after['baseline_read_begin_monotonic_ns'] <=
            after['baseline_read_end_monotonic_ns'] <= after['read_begin_monotonic_ns'],
            'after baseline must follow complete target and precede new record observation')
    require(after['baseline_eof_offset'] <= len(data) and
            after['baseline_eof_offset'] <= after['line_offset'],
            'after record already existed at baseline EOF or EOF exceeds final log')
    first, last = rows[before['line_offset']], rows[after['line_offset']]
    delta = {f: last[f] - first[f] for f in FIELDS}
    misses = sum(delta[f] for f in ['empty', 'pc', 'state'])
    slow = delta['slow_hit'] + delta['slow_absent']
    require(slow <= misses, 'slow results exceed admitted misses; source/counter epoch differs')
    attempts = delta['fast'] + misses
    elapsed = Decimal(last['elapsed_seconds']) - Decimal(first['elapsed_seconds'])
    return {'schema': 1, 'identity': identity, 'label': selection['label'],
            'log_sha256': selection['log']['sha256'], 'before': first, 'after': last,
            'delta': delta, 'lookup_attempts': attempts, 'lookup_misses': misses,
            'misses_without_recorded_slow_result': misses - slow,
            'miss_reason_fraction': {f: delta[f] / misses if misses else None
                                     for f in ['empty', 'pc', 'state']},
            'fast_hit_fraction_of_lookup_calls': delta['fast'] / attempts if attempts else None,
            'counter_elapsed_rounded_difference_seconds': str(elapsed),
            'counter_elapsed_rounding_bounds_seconds': [str(max(Decimal(0), elapsed - Decimal('.1'))),
                                                        str(elapsed + Decimal('.1'))],
            'action_source_frame_latency_bracket_ns': [
                action['last_transition_source_ns'] - action['down_end_monotonic_ns'],
                action['first_complete_source_ns'] - action['down_begin_monotonic_ns']],
            'limits': [
                'Owner-supplied identity/scene/read witnesses are not independently live-verified.',
                'New appended line proves publication after EOF, not counter evaluation after target.',
                'Nominal 1Hz dumps may be delayed by vCPU execution or stderr lock; no fixed 1s host error bound.',
                'Deltas describe a wider observation window, not exact action-only events or CPU time.',
                'Empty misses include startup/invalidation; PC misses are slot conflicts, not all guest branches.',
                'State combines cs_base/flags/cflags; no ASID/physical-mapping cause inferred.',
                'Fault/longjmp before QHT return can leave a miss without a slow result.',
                'Direct chained TBs bypass these lookups; this is not a guest instruction/global TB hit ratio.',
                'Instrumentation/accounting perturbs execution; no speedup or latency ranking from counts.',
            ]}


def bounded_read(path, maximum):
    with path.open('rb') as stream:
        data = stream.read(maximum + 1)
    require(len(data) <= maximum, 'input exceeds byte bound')
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('selection', type=Path)
    args = parser.parse_args()
    metadata = bounded_read(args.selection, 1024 * 1024)
    selection = json.loads(metadata)
    data = bounded_read(Path(selection['log']['path']), MAX_LOG_BYTES)
    result = compare(selection, data)
    result['selection_sha256'] = hashlib.sha256(metadata).hexdigest()
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
