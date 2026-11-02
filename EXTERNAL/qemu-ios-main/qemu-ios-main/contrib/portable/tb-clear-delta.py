#!/usr/bin/env python3
"""Read-only publication witnesses and offline experimental clear-count deltas.

Identity/CPU-lifetime and scene witnesses are caller evidence, not facts inferred
from counter values. No process control, guest commands, compiler or log writes.
"""
import argparse
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import time

MAX_LOG = 16 * 1024 * 1024
MAX_TAIL = 1024 * 1024
MAX_U64 = (1 << 64) - 1
FIELDS = ('full_total', 'full_tlb', 'tlb_clean', 'tlb_partial', 'tlb_all',
          'full_range', 'full_tb', 'full_pcrel', 'full_reset', 'page_total',
          'page_previous', 'page_current', 'page_range', 'tb_check', 'tb_match')
LINE = re.compile(rb'\[tb-clear\] ([0-9]+\.[0-9])s cpu=([0-9]+) entries=(4096)' +
                  b''.join(b' ' + name.encode() + rb'=([0-9]+)' for name in FIELDS) + b'\n')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def integer(value, maximum=MAX_U64):
    require(type(value) is int and 0 <= value <= maximum, 'invalid unsigned integer')
    return value


def identity(value):
    require(isinstance(value, dict), 'identity required')
    require(set(value) == {'host_boot_id', 'pid', 'start_ticks', 'engine_sha256',
                           'cpu', 'cpu_epoch'}, 'identity fields differ')
    for name in ('host_boot_id', 'cpu_epoch'):
        require(isinstance(value[name], str) and 0 < len(value[name]) <= 256,
                'owner/CPU lifetime certificate required')
    require(integer(value['pid']) > 0, 'owner PID required')
    require(isinstance(value['start_ticks'], str) and
            re.fullmatch(r'[0-9]{1,20}', value['start_ticks']) and
            int(value['start_ticks']) <= MAX_U64, 'owner start ticks required')
    require(isinstance(value['engine_sha256'], str) and
            re.fullmatch(r'[a-f0-9]{64}', value['engine_sha256']), 'engine hash required')
    integer(value['cpu'], (1 << 32) - 1)
    return value


def rows(data, cpu, offset=0, allow_partial=False):
    """Parse exact complete records; a bounded live tail may end mid-record."""
    require(len(data) <= MAX_LOG, 'log exceeds 16MiB bound')
    integer(cpu, (1 << 32) - 1)
    integer(offset)
    if offset:
        end = data.find(b'\n')
        require(end >= 0, 'tail has no complete line boundary')
        offset += end + 1
        data = data[end + 1:]
    found = []
    for line in data.splitlines(keepends=True):
        if b'[tb-clear]' in line:
            if not line.endswith(b'\n') and allow_partial:
                break
            match = LINE.fullmatch(line)
            require(match is not None, 'incomplete/malformed/interleaved clear record')
            elapsed, owner, entries, *values = match.groups()
            require(int(owner) == cpu and int(owner) < (1 << 32), 'counter CPU differs')
            counts = dict(zip(FIELDS, map(int, values)))
            require(all(v <= MAX_U64 for v in counts.values()), 'counter outside uint64')
            found.append({'line_offset': offset, 'line_ascii': line.decode('ascii'),
                          'line_sha256': sha(line), 'elapsed_seconds': elapsed.decode(),
                          'cpu': int(owner), 'entries': int(entries), 'counts': counts})
        offset += len(line)
    for before, after in zip(found, found[1:]):
        require(Decimal(after['elapsed_seconds']) > Decimal(before['elapsed_seconds']),
                'elapsed repeated/decreased; counter epoch ambiguous')
        require(all(after['counts'][f] >= before['counts'][f] for f in FIELDS),
                'counter decreased/reset/wrapped; epoch ambiguous')
    return found


def snapshot(fd, expected, guard):
    """guard must revalidate live PID/start/boot/engine/CPU lifetime externally."""
    require(identity(guard()) == expected, 'live owner/epoch differs')
    begin = time.monotonic_ns()
    st = os.fstat(fd)
    require(stat.S_ISREG(st.st_mode), 'log must be regular file')
    require(0 <= st.st_size <= MAX_LOG, 'log exceeds 16MiB bound')
    offset = max(0, st.st_size - MAX_TAIL)
    data = os.pread(fd, st.st_size - offset, offset)
    require(len(data) == st.st_size - offset, 'log truncated during read')
    end = time.monotonic_ns()
    require(identity(guard()) == expected, 'live owner/epoch differs')
    return {'identity': dict(expected), 'file_identity': {'device': st.st_dev, 'inode': st.st_ino},
            'read_begin_monotonic_ns': begin, 'read_end_monotonic_ns': end,
            'eof_offset': st.st_size, 'tail_offset': offset, 'tail_sha256': sha(data)}, data


def observe(fd, expected, mode, guard, first_complete_source_ns=None, timeout=5):
    """Return BEFORE latest record or AFTER a newly started complete record.

    A line already partially present at the AFTER baseline EOF is ineligible.
    Polling is bounded; the caller must supply a bounded guard and supervise
    filesystem syscalls if a hard aggregate deadline is required.
    """
    expected = dict(identity(expected))
    require(mode in ('before', 'after'), 'invalid boundary mode')
    require(type(timeout) in (int, float) and 0 < timeout <= 5, 'invalid timeout')
    baseline, data = snapshot(fd, expected, guard)
    parsed = rows(data, expected['cpu'], baseline['tail_offset'], allow_partial=True)
    if mode == 'before':
        require(parsed, 'no complete before record')
        return {**baseline, **parsed[-1]}
    require(integer(first_complete_source_ns) <= baseline['read_begin_monotonic_ns'],
            'after baseline precedes full target source')
    deadline = time.monotonic() + timeout
    previous_size = baseline['eof_offset']
    while time.monotonic() < deadline:
        current, data = snapshot(fd, expected, guard)
        require(current['file_identity'] == baseline['file_identity'], 'log inode changed')
        require(current['eof_offset'] >= previous_size, 'log truncated/reset')
        require(current['eof_offset'] - baseline['eof_offset'] <= MAX_TAIL,
                'new log bytes exceed bounded tail')
        # An earlier witnessed prefix must remain intact even if final size grew.
        prefix = os.pread(fd, baseline['eof_offset'] - baseline['tail_offset'],
                          baseline['tail_offset'])
        require(sha(prefix) == baseline['tail_sha256'], 'witnessed log prefix changed')
        previous_size = current['eof_offset']
        parsed = rows(data, expected['cpu'], current['tail_offset'], allow_partial=True)
        eligible = [x for x in parsed if x['line_offset'] >= baseline['eof_offset']]
        if eligible:
            return {**current, **eligible[0], 'baseline': baseline,
                    'first_complete_source_ns': first_complete_source_ns}
        time.sleep(min(.01, max(0, deadline - time.monotonic())))
    raise TimeoutError('no new complete clear record within bounded interval')


def validate_witness(witness, expected, data):
    require(identity(witness['identity']) == expected, 'witness owner/epoch differs')
    for key in ('line_offset', 'read_begin_monotonic_ns', 'read_end_monotonic_ns',
                'eof_offset', 'tail_offset'):
        integer(witness[key])
    require(witness['read_begin_monotonic_ns'] <= witness['read_end_monotonic_ns'],
            'reversed read bounds')
    start, end = witness['line_offset'], witness['eof_offset']
    require(0 <= witness['tail_offset'] <= start < end <= len(data), 'invalid witness offsets')
    require(witness['tail_offset'] == max(0, end - MAX_TAIL), 'witness tail shape differs')
    require(sha(data[witness['tail_offset']:end]) == witness['tail_sha256'],
            'retained witness tail differs')
    require(isinstance(witness['file_identity'], dict) and
            set(witness['file_identity']) == {'device', 'inode'}, 'file identity required')
    for value in witness['file_identity'].values():
        integer(value)
    candidates = rows(data[:end], expected['cpu'], allow_partial=True)
    selected = [x for x in candidates if x['line_offset'] == start]
    require(len(selected) == 1, 'offset is not a complete record start')
    row = selected[0]
    for name in ('line_ascii', 'line_sha256', 'elapsed_seconds', 'cpu', 'entries', 'counts'):
        require(witness[name] == row[name], 'selected record evidence differs')
    return row


def compare(selection, data):
    require(selection['schema'] == 1, 'unsupported schema')
    expected = identity(selection['identity'])
    require(isinstance(selection['label'], str) and selection['label'], 'phase label required')
    require(sha(data) == selection['log']['sha256'], 'retained log hash differs')
    parsed = rows(data, expected['cpu'])
    require(parsed, 'no complete clear records')
    before, after, action = selection['before'], selection['after'], selection['action']
    first = validate_witness(before, expected, data)
    last = validate_witness(after, expected, data)
    require(rows(data[:before['eof_offset']], expected['cpu'], allow_partial=True)[-1] == first,
            'before record is not latest complete publication')
    baseline = after['baseline']
    require(identity(baseline['identity']) == expected, 'baseline owner/epoch differs')
    require(before['file_identity'] == after['file_identity'] == baseline['file_identity'],
            'log file identity differs')
    for key in ('read_begin_monotonic_ns', 'read_end_monotonic_ns', 'eof_offset', 'tail_offset'):
        integer(baseline[key])
    require(0 <= baseline['eof_offset'] <= len(data) and
            baseline['tail_offset'] == max(0, baseline['eof_offset'] - MAX_TAIL),
            'invalid baseline EOF/tail')
    require(sha(data[baseline['tail_offset']:baseline['eof_offset']]) == baseline['tail_sha256'],
            'baseline prefix differs')
    require(first['line_offset'] < last['line_offset'] and
            before['eof_offset'] <= baseline['eof_offset'] <= last['line_offset'],
            'record existed at baseline EOF or boundary order differs')
    eligible = [x for x in parsed if baseline['eof_offset'] <= x['line_offset'] and
                x['line_offset'] + len(x['line_ascii']) <= after['eof_offset']]
    require(eligible and eligible[0] == last, 'after record is not first eligible publication')
    for name in ('down_begin_monotonic_ns', 'down_end_monotonic_ns',
                 'last_transition_source_ns', 'first_complete_source_ns'):
        integer(action[name])
    require(before['read_end_monotonic_ns'] <= action['down_begin_monotonic_ns'] <=
            action['down_end_monotonic_ns'] <= action['last_transition_source_ns'] <=
            action['first_complete_source_ns'] == after['first_complete_source_ns'] <=
            baseline['read_begin_monotonic_ns'] <= baseline['read_end_monotonic_ns'] <=
            after['read_begin_monotonic_ns'], 'input/source/publication order differs')
    delta = {f: last['counts'][f] - first['counts'][f] for f in FIELDS}
    elapsed = Decimal(last['elapsed_seconds']) - Decimal(first['elapsed_seconds'])
    return {'schema': 1, 'identity': expected, 'label': selection['label'],
            'log_sha256': sha(data), 'before': first, 'after': last, 'delta': delta,
            'counter_elapsed_rounded_difference_seconds': str(elapsed),
            'counter_elapsed_rounding_bounds_seconds': [str(max(Decimal(0), elapsed - Decimal('.1'))),
                                                        str(elapsed + Decimal('.1'))],
            'action_source_frame_latency_bracket_ns': [
                action['last_transition_source_ns'] - action['down_end_monotonic_ns'],
                action['first_complete_source_ns'] - action['down_begin_monotonic_ns']],
            'limits': [
                'Caller-supplied owner/CPU-lifetime and scene attestations are not independently verified.',
                'New line publication after EOF does not prove counter evaluation after full target.',
                'Owner-loop dumps may be delayed; no fixed 1Hz host-time error bound.',
                'Independent atomic field reads are not a coherent transaction; no sum equality required.',
                'Deltas count wider-window clear operations, not unique evictions, misses or CPU time.',
                'No ARM CP15 origin, exact launch attribution or performance win follows.',
                'Instrumentation/accounting/layout cost precludes production timing ranking.',
            ]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('selection', type=Path)
    args = parser.parse_args()
    with args.selection.open('rb') as stream:
        metadata = stream.read(MAX_TAIL + 1)
    require(len(metadata) <= MAX_TAIL, 'selection exceeds 1MiB')
    selection = json.loads(metadata)
    with Path(selection['log']['path']).open('rb') as stream:
        data = stream.read(MAX_LOG + 1)
    result = compare(selection, data)
    result['selection_sha256'] = sha(metadata)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
