# SPDX-License-Identifier: GPL-2.0-or-later
import subprocess
import sys
from pathlib import Path
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import memory_monitor as memory
import pixel_supervisor
import cow_plan


class Memory(unittest.TestCase):
    def setUp(self):
        self.host_init = mock.patch.object(pixel_supervisor.PixelAttempt, 'initialize_host_lifecycle')
        self.host_init.start()
        self.addCleanup(self.host_init.stop)

    def sampler(self, run=None, identity=lambda p: 123, now=lambda: 1):
        return memory.Sampler('/synthetic/adb', 99, 123, 10**9, run=run or mock.Mock(), now=now, identity=identity)

    def test_full_swap_does_not_invent_stall_or_swap_io(self):
        old = 'pswpin 100\npswpout 20\n'
        delta = memory.swap_delta(old, old, 4096)
        self.assertEqual(delta['bytes'], {'pswpin': 0, 'pswpout': 0})
        self.assertEqual(memory.swap_delta(old, 'pswpin 102\npswpout 21\n', 16384)['bytes']['pswpin'], 32768)

    def test_counter_reset_or_missing_is_unavailable(self):
        for text in ('pswpin 1\npswpout 10\n', 'pswpin 200\n'):
            self.assertFalse(memory.swap_delta('pswpin 100\npswpout 20\n', text, 4096)['available'])

    def test_denied_psi_is_null_and_does_not_mask_other_samples(self):
        def run(argv, **kw):
            denied = 'pressure/memory' in argv[-1]
            return subprocess.CompletedProcess(argv, 0, b'\n__LT_MEMORY_EXIT__=1\n' if denied else b'MemTotal: 123 kB\n\n__LT_MEMORY_EXIT__=0\n', b'Permission denied' if denied else b'')
        result = self.sampler(run).guest()
        self.assertFalse(result['files']['/proc/pressure/memory']['available'])
        self.assertIsNone(result['files']['/proc/pressure/memory']['text'])
        self.assertTrue(result['files']['/proc/meminfo']['available'])

    def test_reused_owner_or_original_deadline_never_dispatches_adb(self):
        for sampler in (self.sampler(identity=lambda p: 124), self.sampler(now=lambda: 10**9)):
            sampler.remote('/proc/meminfo')
            sampler.run.assert_not_called()

    def test_timeout_keeps_partial_without_available_value(self):
        run = mock.Mock(side_effect=subprocess.TimeoutExpired('synthetic', .75, output=b'partial'))
        receipt = self.sampler(run).remote('/proc/meminfo')
        self.assertFalse(receipt['available']); self.assertEqual(receipt['partial'], 'partial')
        self.assertLessEqual(run.call_args.kwargs['timeout'], .75)

    def test_remote_exit_required_even_if_adb_zero(self):
        for stdout in (b'123', b'123\n__LT_MEMORY_EXIT__=1\n', b'x'*131073 + b'\n__LT_MEMORY_EXIT__=0\n'):
            run = mock.Mock(return_value=subprocess.CompletedProcess([], 0, stdout, b''))
            self.assertFalse(self.sampler(run).remote('/proc/meminfo')['available'])

    def test_cleanup_disables_guest_commands_but_host_sample_remains_available(self):
        sampler = self.sampler(); sampler.disabled = lambda: True
        result = sampler.guest()
        sampler.run.assert_not_called()
        self.assertTrue(all(not x['available'] for x in result['files'].values()))
        self.assertFalse(memory.host_sample(lambda p: 'synthetic', [])['guestAcceptancePassed'])

    def test_wrong_boot_record_prevents_worker_pss_read(self):
        sampler = self.sampler()
        sampler.remote = mock.Mock(return_value={'available': True, 'text': 'wrong'})
        result = sampler.guest({'workerPid': 42, 'bootId': 'correct', 'recordId': 'correct'})
        self.assertFalse(result['worker']['available'])
        self.assertFalse(any('smaps_rollup' in c.args[0] for c in sampler.remote.call_args_list))

    def test_worker_identity_checks_uid_ticks_and_role(self):
        stat = '42 (worker) ' + ' '.join(['S'] + ['0']*18 + ['123'])
        b = {'workerStartTicks': 123, 'appUid': 10215}
        self.assertTrue(memory.worker_identity(stat, 'Uid:\t10215\t10215\n', 'org.lighttouch.android:engine\0', b))
        # Actual API36 /proc cmdline used a99-byte argv slot padded with NULs.
        padded = 'org.lighttouch.android:engine'.ljust(99, '\0')
        self.assertTrue(memory.worker_identity(stat, 'Uid:\t10215\t10215\n', padded, b))
        self.assertFalse(memory.worker_identity(stat, 'Uid:\t10215\t10215\n', 'org.lighttouch.android:engine\0replacement', b))
        for change in ({'workerStartTicks': 124}, {'appUid': 10216}):
            self.assertFalse(memory.worker_identity(stat, 'Uid:\t10215\n', 'org.lighttouch.android:engine\0', {**b, **change}))
        self.assertFalse(memory.worker_identity(stat, 'Uid:\t10215\n', 'org.lighttouch.android', b))

    def test_denied_host_file_and_zram_provenance_retained(self):
        def reader(path):
            if path == '/proc/pressure/io': raise PermissionError('denied')
            return 'synthetic zram' if path.startswith('/sys') else path
        result = memory.host_sample(reader, [Path('/sys/block/zram0')])
        self.assertFalse(result['files']['/proc/pressure/io']['available'])
        self.assertEqual(result['zram']['/sys/block/zram0']['comp_algorithm']['text'], 'synthetic zram')
        self.assertFalse(result['guestAcceptancePassed'])

    def test_supervisor_uses_actual_original_process_clock_even_after_cli_validation(self):
        import os,tempfile
        with tempfile.TemporaryDirectory() as d:
            args=type('Args',(),{'output':d+'/attempt','adb':'/synthetic/adb'})()
            with mock.patch.object(memory,'process_ticks',return_value=123456):
                attempt=pixel_supervisor.PixelAttempt(args)
            expected=(123456*1_000_000_000//os.sysconf('SC_CLK_TCK'))/1e9
            self.assertEqual(attempt.start,expected);self.assertEqual(attempt.deadline,expected+900)

    def test_pixel_8g_option_only_and_ambiguous_flag_rejected(self):
        self.assertEqual(pixel_supervisor.pixel_options(['--pixel-memory-mb', '8192', '--avd', 'fresh']), (8192, ['--avd', 'fresh']))
        with self.assertRaises(ValueError): pixel_supervisor.pixel_options(['--pixel-memory-mb', '8192', '--memory-mb', '4096'])
        with self.assertRaises(ValueError): pixel_supervisor.pixel_options(['--pixel-memory-mb', '8192', '--memory-mb=4096'])
        with self.assertRaises(SystemExit): pixel_supervisor.pixel_options(['--pixel-memory-mb', '16384'])

    def test_cow_plan_is_commands_only_and_preserves_parent_scope(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            source = Path(d) / 'preserved'; source.mkdir()
            for name in ('userdata-qemu.img.qcow2', 'encryptionkey.img.qcow2', 'cache.img.qcow2'):
                (source / name).write_text('synthetic')
            (source / 'config.ini').write_text('avd.name=old\nhw.ramSize=2G\nimage.sysdir.1=/synthetic/system\n')
            child = Path(d) / 'fresh'
            result = cow_plan.plan(source, child, 'pixel8g', '/sdk/qemu-img', '/sdk/emulator', 8192)
            self.assertFalse(child.exists()); self.assertFalse(result['executed'])
            self.assertEqual(result['createCommands'][0][-3:], ['-b', str(source / 'userdata-qemu.img.qcow2'), str(child / 'userdata-branch.img')])
            self.assertIn('hw.ramSize=8192', result['smallTextFiles']['config.ini'])
            compile(result['smallTextFiles']['launch-emulator.py'], '<synthetic-launcher>', 'exec')
            with self.assertRaises(ValueError): cow_plan.plan(source, source / 'bad', 'pixel', '/sdk/qemu-img', '/sdk/emulator', 8192)
            child.mkdir(); (child / 'userdata-branch.img').write_text('retained')
            with self.assertRaises(ValueError): cow_plan.plan(source, child, 'pixel', '/sdk/qemu-img', '/sdk/emulator', 8192)

    def test_cleanup_failure_still_records_final_host_and_stop_receipt_failure_cannot_suppress_stop(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            args = type('Args', (), {'output': d + '/attempt', 'adb': '/synthetic/adb'})()
            attempt = pixel_supervisor.PixelAttempt(args)
            attempt.pixel_readback = mock.Mock()
            with mock.patch.object(pixel_supervisor.base.Attempt, 'stop') as stop, mock.patch.object(Path, 'write_text', side_effect=OSError('diskfull')):
                attempt.stop(); stop.assert_called_once()
            attempt.memory_phase = mock.Mock()
            attempt.pixel_guest_disable.set = mock.Mock(side_effect=RuntimeError('monitor failed'))
            with mock.patch.object(pixel_supervisor.base.Attempt, 'cleanup') as cleanup:
                with self.assertRaises(RuntimeError): attempt.cleanup()
                cleanup.assert_called_once()
            self.assertEqual(attempt.memory_phase.call_args.args, ('cleanup-after-owned-disposal',))


if __name__ == '__main__': unittest.main()
