# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pixel_supervisor

SOURCE = Path(__file__).resolve().parents[1]


class HostLifecycle(unittest.TestCase):
    def test_actual_pending_term_during_start_and_identity_is_delivered_inside_disposal_scope(self):
        for phase in ('start','identity'):
            with self.subTest(phase=phase):
                child=self.run_child('''
import host_lifecycle,pixel_supervisor,multiprocessing,os,time,json,signal
events=[];host_lifecycle.install(events)
phase=sys.argv[1];owned=[];original=pixel_supervisor.base.process_identity
def blocked(*args):
    os.setsid()
    signal.pthread_sigmask(signal.SIG_SETMASK,args[3])
    while True:time.sleep(1)
pixel_supervisor.capture_child=blocked
class P(multiprocessing.get_context('fork').Process):
    def start(self):
        super().start()
        if phase=='start':os.kill(os.getpid(),signal.SIGTERM)
def factory(**kwargs):
    p=P(**kwargs);owned.append(p);return p
def identity(pid):
    value=original(pid)
    if phase=='identity':os.kill(os.getpid(),signal.SIGTERM)
    return value
pixel_supervisor.base.process_identity=identity
try:
    pixel_supervisor.bounded_capture('/synthetic/adb',{'originNs':time.monotonic_ns()},'/synthetic/out',process_factory=factory)
except KeyboardInterrupt:
    events.append({'unwound':True})
finally:
    p=owned[0]
    print(json.dumps({'pid':p.pid,'alive':p.is_alive(),'exit':p.exitcode,'events':events}),flush=True)
''',(phase,))
                stdout,stderr=child.communicate(timeout=3)
                self.assertEqual(child.returncode,0,stderr)
                result=json.loads(stdout)
                self.assertFalse(result['alive'])
                self.assertEqual(result['exit'],-signal.SIGKILL)
                self.assertEqual(result['events'][0]['signal'],signal.SIGTERM)
                self.assertFalse(Path(f"/proc/{result['pid']}").exists())

    def test_actual_host_signal_during_capture_join_retires_blocked_child_before_outer_finally(self):
        for number in (signal.SIGTERM,signal.SIGHUP):
            with self.subTest(number=number):
                child=self.run_child('''
import host_lifecycle,pixel_supervisor,multiprocessing,os,time,json,signal
events=[];host_lifecycle.install(events)
def blocked(*args):
    os.setsid()
    print(json.dumps({'capturePid':os.getpid()}),flush=True)
    while True:time.sleep(1)
pixel_supervisor.capture_child=blocked
owned=[]
def factory(**kwargs):
    p=multiprocessing.get_context('fork').Process(**kwargs);owned.append(p);return p
try:
    pixel_supervisor.bounded_capture('/synthetic/adb',{'originNs':time.monotonic_ns()},'/synthetic/out',process_factory=factory)
except KeyboardInterrupt:
    events.append({'unwound':True})
finally:
    p=owned[0]
    print(json.dumps({'aliveBeforeOuterCleanup':p.is_alive(),'captureExit':p.exitcode,'events':events}),flush=True)
''')
                capture_pid=None
                try:
                    capture_pid=json.loads(child.stdout.readline())['capturePid']
                    child.send_signal(number)
                    stdout,stderr=child.communicate(timeout=3)
                    self.assertEqual(child.returncode,0,stderr)
                    result=json.loads(stdout)
                    self.assertFalse(result['aliveBeforeOuterCleanup'])
                    self.assertEqual(result['captureExit'],-signal.SIGKILL)
                    self.assertEqual(result['events'][0]['signal'],number)
                    self.assertFalse(Path(f'/proc/{capture_pid}').exists())
                finally:
                    if child.poll() is None:child.kill();child.wait()
                    if capture_pid and Path(f'/proc/{capture_pid}').exists():
                        os.kill(capture_pid,signal.SIGKILL)

    def test_signal_stop_or_receipt_failure_cannot_suppress_owned_cleanup(self):
        for fail_receipt in (False, True):
            with self.subTest(fail_receipt=fail_receipt):
                attempt=pixel_supervisor.PixelAttempt.__new__(pixel_supervisor.PixelAttempt)
                calls=[]
                attempt.pixel_host_signals=[{'signal':signal.SIGTERM}]
                attempt.avd=object();attempt.stop_attempted=False;attempt.pixel_monitor=None
                attempt.pixel_guest_disable=mock.Mock();attempt.memory_phase=mock.Mock()
                attempt.retire_monitor=lambda:calls.append('retire-monitor')
                def write(*args):
                    calls.append('receipt')
                    if fail_receipt:raise OSError('receipt unavailable')
                attempt.write=write
                def stop():
                    calls.append('stop')
                    raise RuntimeError('Stop unavailable')
                attempt.stop=stop
                with mock.patch.object(pixel_supervisor.base.Attempt,'cleanup',lambda s:calls.append('cleanup')):
                    with self.assertRaisesRegex(RuntimeError,'Stop unavailable'):
                        attempt.cleanup()
                self.assertEqual(calls,['receipt','stop','cleanup','retire-monitor'])

    def run_child(self, script, args=()):
        return subprocess.Popen([sys.executable, '-u', '-c',
            'import sys;sys.path.insert(0,' + repr(str(SOURCE)) + ');' + script, *args],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def test_actual_term_and_hup_unwind_finally_without_handler_io(self):
        for number in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(number=number):
                code = '''
import host_lifecycle,json,time
events=[]
host_lifecycle.install(events)
print('ready',flush=True)
try:
    time.sleep(10)
except KeyboardInterrupt:
    events.append({'unwound':True})
finally:
    events.append({'cleanup':True})
    print(json.dumps(events),flush=True)
'''
                child=self.run_child(code)
                try:
                    self.assertEqual(child.stdout.readline().strip(),'ready')
                    child.send_signal(number)
                    stdout,stderr=child.communicate(timeout=3)
                    self.assertEqual(child.returncode,0,stderr)
                    rows=json.loads(stdout)
                    self.assertEqual(rows[0]['signal'],number)
                    self.assertFalse(rows[0]['senderKnown'])
                    self.assertEqual(rows[1:],[{'unwound':True},{'cleanup':True}])
                finally:
                    if child.poll() is None:child.kill();child.wait()

    def test_forked_helper_uses_default_signal_and_never_parent_cleanup(self):
        child=self.run_child('''
import host_lifecycle,os,signal,json
events=[]
host_lifecycle.install(events)
pid=os.fork()
if pid==0:
    os.kill(os.getpid(),signal.SIGTERM)
    os._exit(99)
_,status=os.waitpid(pid,0)
print(json.dumps({'term':os.WIFSIGNALED(status) and os.WTERMSIG(status)==signal.SIGTERM,'parentEvents':events}))
''')
        stdout,stderr=child.communicate(timeout=3)
        self.assertEqual(child.returncode,0,stderr)
        self.assertEqual(json.loads(stdout),{'term':True,'parentEvents':[]})

    def test_supervisor_constructor_saves_actual_origin_before_any_avd(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'attempt';wrapper=SOURCE/'pixel_supervisor.py'
            child=self.run_child('''
import argparse,json,os
import pixel_supervisor,capture_origin
p=argparse.ArgumentParser();p.add_argument('--output');p.add_argument('--avd');p.add_argument('--expected-page-bytes',type=int)
a=p.parse_args(sys.argv[2:]);attempt=pixel_supervisor.PixelAttempt(a)
raw=(attempt.output/'clock-context.json').read_bytes()
context=json.loads(raw)
assert context['supervisorPid']==os.getpid() and attempt.avd is None
assert context['originNs']/1e9==attempt.start
capture_origin.attach_session(a.output,os.getpid(),987,a.avd)
assert (attempt.output/'clock-context.json').read_bytes()==raw
try:capture_origin.capture(a.output,os.getpid(),987,a.avd)
except FileExistsError:pass
else:raise AssertionError('original clock overwritten')
print(json.dumps({'context':context,'session':json.loads((attempt.output/'exec-session.json').read_text())}))
''',(str(wrapper),'--output',str(out),'--avd','synthetic-pixel','--expected-page-bytes','16384'))
            stdout,stderr=child.communicate(timeout=3)
            self.assertEqual(child.returncode,0,stderr)
            result=json.loads(stdout)
            self.assertIsNone(result['context']['persistentExecSession'])
            self.assertFalse(result['context']['clockOriginReset'])
            self.assertEqual(result['session']['persistentExecSession'],987)


if __name__=='__main__':unittest.main()
