import importlib.util
import pathlib
import tempfile
import unittest

path = pathlib.Path(__file__).resolve().parents[1] / 'android_input_priority.py'
spec = importlib.util.spec_from_file_location('input_priority', path)
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)
NAME = 'action-00000000-0000-0000-0000-000000000001.json'


class InputPriorityTests(unittest.TestCase):
    def test_request_already_queued_runs_before_expensive_capture(self):
        with tempfile.TemporaryDirectory() as d:
            run = pathlib.Path(d)
            (run / NAME).write_text('{}')
            calls = []
            self.assertTrue(policy.service_pending(run, set(), lambda p: calls.append('input'),
                                                  lambda p, result: calls.append('receipt')))
            calls.append('capture')
            self.assertEqual(calls, ['input', 'receipt', 'capture'])

    def test_request_arriving_during_blocking_read_abandons_remaining_capture(self):
        with tempfile.TemporaryDirectory() as d:
            run = pathlib.Path(d)
            clock = [0]
            events = []
            seen = set()
            # One unavoidable blocking read lasts two seconds. The request
            # arrives while it is in flight; a checkpoint must precede all
            # remaining expensive snapshot/button/status operations.
            def blocking_read():
                clock[0] += 2
                (run / NAME).write_text('{}')
                events.append('read-finished')
            def capture():
                blocking_read()
                if policy.service_pending(run, seen,
                        lambda p: events.append(('input-at', clock[0])),
                        lambda p, result: events.append('invalidate')):
                    return None
                clock[0] += 10
                events.append('old-capture-published')
            self.assertIsNone(capture())
            self.assertEqual(events, ['read-finished', ('input-at', 2), 'invalidate'])
            self.assertEqual(clock[0], 2)
            self.assertFalse(policy.service_pending(run, seen, lambda p: self.fail('replayed'),
                                                   lambda p, result: None))

    def test_completion_receipts_never_dispatch_or_consume_command_limit(self):
        with tempfile.TemporaryDirectory() as d:
            run = pathlib.Path(d)
            (run / NAME).write_text('{}')
            for name in ['action-completed-latest.json', 'action-dispatch-latest.json', 'action-foreign.json']:
                (run / name).write_text('{}')
            seen = set()
            policy.service_pending(run, seen, lambda p: 1, lambda p, value: None, maximum=1)
            self.assertEqual(seen, {NAME})

    def test_failed_or_unknown_dispatch_is_not_retried_and_has_no_success_receipt(self):
        with tempfile.TemporaryDirectory() as d:
            run = pathlib.Path(d)
            (run / NAME).write_text('{}')
            seen = set()
            def unknown(path):
                raise TimeoutError('remote input completion unknown')
            with self.assertRaises(TimeoutError):
                policy.service_pending(run, seen, unknown,
                                       lambda *args: self.fail('false completion'))
            self.assertFalse(policy.service_pending(run, seen, lambda p: self.fail('retried'),
                                                   lambda *args: None))

    def test_bound_is_checked_before_thirteenth_input(self):
        with tempfile.TemporaryDirectory() as d:
            run = pathlib.Path(d)
            for n in range(13):
                (run / f'action-00000000-0000-0000-0000-{n:012x}.json').write_text('{}')
            dispatched = []
            with self.assertRaises(ValueError):
                policy.service_pending(run, set(), lambda p: dispatched.append(p.name),
                                       lambda *args: None)
            self.assertEqual(len(dispatched), 12)


if __name__ == '__main__':
    unittest.main()
