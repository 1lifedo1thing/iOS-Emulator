# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib,json,re,shlex,subprocess,sys,tempfile,unittest,time
from types import SimpleNamespace
from pathlib import Path
from unittest import mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import bind_recovery
import pixel_supervisor


class Recovery(unittest.TestCase):
    def test_marker_timeout_partial_streams_and_late_positive_status_cannot_admit_clean(self):
        for late in (False,True):
            with self.subTest(late=late),tempfile.TemporaryDirectory() as d:
                def run(argv,**kw):
                    if not late:raise subprocess.TimeoutExpired(argv,1,output=b'partial',stderr=b'partial error')
                    return subprocess.CompletedProcess(argv,0,b'LT_MARKER_ABSENT\n',b'')
                capture=SimpleNamespace(adb='/synthetic/adb',run=run,now=mock.Mock(side_effect=[10**9,11*10**9]),deadline=10*10**9)
                with self.assertRaises(ValueError):bind_recovery.normal_marker_absence(capture,'files/library/records/'+'a'*32,d)
                receipt=json.loads((Path(d)/'normal-marker-presence.json').read_text())
                self.assertFalse(receipt['confirmedAbsent'])
                if not late:
                    self.assertEqual(receipt['adbExit'],124);self.assertEqual(receipt['stdout'],'partial');self.assertEqual(receipt['stderr'],'partial error')

    def test_real_shell_marker_status_requires_absence_and_rejects_inaccessible_or_symlink_state(self):
        for kind in ('absent','present','dangling-marker','denied-state','missing-state','symlink-state'):
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as d:
                out=Path(d);root='files/library/records/'+'a'*32;state=out/root/'state'
                state.mkdir(parents=True)
                if kind=='present':(state/'.dirty').write_text('retained dirty')
                if kind=='dangling-marker':(state/'.dirty').symlink_to('missing')
                if kind=='denied-state':state.chmod(0)
                if kind=='missing-state':state.rmdir()
                if kind=='symlink-state':state.rmdir();state.symlink_to(out)
                def run(argv,**kw):
                    remote=shlex.split(argv[-1]);self.assertEqual(remote[:4],['run-as','org.lighttouch.android','sh','-c'])
                    return subprocess.run(['/bin/sh','-c',remote[4]],cwd=out,**kw)
                capture=SimpleNamespace(adb='/synthetic/adb',run=run,now=time.monotonic_ns,deadline=time.monotonic_ns()+5_000_000_000)
                try:
                    if kind=='absent':
                        result=bind_recovery.normal_marker_absence(capture,root,out)
                        self.assertEqual(result['stdout'],'LT_MARKER_ABSENT\n');self.assertTrue(result['confirmedAbsent'])
                    else:
                        with self.assertRaises(ValueError):bind_recovery.normal_marker_absence(capture,root,out)
                        self.assertFalse(json.loads((out/'normal-marker-presence.json').read_text())['confirmedAbsent'])
                finally:
                    if kind=='denied-state':state.chmod(0o700)

    def test_merged_or_split_error_text_and_ambiguous_output_never_mean_marker_absence(self):
        for stdout,stderr,code in ((b'head: marker: No such file or directory\n\n__LT_PIXEL_REMOTE_EXIT__=1\n',b'',0),
                (b'\n__LT_PIXEL_REMOTE_EXIT__=1\n',b'No such file or directory',0),
                (b'LT_MARKER_ABSENT\n',b'Permission denied',0),(b'Permission denied\nLT_MARKER_ABSENT\n',b'',0),
                (b'LT_MARKER_UNKNOWN\n',b'',0),(b'LT_MARKER_ABSENT\n',b'',1)):
            with self.subTest(stdout=stdout,stderr=stderr,code=code),tempfile.TemporaryDirectory() as d:
                capture=SimpleNamespace(adb='/synthetic/adb',run=lambda argv,**kw:subprocess.CompletedProcess(argv,code,stdout,stderr),now=lambda:1,deadline=10**9)
                with self.assertRaises(ValueError):bind_recovery.normal_marker_absence(capture,'files/library/records/'+'a'*32,d)
                self.assertFalse(json.loads((Path(d)/'normal-marker-presence.json').read_text())['confirmedAbsent'])

    def setUp(self):
        self.host_init = mock.patch.object(pixel_supervisor.PixelAttempt, 'initialize_host_lifecycle')
        self.host_init.start()
        self.addCleanup(self.host_init.stop)

    def fixture(self, out, change=None):
        uid=b'Uid:\t10215\t10215\t10215\t10215\n'; record='a'*32; token='b'*32
        stat=('42 (engine) '+' '.join(['S']+['0']*18+['1234'])).encode()
        base=b'base';rom=b'rom';guest=b'guest';sha=lambda x:hashlib.sha256(x).hexdigest()
        meta=json.dumps({'schema':1,'id':record,'base_sha256':sha(base),'bootrom_sha256':sha(rom),'guest_package_sha256':sha(guest)}).encode()
        root='files/library/records/'+record
        data={'/proc/self/status':uid,'/proc/42/stat':stat,'/proc/42/status':uid,'/proc/42/cmdline':b'org.lighttouch.android:engine\0',
            '/proc/sys/kernel/random/boot_id':b'11111111-2222-3333-4444-555555555555','files/library/selected':record.encode(),
            root+'/record.json':meta,root+'/base/device.lock.json':base,root+'/assets/bootrom.bin':rom,root+'/assets/guest.itpack':guest,root+'/state/.dirty':b'actual interrupted',root+'/state/.base-identity':b'existing base identity'}
        if change:data.update(change)
        context={'originNs':10**12,'supervisorPid':777,'supervisorStartTicks':100000,'clockTicksPerSecond':100}
        (out/'clock-context.json').write_text(json.dumps(context));(out/'memory-monitor.json').write_text(json.dumps({'originNs':10**12,'ownerPid':999,'ownerStartTicks':789}))
        pins={suffix:sha(data[root+'/'+suffix]) for suffix in ('record.json','base/device.lock.json','assets/bootrom.bin','assets/guest.itpack')}
        def run(argv,**kw):
            if argv[-2:] == ['pidof','org.lighttouch.android:engine']:return subprocess.CompletedProcess(argv,0,b'42\n',b'')
            if argv[3:]==['exec-out','cat','/proc/sys/kernel/random/boot_id','/proc/uptime','/proc/sys/kernel/random/boot_id']:
                boot=data['/proc/sys/kernel/random/boot_id'];return subprocess.CompletedProcess(argv,0,boot+b'\n100.00 200.00\n'+boot+b'\n',b'')
            if 'LT_MARKER_ABSENT' in argv[-1]:
                status=b'LT_MARKER_ABSENT\n' if data.get(root+'/state/.dirty') is None else b'LT_MARKER_PRESENT\n'
                return subprocess.CompletedProcess(argv,0,status,b'')
            script=argv[-1];m=re.search(r'head -c \d+ ([^;]+);',script)
            path=shlex.split(m[1])[0].strip("'")
            content=data.get(path)
            marker='__LT_PIXEL_REMOTE_EXIT__' if '__LT_PIXEL_REMOTE_EXIT__' in script else '__LT_MEMORY_EXIT__'
            stdout=(content or b'')+f'\n{marker}={0 if content is not None else 1}\n'.encode()
            return subprocess.CompletedProcess(argv,0,stdout,b'' if content is not None else b'No such file or directory')
        return record,token,pins,run,data

    def bound(self):
        return {'schema':1,'originNs':10**12,'appUid':10215,'workerPid':42,'workerStartTicks':1234,'bootId':'11111111-2222-3333-4444-555555555555',
            'recordId':'a'*32,'attemptToken':'b'*32,'originToken':'c'*64,'priorDirty':True,'coldRecoveryUsed':True,'pinsVerified':True,
            'graphicsProviderRequested':'platform','graphicsProviderObserved':'platform','graphicsProviderFixed':True}

    def test_observed_provider_cannot_silently_fallback_or_omit_fixed_selection(self):
        for chosen in ('platform','angle-gl'):
            prior={**self.bound(),'graphicsProviderRequested':chosen}
            ready={**prior,'graphicsProviderObserved':chosen}
            bind_recovery.match_ready(prior,ready)
            for change in ({'graphicsProviderRequested':'angle-gl' if chosen=='platform' else 'platform'},
                    {'graphicsProviderObserved':'angle-gl' if chosen=='platform' else 'platform'},
                    {'graphicsProviderFixed':False},{'graphicsProviderFixed':1},{'graphicsProviderObserved':None}):
                with self.subTest(chosen=chosen,change=change),self.assertRaises(ValueError):bind_recovery.match_ready(prior,{**ready,**change})
        prior=self.bound();prior.pop('graphicsProviderRequested')
        with self.assertRaises(ValueError):bind_recovery.match_ready(prior,self.bound())

    def test_unknown_provider_fails_before_any_adb_or_state_read(self):
        run=mock.Mock()
        with self.assertRaises(ValueError):bind_recovery.bind('/synthetic/adb','/not-created','b'*32,'a'*32,{},run=run,pixel_graphics_provider='auto')
        run.assert_not_called()

    def test_match_original_recovery_identity_and_mode(self):
        bound=self.bound();bind_recovery.match_ready(bound,bound)
        for change in ({'workerPid':43},{'workerStartTicks':1235},{'appUid':10216},{'bootId':'other'}, {'recordId':'d'*32},{'priorDirty':False},{'coldRecoveryUsed':False}):
            with self.subTest(change=change),self.assertRaises(ValueError):bind_recovery.match_ready(bound,{**bound,**change})
        with self.assertRaises(ValueError):bind_recovery.match_ready({**bound,'pinsVerified':False},bound)

    def test_actual_selected_dirty_pins_and_worker_bound_without_start_or_import(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);record,token,pins,run,data=self.fixture(out)
            with mock.patch.object(bind_recovery,'process_ticks',side_effect=lambda p:{777:100000,999:789}[p]),mock.patch('memory_monitor.process_ticks',side_effect=lambda p:789):
                # Sampler's default callable is definition-bound, so use its constructor's injected checker.
                original=bind_recovery.Sampler
                def sampler(*a,**kw):return original(*a,identity=lambda p:789,**kw)
                with mock.patch.object(bind_recovery,'Sampler',side_effect=sampler):
                    result=bind_recovery.bind('/synthetic/adb',out,token,record,pins,run=run,now=lambda:10**12+100*10**9)
            self.assertTrue(result['priorDirty']);self.assertTrue(result['pinsVerified']);self.assertFalse(result['guestAcceptancePassed'])
            self.assertTrue(result['beforeStart']);self.assertTrue((out/'recovery-record-bound.json').exists())
            self.assertEqual(result['graphicsProviderRequested'],'platform')
            self.assertNotIn('graphicsProviderObserved',result)  # Selection has not run yet.
            self.assertEqual(data['files/library/records/'+record+'/state/.dirty'],b'actual interrupted')

    def test_missing_dirty_permission_or_different_pin_fails_no_binding(self):
        for kind in ('missing','wrong-pin','wrong-selected','wrong-worker'):
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as d:
                out=Path(d);record,token,pins,run,data=self.fixture(out)
                if kind=='missing':data['files/library/records/'+record+'/state/.dirty']=None
                if kind=='wrong-pin':pins['assets/guest.itpack']='0'*64
                if kind=='wrong-selected':data['files/library/selected']=b'different'
                if kind=='wrong-worker':data['/proc/42/cmdline']=b'org.lighttouch.android\0'
                original=bind_recovery.Sampler
                with mock.patch.object(bind_recovery,'process_ticks',side_effect=lambda p:{777:100000,999:789}[p]),mock.patch.object(bind_recovery,'Sampler',side_effect=lambda *a,**kw:original(*a,identity=lambda p:789,**kw)):
                    with self.assertRaises(ValueError):bind_recovery.bind('/synthetic/adb',out,token,record,pins,run=run,now=lambda:10**12+100*10**9)
                self.assertFalse((out/'recovery-record-bound.json').exists())

    def test_precleanup_falls_back_to_prestart_binding_when_admission_failed(self):
        with tempfile.TemporaryDirectory() as d:
            args=type('Args',(),{'output':d+'/attempt','adb':'/synthetic/adb'})()
            attempt=pixel_supervisor.PixelAttempt(args)
            context={'originNs':10**12,'supervisorStartTicks':100000,'clockTicksPerSecond':100}
            raw=json.dumps(context).encode();bound=self.bound();bound['originToken']=hashlib.sha256(raw).hexdigest()
            (attempt.output/'clock-context.json').write_bytes(raw)
            (attempt.output/'recovery-record-bound.json').write_text(json.dumps(bound))
            attempt.avd=mock.Mock()  # Synthetic owned launcher exists; no process started.
            with mock.patch.object(pixel_supervisor,'bounded_capture',return_value={'captureStarted':True}) as capture:
                attempt.pixel_readback();capture.assert_called_once()
                self.assertEqual(capture.call_args.args[1]['attemptToken'],bound['attemptToken'])
            receipt=json.loads((attempt.output/'recovery-stop-binding.json').read_text())
            self.assertFalse(receipt['postStartBindingAvailable']);self.assertFalse(receipt['guestShutdownConfirmed'])

    def test_stop_rejects_replaced_worker_without_admitting_gui_stop_and_cleanup_runs(self):
        with tempfile.TemporaryDirectory() as d:
            args=type('Args',(),{'output':d+'/attempt','adb':'/synthetic/adb'})()
            attempt=pixel_supervisor.PixelAttempt(args);bound=self.bound()
            raw=b'original-clock';bound['originToken']=hashlib.sha256(raw).hexdigest()
            (attempt.output/'clock-context.json').write_bytes(raw)
            (attempt.output/'recovery-record-bound.json').write_text(json.dumps(bound))
            stat=('42 (engine) '+' '.join(['S']+['0']*18+['1235'])).encode()
            def adb(*argv,**kw):
                data={'stat':stat,'status':b'Uid: 10215 10215 10215 10215\n','cmdline':b'org.lighttouch.android:engine\0'}
                return 0,data[argv[-1].rsplit('/',1)[-1]]
            attempt.adb=mock.Mock(side_effect=adb);attempt.path_presence=mock.Mock(return_value=True)
            attempt.pixel_readback=mock.Mock()
            with mock.patch.object(pixel_supervisor.base.Attempt,'stop') as stop:
                attempt.stop();stop.assert_not_called()
            receipt=json.loads((attempt.output/'stop-admission.json').read_text())
            self.assertFalse(receipt['guestShutdownConfirmed']);self.assertFalse(receipt['replacementWorkerStopAdmitted'])
            self.assertTrue(attempt.stop_attempted);self.assertEqual(attempt.record_id,'a'*32)
            with mock.patch.object(pixel_supervisor.base.Attempt,'cleanup') as cleanup:
                attempt.cleanup();cleanup.assert_called_once()

    def test_stop_original_worker_admitted_record_and_pid_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            args=type('Args',(),{'output':d+'/attempt','adb':'/synthetic/adb'})()
            attempt=pixel_supervisor.PixelAttempt(args);bound=self.bound();raw=b'original-clock'
            bound['originToken']=hashlib.sha256(raw).hexdigest()
            (attempt.output/'clock-context.json').write_bytes(raw);(attempt.output/'recovery-record-bound.json').write_text(json.dumps(bound))
            stat=('42 (engine) '+' '.join(['S']+['0']*18+['1234'])).encode()
            def adb(*argv,**kw):
                if argv[1]=='pidof':return 0,b'42\n'
                if argv[-1]=='/proc/sys/kernel/random/boot_id':return 0,bound['bootId'].encode()
                if argv[-1]=='files/library/selected':return 0,bound['recordId'].encode()
                data={'stat':stat,'status':b'Uid: 10215 10215 10215 10215\n','cmdline':b'org.lighttouch.android:engine\0'}
                return 0,data[argv[-1].rsplit('/',1)[-1]]
            attempt.adb=mock.Mock(side_effect=adb);attempt.pixel_readback=mock.Mock()
            with mock.patch.object(pixel_supervisor.base.Attempt,'stop') as stop:
                attempt.stop();stop.assert_called_once()
            self.assertEqual(attempt.worker_pid,'42');self.assertEqual(attempt.record_id,'a'*32)

    def test_normal_clean_binding_requires_absent_marker_not_permission_denial(self):
        for denied in (False,True):
            with self.subTest(denied=denied),tempfile.TemporaryDirectory() as d:
                out=Path(d);record,token,pins,run,data=self.fixture(out)
                data['files/library/records/'+record+'/state/.dirty']=None
                def invoke(argv,**kw):
                    r=run(argv,**kw)
                    if denied and '/state/.dirty' in argv[-1]:r.stderr=b'Permission denied'
                    return r
                original=bind_recovery.Sampler
                with mock.patch.object(bind_recovery,'process_ticks',side_effect=lambda p:{777:100000,999:789}[p]),mock.patch.object(bind_recovery,'Sampler',side_effect=lambda *a,**kw:original(*a,identity=lambda p:789,**kw)):
                    if denied:
                        with self.assertRaises(ValueError):bind_recovery.bind('/synthetic/adb',out,token,record,pins,run=invoke,now=lambda:10**12+100*10**9,cold_recovery=False)
                        self.assertFalse((out/'normal-record-bound.json').exists())
                    else:
                        result=bind_recovery.bind('/synthetic/adb',out,token,record,pins,run=invoke,now=lambda:10**12+100*10**9,cold_recovery=False)
                        self.assertFalse(result['priorDirty']);self.assertFalse(result['coldRecoveryUsed']);self.assertTrue(result['dirtyReceipt']['confirmedAbsent'])
                        bind_recovery.match_ready(result,{**result,'graphicsProviderObserved':'platform','graphicsProviderFixed':True})
                        self.assertTrue((out/'normal-record-bound.json').exists());self.assertFalse((out/'recovery-record-bound.json').exists())

    def test_prestart_reads_actual_padded_role_and_rejects_real_identity_change(self):
        b=self.bound()
        stat=('42 (engine) '+' '.join(['S']+['0']*18+['1234'])).encode()
        class Controller:
            values={'boot_id':b['bootId'].encode(),'selected':b['recordId'].encode(),
                'stat':stat,'status':b'Uid: 10215 10215 10215 10215\n',
                'cmdline':b'org.lighttouch.android:engine'.ljust(99,b'\0')}
            def run(self,label,argv,*a):return self.values[argv[-1].rsplit('/',1)[-1]]
        c=Controller()
        result=bind_recovery.recheck_before_start(c,b)
        self.assertEqual(result['cmdlineBytes'],99);self.assertTrue(result['identityMatched'])
        self.assertFalse(result['guestAcceptancePassed'])
        for name,value in (('cmdline',b'org.lighttouch.android\0'),('status',b'Uid: 10216 10216 10216 10216\n'),('selected',b'other')):
            with self.subTest(name=name):
                old=c.values[name];c.values[name]=value
                with self.assertRaises(ValueError):bind_recovery.recheck_before_start(c,b)
                c.values[name]=old

    def test_late_origin_dispatches_no_adb(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);record,token,pins,run,data=self.fixture(out);runner=mock.Mock()
            with mock.patch.object(bind_recovery,'process_ticks',return_value=100000):
                with self.assertRaises(TimeoutError):bind_recovery.bind('/synthetic/adb',out,token,record,pins,run=runner,now=lambda:10**12+491*10**9)
            runner.assert_not_called()


if __name__=='__main__':unittest.main()
