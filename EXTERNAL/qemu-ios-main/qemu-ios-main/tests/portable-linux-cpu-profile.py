#!/usr/bin/env python3
"""No perf/sudo/guest attachment: exercise collector refusal and attribution."""
import importlib.util
import os
from pathlib import Path
import subprocess
import signal
import json
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

SOURCE = Path(__file__).resolve().parents[1] / 'contrib/portable/linux-cpu-profile.py'
SPEC = importlib.util.spec_from_file_location('cpu_profile', SOURCE)
profile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profile)


class CpuProfile(unittest.TestCase):
    def test_actual_self_identity(self):
        current = profile.identity(os.getpid())
        self.assertEqual(current['uid'], os.getuid())
        self.assertEqual(current['start_ticks'], profile.identity(os.getpid(), os.getpid())['start_ticks'])

    def test_prepare_refuses_ordinary_nonworker_without_writing(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            args = Mock(pid=os.getpid(), preset='user-40s', thread=[str(os.getpid()) + ':test-only'],
                        engine=Path(sys.executable), plan=root / 'plan.json', output=root / 'new')
            with self.assertRaisesRegex(ValueError, 'Native14 worker'):
                profile.prepare(args)
            self.assertFalse(args.plan.exists())
            self.assertFalse(args.output.exists())

    def test_fixed_capture_and_explicit_threads(self):
        argv = profile.profile_argv({'threads': [{'tid': 101}, {'tid': 102}]}, Path('/private/new'))
        self.assertEqual(argv[argv.index('-t') + 1], '101,102')
        self.assertIn('--no-inherit', argv)
        self.assertIn('--no-buildid-cache', argv)
        self.assertNotIn('--no-buildid', argv) # Preserve recording identity; suppress only cache writes.
        self.assertEqual(argv[-3:], ['--', '/usr/bin/sleep', '40'])
        self.assertEqual(argv[argv.index('-e') + 1], 'cpu-clock:u')
        self.assertEqual(argv[argv.index('-c') + 1], '20000000')
        self.assertNotIn('-p', argv)

    def test_narrow_kernel_preset_binds_bounds_and_event(self):
        spec = dict(profile.PRESETS['user-kernel-10s'], preset='user-kernel-10s')
        plan = {'capture': spec, 'threads': [{'tid': 101}, {'tid': 102}]}
        argv = profile.profile_argv(plan, Path('/private/new'))
        self.assertEqual(argv[-1], '10')
        self.assertIn('15s', argv)
        self.assertEqual(argv[argv.index('-e') + 1], 'cpu-clock')
        self.assertIn('--fsize=67108864:67108864', argv)
        self.assertEqual(spec['outer_seconds'], 18)
        self.assertTrue(spec['include_misc'])
        plan['threads'].append({'tid': 103})
        with self.assertRaisesRegex(ValueError, 'bounded thread'):
            profile.profile_argv(plan, Path('/private/new'))
        for changed in ({'preset': 'unbounded'}, dict(spec, seconds=11),
                        dict(spec, event='cpu-clock:u'), dict(spec, max_bytes=profile.MAX_BYTES)):
            with self.subTest(changed=changed):
                with self.assertRaises(ValueError):
                    profile.capture_spec({'capture': changed})

    def test_legacy_and_explicit_default_argv_identical(self):
        old = {'threads': [{'tid': 101}]}
        explicit = dict(old, capture=dict(profile.PRESETS['user-40s'], preset='user-40s'))
        self.assertEqual(profile.profile_argv(old, Path('/private/new')),
                         profile.profile_argv(explicit, Path('/private/new')))

    def test_kernel_mode_never_classified_as_native_qemu(self):
        source = '\n'.join([
            '11/12 U 10.123456789: 20000000 abc tlb_flush (/private/libqemu-arm.so)',
            '11/12 K 10.223456789: 20000000 def tlb_flush ([kernel.kallsyms])',
            '11/13 K 10.323456789: 20000000 0 [unknown] ([kernel.kallsyms])',
        ])
        parsed = profile.parse_samples(source, include_misc=True)
        self.assertTrue(parsed['classification_valid'])
        self.assertEqual(parsed['period_ns_by_cpu_mode'], {'user': 20000000, 'kernel': 40000000})
        self.assertEqual(parsed['period_ns_by_thread_and_cpu_mode'],
                         {'12': {'user': 20000000, 'kernel': 20000000}, '13': {'kernel': 20000000}})
        self.assertEqual([r['category'] for r in parsed['samples']],
                         ['native-execution-helper-or-mmu', 'kernel-resolved', 'kernel-unresolved'])
        self.assertFalse(profile.parse_samples(source)['classification_valid'])
        self.assertFalse(profile.parse_samples(source.replace(' K ', ' ? '), include_misc=True)['classification_valid'])
        self.assertFalse(profile.parse_samples(source.replace(' U ', ' '), include_misc=True)['classification_valid'])

    def test_actual_inert_worker_prepare_and_preflight_never_attach(self):
        # Only a test-owned inert Python child carrying the worker marker.
        child = subprocess.Popen([sys.executable, '-c',
            'import sys; print("ready", flush=True); sys.stdin.read()', '--worker'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), 'ready')
            with tempfile.TemporaryDirectory() as name:
                root = Path(name)
                args = Mock(pid=child.pid, preset='user-kernel-10s',
                            thread=[str(child.pid) + ':inert-test-only'],
                            engine=Path('/proc') / str(child.pid) / 'exe',
                            plan=root / 'plan.json', output=root / 'recording')
                with patch('builtins.print'), patch.object(profile.subprocess, 'Popen') as popen:
                    profile.prepare(args)
                    profile.collect(Mock(plan=args.plan, execute_approved_sha256=None))
                popen.assert_not_called()
                plan = json.loads(args.plan.read_text())
                self.assertEqual(plan['capture']['event'], 'cpu-clock')
                self.assertEqual(plan['capture']['max_threads'], 2)
                self.assertEqual(plan['threads'][0]['start_ticks'],
                                 profile.identity(child.pid)['start_ticks'])
                self.assertFalse(args.output.exists())
                self.assertFalse((root / 'ROOT-GRANT.json').exists())
        finally:
            child.stdin.close()
            child.wait(timeout=2)
            child.stdout.close()

    def test_actual_output_refusals(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            profile.outside_git(root / 'fresh')
            for path in (root, Path('relative')):
                with self.assertRaises(ValueError):
                    profile.outside_git(path)
            (root / 'link').symlink_to(root / 'missing')
            with self.assertRaises(ValueError):
                profile.outside_git(root / 'link')
            (root / '.git').write_text('gitdir: elsewhere')
            with self.assertRaises(ValueError):
                profile.outside_git(root / 'fresh')

    def test_self_samples_and_unknown_are_not_guest(self):
        rows = '\n'.join([
            '11 12 10.123456789: 20000000 abc tcg_optimize+0x10 (/private/libqemu-arm.so)',
            '11 12 10.223456789: 20000000 def tcg_out_op (/private/libqemu-arm.so)',
            '11 12 10.323456789: 20000000 123 tb_gen_code (/private/libqemu-arm.so)',
            '11 12 10.423456789: 20000000 456 memory_region_dispatch_read (/private/libqemu-arm.so)',
            '11 12 10.523456789: 20000000 789 glReadPixels (/usr/lib/dri/test.so)',
            '11 12 10.623456789: 20000000 111 [unknown] ([anon:tcg-code])',
        ])
        parsed = profile.parse_samples(rows)
        self.assertTrue(parsed['classification_valid'])
        self.assertEqual(len(parsed['samples']), 6)
        self.assertEqual(parsed['period_ns_by_self_category']['unresolved'], 20000000)
        self.assertEqual(len(parsed['period_ns_by_self_category']), 6)
        self.assertTrue(profile.parse_samples(rows.replace('11 12 ', '11/12 '))['classification_valid'])
        self.assertFalse(profile.parse_samples('')['classification_valid'])
        rejected = profile.parse_samples(rows + '\nunexpected perf format')
        self.assertFalse(rejected['classification_valid'])
        self.assertEqual(rejected['unparsed_rows'], ['unexpected perf format'])

    def test_normal_retirement_does_not_signal_worker(self):
        child = Mock(pid=810)
        with patch.object(profile, 'owned_session_members', return_value={}), \
             patch.object(profile.subprocess, 'run') as run:
            profile.retire_collector(child, '20')
        run.assert_not_called()
        child.wait.assert_called_once()

    def test_stale_child_refuses_signal(self):
        child = Mock(pid=810)
        child.poll.return_value = None
        with patch.object(profile, 'owned_session_members', return_value={810: '21'}), \
             patch.object(profile, 'identity', return_value={'start_ticks': '21'}), \
             patch.object(profile.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'identity changed'):
                profile.retire_collector(child, '20')
        run.assert_not_called()

    def test_actual_owned_sleep_session_cleanup(self):
        # Ordinary child only; translate the privileged kill argv to os.killpg
        # for precisely this test-owned session, never invoking sudo/perf.
        child = subprocess.Popen(['/usr/bin/sleep', '5'],
                                 start_new_session=True)
        try:
            start = profile.identity(child.pid)['start_ticks']
            def own_kill(argv, **kwargs):
                self.assertEqual(argv[-1], '-' + str(child.pid))
                self.assertEqual(argv[-3], 'INT')
                os.killpg(child.pid, signal.SIGINT)
            with patch.object(profile.subprocess, 'run', side_effect=own_kill):
                profile.retire_collector(child, start)
            self.assertIsNotNone(child.returncode)
            self.assertEqual(profile.owned_session_members(child.pid), {})
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=1)

    def test_failed_recording_refused_before_offline_commands(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            profile.save(root / 'terminal.json', {'exit': 1, 'failure': 'record failed', 'cleanup_failure': None})
            args = Mock(recording=root, output=root / 'report')
            with patch.object(profile.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'failed recording'):
                    profile.report(args)
            run.assert_not_called()

    def test_terminal_preserves_record_and_cleanup_failure(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            plan_path = root / 'plan.json'
            profile.save(plan_path, {'output': str(root / 'new-output'), 'pid': 123})
            digest = profile.sha(plan_path)
            profile.save(root / 'ROOT-GRANT.json', {'plan_sha256': digest,
                         'tool_sha256': profile.sha(SOURCE)})
            args = Mock(plan=plan_path, execute_approved_sha256=digest)
            child = Mock(pid=999, returncode=1)
            child.poll.return_value = 1
            fd = os.open('/dev/null', os.O_RDONLY)
            watch = Mock()
            watch.poll.return_value = []
            with patch.object(profile, 'validate', return_value={}), \
                 patch.object(profile, 'profile_argv', return_value=['not-executed']), \
                 patch.object(profile.os, 'pidfd_open', return_value=fd), \
                 patch.object(profile.select, 'poll', return_value=watch), \
                 patch.object(profile, 'identity', return_value={'start_ticks': '2'}), \
                 patch.object(profile.subprocess, 'Popen', return_value=child), \
                 patch.object(profile, 'retire_collector', side_effect=RuntimeError('cleanup refused')), \
                 patch('builtins.print'):
                with self.assertRaisesRegex(ValueError, 'recording failed'):
                    profile.collect(args)
            receipt = json.loads((root / 'new-output/terminal.json').read_text())
            self.assertIn('recording failed', receipt['failure'])
            self.assertIn('cleanup refused', receipt['cleanup_failure'])
            self.assertIsNone(receipt['perf_data_sha256'])


if __name__ == '__main__':
    unittest.main()
