# SPDX-License-Identifier: GPL-2.0-or-later
import contextlib,hashlib,io,json,subprocess,sys,tempfile,time,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import persistent_avd as persistent
import pixel_supervisor


class Persistent(unittest.TestCase):
    def owner_readiness_case(self,never_ready):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);out=root/'owner';launcher=root/'launcher';launcher.write_text('# synthetic')
            plan={'output':str(out),'avd':'LightTouch_pixel_10_API36_16384_24_8192','ramMB':8192,'pageBytes':16384,
                'launcher':str(launcher),'launcherSha256':hashlib.sha256(launcher.read_bytes()).hexdigest(),'adb':'/synthetic/adb'}
            path=root/'plan.json';path.write_text(json.dumps(plan));(root/'ROOT-PERSISTENT-AVD-GRANT.json').write_text(json.dumps({'owner':'/root/pixel','persistentAvdAuthorized':True}))
            avd=mock.Mock(pid=123);avd.poll.return_value=None
            avd.wait.side_effect=lambda timeout:setattr(avd.poll,'return_value',0)
            ticks=[0];probes=[0];calls=[]
            def now():ticks[0]+=1;return ticks[0]
            def sleep(seconds):
                if (out/'owner.json').exists() or (out/'readiness-unconfirmed.json').exists():
                    self.assertIsNone(avd.poll());(out/'REQUEST-STOP-AVD').write_text('explicit synthetic user stop')
            def run(argv,**kw):
                calls.append(argv)
                if argv[-1]=='devices':return subprocess.CompletedProcess(argv,0,b'List of devices\n',b'')
                if argv[-2:]==['emu','kill']:
                    self.assertTrue((out/'REQUEST-STOP-AVD').exists());return subprocess.CompletedProcess(argv,0,b'OK',b'')
                self.assertLessEqual(kw['timeout'],2);probes[0]+=1
                if never_ready or probes[0]==1:raise subprocess.TimeoutExpired(argv,kw['timeout'],output=b'partial',stderr=b'probe timeout')
                if probes[0]==2:raise OSError('transient adb unavailable')
                value=b'1\n' if argv[-1]=='sys.boot_completed' else b'16384\n' if argv[-1]=='PAGE_SIZE' else b'11111111-2222-3333-4444-555555555555\n'
                return subprocess.CompletedProcess(argv,0,value,b'')
            q=mock.Mock(side_effect=[({},[]),({123:10,124:11},[(124,11)])])
            with mock.patch.object(persistent.subprocess,'run',side_effect=run),mock.patch.object(persistent.subprocess,'Popen',return_value=avd) as spawn, \
                 mock.patch.object(persistent,'identity',return_value=10),mock.patch.object(persistent,'owned_qemu',q), \
                 mock.patch.object(persistent.time,'monotonic',side_effect=now),mock.patch.object(persistent.time,'sleep',side_effect=sleep),contextlib.redirect_stdout(io.StringIO()):
                persistent.launch_owner(path)
            spawn.assert_called_once();avd.kill.assert_not_called();avd.terminate.assert_not_called()
            errors=[json.loads(line) for line in (out/'readiness-probe-errors.jsonl').read_text().splitlines()]
            self.assertTrue(all(e['emulatorPreserved'] for e in errors));self.assertTrue((out/'owner-terminal.json').exists())
            if never_ready:self.assertTrue((out/'readiness-unconfirmed.json').exists());self.assertFalse((out/'owner.json').exists())
            else:self.assertTrue((out/'owner.json').exists());self.assertEqual(q.call_count,2)

    def test_transient_adb_timeouts_errors_and_zero_qemu_retry_same_owner(self):self.owner_readiness_case(False)

    def test_original210_expiry_keeps_owner_alive_unconfirmed_until_explicit_stop(self):self.owner_readiness_case(True)

    def test_attach_flag_is_separate_from_original_guest_memory_and_clock(self):
        owner,rest=pixel_supervisor.attach_options(['--attach-avd-owner','/private/owner.json','--avd','same'])
        self.assertEqual(owner,'/private/owner.json');self.assertEqual(rest,['--avd','same'])

    def test_owner_reused_pid_or_changed_android_boot_refuses_attachment(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'owner.json';owner={'schema':1,'avd':'same','serial':'emulator-5554','pageBytes':16384,'ramMB':8192,
                'wrapperPid':123,'wrapperStartTicks':10,'qemuPid':124,'qemuStartTicks':11,'bootId':'original'}
            p.write_text(json.dumps(owner));run=mock.Mock(return_value=subprocess.CompletedProcess([],0,b'original\n',b''))
            with mock.patch.object(persistent,'identity',side_effect=lambda pid:{123:10,124:11}[pid]):
                value,pin=persistent.load_owner(p,'same','/synthetic/adb',run=run)
                self.assertEqual(value,owner);self.assertEqual(pin,hashlib.sha256(p.read_bytes()).hexdigest())
                run.return_value=subprocess.CompletedProcess([],0,b'changed\n',b'')
                with self.assertRaises(ValueError):persistent.load_owner(p,'same','/synthetic/adb',run=run)
            with mock.patch.object(persistent,'identity',return_value=999):
                with self.assertRaises(ValueError):persistent.load_owner(p,'same','/synthetic/adb',run=run)

    def test_guest_cleanup_stop_failure_still_retires_only_guest_helpers_and_preserves_avd(self):
        for fail in (False,True):
            with self.subTest(fail=fail):
                calls=[];avd=mock.Mock(pid=123);avd.poll.return_value=None
                complete=mock.Mock();watch=mock.Mock(pid=44);watch.is_alive.return_value=False
                a=SimpleNamespace(stop_attempted=False,avd=avd,args=SimpleNamespace(adb='/synthetic/adb'),
                    output=Path('/synthetic/private'),deadline=time.monotonic()+900,complete=complete,
                    watcher=watch,logcat=None,start=time.monotonic(),write=lambda name,value:calls.append((name,value)))
                def stop():
                    calls.append(('Stop',{}))
                    if fail:raise RuntimeError('Stop unconfirmed')
                a.stop=stop
                with mock.patch.object(persistent,'force_bound_worker',return_value={'guestShutdownConfirmed':False}), \
                     mock.patch.object(persistent.subprocess,'run') as run:
                    if fail:
                        with self.assertRaises(RuntimeError):persistent.cleanup_attempt(a)
                    else:persistent.cleanup_attempt(a)
                run.assert_not_called();avd.terminate.assert_not_called();avd.kill.assert_not_called()
                complete.set.assert_called_once();self.assertEqual(calls[-1][0],'terminal.json')
                self.assertTrue(calls[-1][1]['emulatorPreserved'])

    def force_case(self,changed=False):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);clock=b'actual immutable context';(out/'clock-context.json').write_bytes(clock)
            b={'appUid':10215,'workerPid':42,'workerStartTicks':1234,'bootId':'11111111-2222-3333-4444-555555555555',
                'recordId':'a'*32,'attemptToken':'b'*32,'originToken':hashlib.sha256(clock).hexdigest(),'originNs':1}
            (out/'pixel-bound.json').write_text(json.dumps(b));calls=[]
            stat=('42 (engine) '+' '.join(['S']+['0']*18+['9999' if changed else '1234'])).encode()
            values={'/proc/sys/kernel/random/boot_id':b['bootId'].encode(),'files/library/selected':b['recordId'].encode(),
                '/proc/42/stat':stat,'/proc/42/status':b'Uid: 10215 10215 10215 10215\n','/proc/42/cmdline':b'org.lighttouch.android:engine\0'}
            def run(argv,**kw):
                calls.append(argv)
                return subprocess.CompletedProcess(argv,0,values.get(argv[-1],b''),b'')
            result=persistent.force_bound_worker('/synthetic/adb',out,10,run=run,now=lambda:1)
            return result,calls

    def test_hard_fallback_targets_original_worker_only_never_avd_or_replacement(self):
        result,calls=self.force_case()
        self.assertTrue(result['originalWorkerKillRequested'])
        self.assertEqual(calls[-1][-3:],['kill','-9','42'])
        self.assertFalse(any('emu' in argv or 'am' in argv for argv in calls))
        self.assertFalse(result['guestShutdownConfirmed'])
        result,calls=self.force_case(changed=True)
        self.assertFalse(result['originalWorkerKillRequested']);self.assertFalse(any('kill' in argv for argv in calls))

    def test_owner_rejects_existing_serial_before_any_launch(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);launcher=root/'launcher';launcher.write_text('# synthetic')
            plan={'output':str(root/'owner'),'avd':'LightTouch_pixel_10_API36_16384_24_8192','ramMB':8192,'pageBytes':16384,
                'launcher':str(launcher),'launcherSha256':hashlib.sha256(launcher.read_bytes()).hexdigest(),'adb':'/synthetic/adb'}
            path=root/'plan.json';path.write_text(json.dumps(plan));(root/'ROOT-PERSISTENT-AVD-GRANT.json').write_text(json.dumps({'owner':'/root/pixel','persistentAvdAuthorized':True}))
            with mock.patch.object(persistent.subprocess,'run',return_value=subprocess.CompletedProcess([],0,b'List of devices\nemulator-5554\tdevice\n',b'')),mock.patch.object(persistent.subprocess,'Popen') as spawn:
                with self.assertRaises(ValueError):persistent.launch_owner(path)
                spawn.assert_not_called()


if __name__=='__main__':unittest.main()
