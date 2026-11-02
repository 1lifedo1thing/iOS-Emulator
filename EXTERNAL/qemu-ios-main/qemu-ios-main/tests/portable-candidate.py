#!/usr/bin/env python3
"""Small host-only checks for the operator driver; no compiler/device inputs."""
import importlib.util
import json
import os
import stat
import sys
import time
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('candidate', Path(__file__).resolve().parents[1] /
                                              'contrib/portable/run-candidate.py')
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)


class Candidate(unittest.TestCase):
    def test_explicit_api29_plan_is_arm64_only_and_preserves_api_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            plan = {'schema': 1, 'android_api': 29, 'jobs': 1,
                    'files': [], 'tools': [], 'source_revision': 'fixture',
                    'run_output': str(root / 'run'), 'path_environment': os.environ['PATH'],
                    'targets': [{'abi': 'arm64-v8a', 'build': str(root / 'build'),
                                 'notices': str(root / 'notices'),
                                 'configure_argv': ['build-engine.py', '--android-api', '29']}]}
            with mock.patch.object(candidate, 'source_check'):
                self.assertEqual(candidate.preflight(plan)['android_api'], 29)
                plan['targets'][0]['configure_argv'][-1] = '28'
                with self.assertRaisesRegex(RuntimeError, 'compiler API differs'):
                    candidate.preflight(plan)
                plan['targets'][0]['configure_argv'] = ['build-engine.py', '--android-api', '29',
                                                       '--android-api', '28']
                with self.assertRaisesRegex(RuntimeError, 'compiler API differs'):
                    candidate.preflight(plan)
                plan['targets'][0]['abi'] = 'linux'
                with self.assertRaisesRegex(RuntimeError, 'exactly one ARM64'):
                    candidate.preflight(plan)
            self.assertFalse((root / 'run').exists())
            self.assertFalse((root / 'build').exists())

    def test_real_ninja_job_group_is_counted_and_retired_on_rss_limit(self):
        # No compiler: Ninja runs one small Python allocation, in its real job
        # process group. It ignores TERM so cleanup must reach that group/KILL.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ready = root / 'ready.json'
            helper = root / 'allocate.py'
            helper.write_text('''import json,os,signal,time
from pathlib import Path
signal.signal(signal.SIGTERM, signal.SIG_IGN)
memory=bytearray(24*1024*1024)
for i in range(0,len(memory),4096): memory[i]=42
Path("ready.json").write_text(json.dumps({"pid":os.getpid(),"pgrp":os.getpgrp(),"session":os.getsid(0)}))
time.sleep(30)
''')
            (root / 'build.ninja').write_text(
                'rule allocation\n  command = ' + sys.executable + ' ' + str(helper) +
                '\nbuild allocation: allocation\n')
            output = root / 'logs'
            output.mkdir()
            plan = {'max_group_rss_kib': 24 * 1024, 'path_environment': os.environ['PATH']}
            rows = []
            begin = time.monotonic()
            with mock.patch.object(candidate, 'source_check'):
                with self.assertRaisesRegex(RuntimeError, 'RSS ceiling exceeded'):
                    candidate.run_step(plan, output, 'ninja-python-only',
                                       ['/usr/bin/ninja', '-j1'], root, 10, rows)
            child = json.loads(ready.read_text())
            self.assertNotEqual(child['pgrp'], child['session'])
            self.assertEqual(child['session'], rows[0]['pid'])
            self.assertGreater(rows[0]['sampled_peak_session_rss_kib'], 24 * 1024)
            self.assertTrue(rows[0]['cleanup_session_retired'])
            self.assertLess(time.monotonic() - begin, 8)
            # A transient init-owned zombie has no executing work or RSS; all
            # living members of the original owned session must be gone.
            self.assertFalse([p for p in candidate.session_processes(child['session'])
                              if p['state'] != 'Z'])

    def test_reused_session_leader_is_refused_before_any_signal(self):
        process = mock.Mock(pid=12345, candidate_session_start=111)
        newer = {'pid': 12345, 'pgrp': 12345, 'start': 222, 'state': 'S', 'rss_pages': 1}
        with mock.patch.object(candidate, 'session_processes', return_value=[newer]), \
             mock.patch.object(candidate.os, 'killpg') as signal_group:
            with self.assertRaisesRegex(RuntimeError, 'identity changed'):
                candidate.dispose_session(process)
            signal_group.assert_not_called()

    def test_actual_frozen_copies_are_verified_and_read_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / 'build'
            build.mkdir()
            names = ['libqemu-arm.so', 'portable-manifest.json',
                     'scheduler-actual-argv.json', 'libc++_shared.so']
            for name in names:
                (build / name).write_text(name + '\n')
            frozen = root / 'frozen' / 'first-abi'
            candidate.freeze_files(build, frozen, names)
            try:
                self.assertEqual(stat.S_IMODE(frozen.stat().st_mode), 0o555)
                for name in names:
                    self.assertEqual(candidate.sha(frozen / name), candidate.sha(build / name))
                    self.assertEqual(stat.S_IMODE((frozen / name).stat().st_mode), 0o444)
                # The shared parent must remain writable for the next ABI.
                (frozen.parent / 'next-abi').mkdir()
            finally:
                frozen.chmod(0o755)  # Allow ordinary temporary-tree cleanup.

    def test_preflight_mismatch_never_starts_command_or_creates_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pin = root / 'input'
            pin.write_text('actual')
            output = root / 'new'
            plan = {'schema': 1, 'android_api': 28, 'jobs': 1,
                    'files': [{'path': str(pin), 'sha256': '0' * 64}], 'tools': [],
                    'run_output': str(output), 'targets': [], 'path_environment': os.environ['PATH']}
            with mock.patch.object(candidate, 'source_check'), mock.patch.object(candidate.subprocess, 'Popen') as run:
                with self.assertRaisesRegex(RuntimeError, 'input changed'):
                    candidate.preflight(plan)
                run.assert_not_called()
            self.assertFalse(output.exists())

    def test_actual_bounded_host_command_records_exit_without_compilation(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            plan = {'max_group_rss_kib': 128 * 1024, 'path_environment': os.environ['PATH']}
            rows = []
            with mock.patch.object(candidate, 'source_check'):
                candidate.run_step(plan, output, 'host-only', ['/usr/bin/python3', '-c', 'print("host-only")'],
                                   output, 5, rows)
            self.assertEqual(rows[0]['exit'], 0)
            self.assertIn('host-only', (output / 'host-only.log').read_text())
            self.assertEqual(json.loads((output / 'commands.json').read_text())[0]['pid'], rows[0]['pid'])
            self.assertFalse(Path('/proc', str(rows[0]['pid'])).exists())


if __name__ == '__main__':
    unittest.main()
