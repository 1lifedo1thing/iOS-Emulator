import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

SOURCE = Path(__file__).with_name('detached_test.py').resolve()
spec = importlib.util.spec_from_file_location('detached_test', SOURCE)
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)


class DetachedTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def plan(self, code, timeout=4):
        script = self.root / 'work.py'
        script.write_text(code)
        executable = str(Path(sys.executable).resolve())
        value = dict(execution_authorized=True, argv=[executable, str(script)],
            cwd=str(self.root), timeout_seconds=timeout,
            admission_deadline_ns=time.monotonic_ns()+30_000_000_000,
            sha256={executable: d.digest(executable), str(script): d.digest(script)})
        path = self.root / 'plan.json'
        path.write_text(json.dumps(value))
        return path

    def wait_result(self, out):
        until = time.monotonic()+8
        while time.monotonic() < until:
            path = out / 'result.json'
            if path.exists():
                return json.loads(path.read_text())
            time.sleep(.02)
        self.fail('detached supervisor did not produce finite result')

    def test_default_does_not_execute(self):
        run = subprocess.run([sys.executable, str(SOURCE)], capture_output=True, timeout=3)
        self.assertEqual(run.returncode, 0)
        self.assertIn(b'REVIEW ONLY', run.stdout)

    def test_long_acceptance_remains_finite_and_admission_stays_fresh(self):
        path = self.plan('print("not launched during validation")\n')
        plan = json.loads(path.read_text())
        for timeout in (780, 1500, 1800):
            with self.subTest(timeout=timeout):
                plan['timeout_seconds'] = timeout
                d.validate(plan, time.monotonic_ns())
        for timeout in (0, -1, 1800.001, float('inf'), float('nan'), True, '780'):
            with self.subTest(rejected=timeout):
                plan['timeout_seconds'] = timeout
                with self.assertRaisesRegex(ValueError, 'finite timeout'):
                    d.validate(plan, time.monotonic_ns())
        plan['timeout_seconds'] = 1800
        plan['admission_deadline_ns'] = time.monotonic_ns()+91_000_000_000
        with self.assertRaisesRegex(ValueError, 'admission'):
            d.validate(plan, time.monotonic_ns())
        self.assertFalse((self.root/'result').exists())

    def test_child_survives_invoking_session_hangup(self):
        plan = self.plan('import time\ntime.sleep(.8)\nprint("collected")\n')
        out = self.root / 'result'
        invocation = ('import importlib.util,time;'
            f's=importlib.util.spec_from_file_location("d",{str(SOURCE)!r});'
            'd=importlib.util.module_from_spec(s);s.loader.exec_module(d);'
            f'd.launch(d.Path({str(plan)!r}),{d.digest(plan)!r},d.Path({str(out)!r}));'
            'time.sleep(30)')
        parent = subprocess.Popen([sys.executable, '-c', invocation], start_new_session=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            until = time.monotonic()+4
            while not (out/'controller.json').exists() and time.monotonic() < until:
                time.sleep(.02)
            self.assertTrue((out/'controller.json').exists())
            supervisor = json.loads((out/'supervisor.json').read_text())
            controller = json.loads((out/'controller.json').read_text())
            self.assertNotEqual(supervisor['session'], parent.pid)
            self.assertNotEqual(controller['session'], supervisor['session'])
            os.killpg(parent.pid, signal.SIGHUP)
            parent.wait(timeout=2)
            self.assertEqual(parent.returncode, -signal.SIGHUP)
            result = self.wait_result(out)
            self.assertTrue(result['success'])
            self.assertEqual((out/'stdout').read_text(), 'collected\n')
            self.assertFalse(result['deviceShutdownVerified'])
        finally:
            if parent.poll() is None:
                os.killpg(parent.pid, signal.SIGKILL); parent.wait(timeout=2)

    def test_timeout_kills_controller_group_and_keeps_original_deadline(self):
        code = ('import subprocess,sys,time\n'
            'from pathlib import Path\n'
            'p=subprocess.Popen([sys.executable,"-c","import time;time.sleep(30)"])\n'
            'Path("descendant.pid").write_text(str(p.pid))\n'
            'time.sleep(30)\n')
        plan = self.plan(code, timeout=.6)
        out = self.root / 'result'
        d.launch(plan, d.digest(plan), out)
        result = self.wait_result(out)
        envelope = json.loads((out/'envelope.json').read_text())
        self.assertFalse(result['success'])
        self.assertTrue(result['timed_out'])
        self.assertEqual(result['original_hard_deadline_ns'], envelope['hard_deadline_ns'])
        self.assertLess(result['completed_ns']-envelope['hard_deadline_ns'], 2_000_000_000)
        pid = int((self.root/'descendant.pid').read_text())
        stat = Path(f'/proc/{pid}/stat')
        if stat.exists():
            self.assertEqual(stat.read_text().rsplit(')', 1)[1].split()[0], 'Z')

    def test_reuse_and_changed_source_refused_before_launch(self):
        plan = self.plan('print("once")\n')
        out = self.root/'result'
        d.launch(plan, d.digest(plan), out)
        self.assertTrue(self.wait_result(out)['success'])
        with self.assertRaises(FileExistsError):
            d.launch(plan, d.digest(plan), out)
        (self.root/'work.py').write_text('print("changed")\n')
        with self.assertRaisesRegex(ValueError, 'source pin'):
            d.launch(plan, d.digest(plan), self.root/'changed')
        self.assertFalse((self.root/'changed').exists())

    def test_expired_plan_and_reboot_never_execute(self):
        path = self.plan('raise RuntimeError("must not execute")\n')
        plan = json.loads(path.read_text())
        plan['admission_deadline_ns'] = time.monotonic_ns()-1
        with self.assertRaisesRegex(ValueError, 'admission'):
            d.validate(plan, time.monotonic_ns())
        out = self.root/'reboot'
        out.mkdir()
        envelope = dict(boot_id='another-boot', plan=plan,
            hard_deadline_ns=time.monotonic_ns()+1_000_000_000)
        self.assertEqual(d.supervise(envelope, out), 1)
        result = json.loads((out/'result.json').read_text())
        self.assertIn('reboot', result['error'])
        self.assertIsNone(result['issued_ns'])
        self.assertFalse((out/'controller.json').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
