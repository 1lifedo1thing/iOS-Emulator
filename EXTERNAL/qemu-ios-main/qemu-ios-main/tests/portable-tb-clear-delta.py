#!/usr/bin/env python3
"""Exact clear schema, retained evidence and real-file publication tests."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

TOOL = Path(__file__).resolve().parents[1] / 'contrib/portable/tb-clear-delta.py'
spec = importlib.util.spec_from_file_location('clear_delta', TOOL)
clear = importlib.util.module_from_spec(spec)
spec.loader.exec_module(clear)
OWNER = {'host_boot_id': 'test-boot', 'pid': 123, 'start_ticks': '456',
         'engine_sha256': 'a' * 64, 'cpu': 0, 'cpu_epoch': 'verified-fresh-allocation-1'}


def line(seconds='10.0', count=100, **changes):
    counts = dict.fromkeys(clear.FIELDS, count)
    counts.update(changes)
    return ('[tb-clear] ' + seconds + 's cpu=0 entries=4096' +
            ''.join(' ' + k + '=' + str(v) for k, v in counts.items()) + '\n').encode()


def witness(data, row, begin, end):
    return {**row, 'identity': copy.deepcopy(OWNER),
            'file_identity': {'device': 1, 'inode': 2},
            'read_begin_monotonic_ns': begin, 'read_end_monotonic_ns': end,
            'eof_offset': len(data), 'tail_offset': 0, 'tail_sha256': clear.sha(data)}


def fixture():
    first = b'noise\n' + line()
    data = first + line('12.0', 107, full_total=101)
    parsed = clear.rows(data, 0)
    before = witness(first, parsed[0], 100, 110)
    after = witness(data, parsed[1], 180, 190)
    after['baseline'] = {k: copy.deepcopy(v) for k, v in before.items()
                         if k in ('identity', 'file_identity', 'eof_offset', 'tail_offset',
                                  'tail_sha256', 'read_begin_monotonic_ns', 'read_end_monotonic_ns')}
    after['baseline'].update(read_begin_monotonic_ns=160, read_end_monotonic_ns=170)
    after['first_complete_source_ns'] = 150
    return data, {'schema': 1, 'identity': copy.deepcopy(OWNER), 'label': 'synthetic-settings',
                  'log': {'path': '/private/not-live', 'sha256': clear.sha(data)},
                  'before': before, 'after': after,
                  'action': {'down_begin_monotonic_ns': 120, 'down_end_monotonic_ns': 125,
                             'last_transition_source_ns': 140, 'first_complete_source_ns': 150}}


class ClearDeltas(unittest.TestCase):
    def test_schema_matches_actual_production_dump_field_order(self):
        header = (TOOL.parents[2] / 'accel/tcg/tb-cache-clear-stats.h').read_text()
        definition = header.split('#define TB_CLEAR_FIELDS(X)', 1)[1].split('typedef enum', 1)[0]
        self.assertEqual(tuple(re.findall(r'X\((\w+)\)', definition)), clear.FIELDS)
        self.assertIn('"[tb-clear] %.1fs cpu=%u entries=%u"', header)

    def test_exact_fifteen_fields_and_nontransactional_totals(self):
        data, selection = fixture()
        saved = copy.deepcopy(selection)
        result = clear.compare(selection, data)
        self.assertEqual(len(result['delta']), 15)
        self.assertEqual(result['delta'], {k: 1 if k == 'full_total' else 7 for k in clear.FIELDS})
        self.assertEqual(result['counter_elapsed_rounding_bounds_seconds'], ['1.9', '2.1'])
        self.assertEqual(result['action_source_frame_latency_bracket_ns'], [15, 30])
        self.assertEqual(saved, selection)

    def test_exact_grammar_rejects_truncation_interleave_reorder_extra_field(self):
        first = line()
        variants = [first[:-1], first.replace(b' cpu=0', b' cpu=-1'),
                    first.replace(b'entries=4096', b'entries=16384'),
                    first.replace(b'10.0s', b'10.00s'), b'prefix ' + first,
                    first.replace(b'full_total=100 full_tlb=100', b'full_tlb=100 full_total=100'),
                    first.replace(b'full_total=100', b'full_total=100 bogus=1'),
                    first.replace(b'full_total=100', b'full_total=x'), first.replace(b'\n', b'\r\n')]
        for bad in variants:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                clear.rows(bad, 0)

    def test_u64_cpu_geometry_and_epoch_refusal(self):
        clear.rows(line(count=clear.MAX_U64), 0)
        for bad in [line(count=clear.MAX_U64 + 1), line().replace(b'cpu=0', b'cpu=4294967296'),
                    line() + line('11.0', 99), line() + line('10.0', 101),
                    line() + line('9.0', 101), line().replace(b'cpu=0', b'cpu=1')]:
            with self.assertRaises(ValueError):
                clear.rows(bad, 0)
        with self.assertRaises(ValueError):
            clear.rows(b'x' * (clear.MAX_LOG + 1), 0)

    def test_identity_and_each_counter_decrease_refused(self):
        for name in clear.FIELDS:
            with self.subTest(name=name), self.assertRaises(ValueError):
                clear.rows(line() + line('12.0', **{name: 99}), 0)
        for change in [{'pid': True}, {'cpu': True}, {'start_ticks': '-1'},
                       {'cpu_epoch': ''}, {'engine_sha256': 'x' * 64}, {'extra': 1}]:
            with self.subTest(change=change), self.assertRaises(ValueError):
                clear.identity({**OWNER, **change})

    def test_retained_hash_bytes_offset_owner_file_epoch_order_refused(self):
        changes = [lambda s: s['log'].update(sha256='0' * 64),
                   lambda s: s['before'].update(line_offset=1),
                   lambda s: s['before'].update(line_sha256='0' * 64),
                   lambda s: s['before'].update(tail_sha256='0' * 64),
                   lambda s: s['after']['identity'].update(cpu_epoch='another-cache'),
                   lambda s: s['after']['identity'].update(start_ticks='457'),
                   lambda s: s['after']['baseline']['identity'].update(pid=124),
                   lambda s: s['after']['file_identity'].update(inode=3),
                   lambda s: s['after']['baseline'].update(eof_offset=s['after']['line_offset'] + 1),
                   lambda s: s['after']['baseline'].update(tail_sha256='0' * 64),
                   lambda s: s['before'].update(read_end_monotonic_ns=121),
                   lambda s: s['after'].update(read_end_monotonic_ns=179),
                   lambda s: s['after']['baseline'].update(read_begin_monotonic_ns=149),
                   lambda s: s['after'].update(first_complete_source_ns=151),
                   lambda s: s['after']['counts'].update(full_total=999)]
        for change in changes:
            data, selection = fixture()
            change(selection)
            with self.subTest(change=change), self.assertRaises(ValueError):
                clear.compare(selection, data)

    def test_partial_live_tail_is_not_final_evidence(self):
        data = line() + line('11.0', 101)[:-5]
        self.assertEqual(len(clear.rows(data, 0, allow_partial=True)), 1)
        with self.assertRaises(ValueError):
            clear.rows(data, 0)

    def test_offline_requires_latest_before_first_new_after_and_exact_tail(self):
        data, selection = fixture()
        selection['before']['eof_offset'] = len(data)
        selection['before']['tail_sha256'] = clear.sha(data)
        with self.assertRaisesRegex(ValueError, 'latest'):
            clear.compare(selection, data)
        data, selection = fixture()
        data += line('13.0', 110)
        selection['log']['sha256'] = clear.sha(data)
        third = clear.rows(data, 0)[-1]
        selection['after'].update(witness(data, third, 180, 190))
        with self.assertRaisesRegex(ValueError, 'first eligible'):
            clear.compare(selection, data)
        data, selection = fixture()
        selection['before']['tail_offset'] = 1
        selection['before']['tail_sha256'] = clear.sha(data[1:selection['before']['eof_offset']])
        with self.assertRaisesRegex(ValueError, 'tail shape'):
            clear.compare(selection, data)

    def test_cli_actual_selection_and_retained_file(self):
        data, selection = fixture()
        with tempfile.TemporaryDirectory() as d:
            log, metadata = Path(d) / 'stderr.log', Path(d) / 'selection.json'
            log.write_bytes(data)
            selection['log']['path'] = str(log)
            metadata.write_text(json.dumps(selection))
            result = subprocess.run([sys.executable, str(TOOL), str(metadata)],
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['delta']['full_total'], 1)
            self.assertEqual(log.read_bytes(), data)


class FileWitnesses(unittest.TestCase):
    def open_log(self, initial):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'stderr.log'
        self.path.write_bytes(initial)
        fd = os.open(self.path, os.O_RDONLY)
        self.addCleanup(os.close, fd)
        return fd

    def append(self, data):
        with self.path.open('ab') as stream:
            stream.write(data)

    def test_real_file_before_latest_complete_ignores_partial(self):
        first, second = line(), line('11.0', 101)
        fd = self.open_log(first + second + line('12.0', 102)[:50])
        row = clear.observe(fd, OWNER, 'before', lambda: OWNER)
        self.assertEqual(row['line_offset'], len(first))
        self.assertEqual(row['line_ascii'], second.decode())

    def test_real_append_completing_old_partial_skipped_until_new_line_start(self):
        first, partial, fresh = line(), line('11.0', 101), line('12.0', 102)
        prefix = len(partial) // 2
        fd = self.open_log(first + partial[:prefix])
        written = []

        def append_on_poll(_seconds):
            chunk = partial[prefix:] if not written else fresh
            self.append(chunk)
            written.append(chunk)

        with patch.object(clear.time, 'sleep', side_effect=append_on_poll):
            row = clear.observe(fd, OWNER, 'after', lambda: OWNER,
                                first_complete_source_ns=time.monotonic_ns(), timeout=1)
        self.assertEqual(len(written), 2)
        self.assertEqual(row['line_offset'], len(first) + len(partial))
        self.assertGreater(row['line_offset'], row['baseline']['eof_offset'])
        self.assertEqual(row['line_ascii'], fresh.decode())

    def test_real_file_next_complete_at_exact_eof_accepted(self):
        fd = self.open_log(line())
        with patch.object(clear.time, 'sleep', side_effect=lambda _: self.append(line('11.0', 101))):
            row = clear.observe(fd, OWNER, 'after', lambda: OWNER,
                                first_complete_source_ns=time.monotonic_ns(), timeout=1)
        self.assertEqual(row['line_offset'], row['baseline']['eof_offset'])

    def test_actual_file_witnesses_roundtrip_retained_extraction(self):
        fd = self.open_log(line())
        before = clear.observe(fd, OWNER, 'before', lambda: OWNER)
        down_begin = time.monotonic_ns()
        down_end = time.monotonic_ns()
        transition, complete = time.monotonic_ns(), time.monotonic_ns()
        with patch.object(clear.time, 'sleep', side_effect=lambda _: self.append(line('11.0', 103))):
            after = clear.observe(fd, OWNER, 'after', lambda: OWNER,
                                  first_complete_source_ns=complete, timeout=1)
        data = self.path.read_bytes()
        selection = {'schema': 1, 'identity': OWNER, 'label': 'real-file-fixture',
                     'log': {'sha256': clear.sha(data)}, 'before': before, 'after': after,
                     'action': {'down_begin_monotonic_ns': down_begin,
                                'down_end_monotonic_ns': down_end,
                                'last_transition_source_ns': transition,
                                'first_complete_source_ns': complete}}
        self.assertEqual(clear.compare(selection, data)['delta'], dict.fromkeys(clear.FIELDS, 3))

    def test_real_file_timeout_no_new_record(self):
        fd = self.open_log(line())
        begin = time.monotonic()
        with self.assertRaises(TimeoutError):
            clear.observe(fd, OWNER, 'after', lambda: OWNER,
                          first_complete_source_ns=time.monotonic_ns(), timeout=.02)
        self.assertLess(time.monotonic() - begin, 1)

    def test_real_truncate_and_same_size_prefix_rewrite_refused(self):
        for change in [lambda: self.path.write_bytes(b''),
                       lambda: self.path.write_bytes(line(count=99))]:
            fd = self.open_log(line())
            with patch.object(clear.time, 'sleep', side_effect=lambda _: change()):
                with self.assertRaisesRegex(ValueError, 'truncated|prefix changed'):
                    clear.observe(fd, OWNER, 'after', lambda: OWNER,
                                  first_complete_source_ns=time.monotonic_ns(), timeout=1)

    def test_live_owner_and_epoch_guard_refused(self):
        fd = self.open_log(line())
        for change in [{'pid': 124}, {'start_ticks': '457'}, {'host_boot_id': 'other'},
                       {'cpu_epoch': 'recreated'}, {'engine_sha256': 'b' * 64}]:
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, 'owner/epoch'):
                clear.observe(fd, OWNER, 'before', lambda: {**OWNER, **change})

    def test_identity_change_during_read_refused(self):
        fd = self.open_log(line())
        calls = []

        def guard():
            calls.append(True)
            return OWNER if len(calls) == 1 else {**OWNER, 'cpu_epoch': 'recreated-during-read'}

        with self.assertRaisesRegex(ValueError, 'owner/epoch'):
            clear.observe(fd, OWNER, 'before', guard)
        self.assertEqual(len(calls), 2)

    def test_real_overbound_tail_and_future_target_refused(self):
        fd = self.open_log(line())
        with self.assertRaises(ValueError):
            clear.observe(fd, OWNER, 'after', lambda: OWNER,
                          first_complete_source_ns=time.monotonic_ns() + 10**10)
        with patch.object(clear.time, 'sleep', side_effect=lambda _: self.append(b'x' * (clear.MAX_TAIL + 1))):
            with self.assertRaisesRegex(ValueError, 'bounded tail'):
                clear.observe(fd, OWNER, 'after', lambda: OWNER,
                              first_complete_source_ns=time.monotonic_ns(), timeout=1)
        self.path.write_bytes(b'x' * (clear.MAX_LOG + 1))
        with self.assertRaisesRegex(ValueError, '16MiB'):
            clear.observe(fd, OWNER, 'before', lambda: OWNER)


if __name__ == '__main__':
    unittest.main()
