#!/usr/bin/env python3
"""Synthetic offline parser/epoch checks, not an engine or performance test."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('jit', Path(__file__).resolve().parents[1] /
                                             'contrib/portable/jit-snapshot-delta.py')
jit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(jit)


def snapshot(label='before', begin=100, code=1000, count=10, flush=0):
    return {'schema': 1, 'label': label,
            'identity': {'host_boot_id': 'synthetic-boot', 'pid': 123,
                         'start_ticks': '456', 'engine_sha256': 'a' * 64},
            'begin_monotonic_ns': begin, 'end_monotonic_ns': begin + 10,
            'response': {'return': {'human-readable-text': f'''Accelerator settings:
one-insn-per-tb: off

Translation buffer state:
gen code size       {code}/100000
TB count            {count}
TB avg target size  20 max=40 bytes

Statistics:
TB flush count      {flush}
TB invalidate count 2
TLB full flushes    3
TLB partial flushes 4
TLB elided flushes  5
'''}}}


class JitSnapshots(unittest.TestCase):
    def test_retained_growth_and_observation_bounds(self):
        before, after = snapshot(), snapshot('after', 200, 1500, 15)
        saved = copy.deepcopy((before, after))
        result = jit.compare(before, after)
        self.assertEqual(result['delta']['generated_code_bytes'], 500)
        self.assertEqual(result['delta']['live_tb_count'], 5)
        self.assertEqual(result['delta']['tb_flush_count'], 0)
        self.assertEqual(result['observation_gap_bounds_ns'], [90, 110])
        self.assertEqual((before, after), saved)

    def test_flush_allows_negative_net_retained_state(self):
        result = jit.compare(snapshot(), snapshot('after', 200, 100, 1, 1))
        self.assertEqual(result['delta']['generated_code_bytes'], -900)
        self.assertEqual(result['delta']['live_tb_count'], -9)
        self.assertEqual(result['delta']['tb_flush_count'], 1)

    def test_epoch_clock_and_counter_refusals(self):
        for field, value in [('pid', 124), ('start_ticks', '457'),
                             ('host_boot_id', 'other-boot'), ('engine_sha256', 'b' * 64)]:
            after = snapshot('after', 200)
            after['identity'][field] = value
            with self.assertRaisesRegex(ValueError, 'different worker'):
                jit.compare(snapshot(), after)
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            jit.compare(snapshot(), snapshot('after', 105))
        with self.assertRaisesRegex(ValueError, 'counter decreased'):
            jit.compare(snapshot(flush=1), snapshot('after', 200, flush=0))

    def test_missing_duplicate_error_and_capacity_refusals(self):
        for replacement in ['TB count            missing',
                            'TB count            10\nTB count            11']:
            item = snapshot()
            text = item['response']['return']['human-readable-text']
            item['response']['return']['human-readable-text'] = text.replace('TB count            10', replacement)
            with self.assertRaisesRegex(ValueError, 'missing or duplicate'):
                jit.parse(item)
        error = snapshot()
        error['response'] = {'error': {'class': 'GenericError', 'desc': 'not available'}}
        with self.assertRaisesRegex(ValueError, 'returned an error'):
            jit.parse(error)
        after = snapshot('after', 200)
        text = after['response']['return']['human-readable-text']
        after['response']['return']['human-readable-text'] = text.replace('/100000', '/200000')
        with self.assertRaisesRegex(ValueError, 'capacity changed'):
            jit.compare(snapshot(), after)


if __name__ == '__main__':
    unittest.main()
