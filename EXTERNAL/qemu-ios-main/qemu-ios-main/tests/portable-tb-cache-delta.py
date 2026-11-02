#!/usr/bin/env python3
"""Synthetic retained-log refusal/delta checks, not a guest counter run."""
import copy
import hashlib
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('cache_delta', Path(__file__).resolve().parents[1] /
                                             'contrib/portable/tb-cache-delta.py')
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)

FIRST = b'[tb-cache] 10.0s entries=4096 fast=100 empty=5 pc=10 state=20 slow-hit=30 slow-absent=3\n'
SECOND = b'[tb-cache] 12.0s entries=4096 fast=200 empty=15 pc=30 state=50 slow-hit=70 slow-absent=20\n'
LOG = b'unrelated\n' + FIRST + b'unrelated\n' + SECOND


def selection(data=LOG):
    return {'schema': 1, 'identity': {'host_boot_id': 'synthetic-boot', 'pid': 123,
            'start_ticks': '456', 'engine_sha256': 'a' * 64}, 'label': 'synthetic-action',
            'log': {'path': '/private/retained.log', 'sha256': hashlib.sha256(data).hexdigest()},
            'before': {'line_offset': LOG.index(FIRST), 'read_begin_monotonic_ns': 100,
                       'read_end_monotonic_ns': 110},
            'after': {'line_offset': LOG.index(SECOND), 'baseline_eof_offset': LOG.index(SECOND),
                      'baseline_read_begin_monotonic_ns': 160, 'baseline_read_end_monotonic_ns': 170,
                      'read_begin_monotonic_ns': 180, 'read_end_monotonic_ns': 190},
            'action': {'down_begin_monotonic_ns': 120, 'down_end_monotonic_ns': 125,
                       'last_transition_source_ns': 140, 'first_complete_source_ns': 150}}


class CacheDeltas(unittest.TestCase):
    def test_cumulative_reasons_and_fault_gap(self):
        source = selection()
        saved = copy.deepcopy(source)
        result = cache.compare(source, LOG)
        self.assertEqual(result['delta'], {'fast': 100, 'empty': 10, 'pc': 20,
                         'state': 30, 'slow_hit': 40, 'slow_absent': 17})
        self.assertEqual(result['lookup_attempts'], 160)
        self.assertEqual(result['misses_without_recorded_slow_result'], 3)
        self.assertEqual(result['miss_reason_fraction']['state'], .5)
        self.assertEqual(result['counter_elapsed_rounding_bounds_seconds'], ['1.9', '2.1'])
        self.assertEqual(result['action_source_frame_latency_bracket_ns'], [15, 30])
        self.assertEqual(source, saved)

    def test_no_activity_has_no_fabricated_ratio(self):
        data = FIRST + FIRST.replace(b'10.0s', b'11.0s')
        source = selection(data)
        source['before']['line_offset'] = 0
        source['after']['line_offset'] = source['after']['baseline_eof_offset'] = len(FIRST)
        result = cache.compare(source, data)
        self.assertIsNone(result['fast_hit_fraction_of_lookup_calls'])
        self.assertEqual(result['miss_reason_fraction'], {'empty': None, 'pc': None, 'state': None})

    def test_reset_geometry_and_elapsed_refused(self):
        for old, new in [(b'fast=200', b'fast=99'), (b'entries=4096', b'entries=16384'),
                         (b'12.0s', b'10.0s')]:
            with self.assertRaises(ValueError):
                cache.records(FIRST + SECOND.replace(old, new))

    def test_truncated_malformed_and_overflow_refused(self):
        for bad in [FIRST.rstrip(b'\n'), FIRST.replace(b'fast=100', b'fast=x'),
                    FIRST.replace(b'fast=100', b'fast=18446744073709551616'),
                    b'other [tb-cache] interleaved\n']:
            with self.assertRaises(ValueError):
                cache.records(bad)
        with self.assertRaisesRegex(ValueError, '16MiB'):
            cache.records(b'x' * (cache.MAX_LOG_BYTES + 1))

    def test_hash_offset_and_boundary_order_refused(self):
        mutations = [lambda x: x['log'].update(sha256='0' * 64),
                     lambda x: x['before'].update(line_offset=1),
                     lambda x: x['before'].update(read_end_monotonic_ns=121),
                     lambda x: x['after'].update(baseline_read_begin_monotonic_ns=149),
                     lambda x: x['after'].update(baseline_eof_offset=len(LOG)),
                     lambda x: x['after'].update(read_begin_monotonic_ns=169),
                     lambda x: x['identity'].update(pid=True)]
        for mutation in mutations:
            source = selection()
            mutation(source)
            with self.assertRaises(ValueError):
                cache.compare(source, LOG)

    def test_impossible_slow_partition_refused(self):
        data = LOG.replace(b'slow-hit=70', b'slow-hit=100')
        with self.assertRaisesRegex(ValueError, 'slow results exceed'):
            cache.compare(selection(data), data)


if __name__ == '__main__':
    unittest.main()
