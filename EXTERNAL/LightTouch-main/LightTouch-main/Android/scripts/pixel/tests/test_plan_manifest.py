# SPDX-License-Identifier: GPL-2.0-or-later
import copy,hashlib,json,subprocess,sys,tempfile,unittest
from unittest import mock
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import plan_manifest as plans


class Manifest(unittest.TestCase):
    def fixture(self, root):
        source=root/'source';source.mkdir();prepared=root/'prepared';prepared.mkdir()
        for name in ('pixel_supervisor.py','capture_origin.py','admit_observation.py','admit_ready.py','plan_manifest.py','bind_recovery.py','memory_monitor.py','ui_approval.py','diagnostic_control.py','host_lifecycle.py','persistent_avd.py'):(source/name).write_text('raise RuntimeError("synthetic VM must not execute")\n')
        controller=prepared/'controller.py';controller.write_text('# synthetic controller\n');launcher=prepared/'launcher';launcher.write_text('# synthetic launcher\n')
        sha=lambda b:hashlib.sha256(b).hexdigest();assets=[]
        for name in ('app-debug.apk','app-debug-androidTest.apk'):
            path=root/name;path.write_bytes(b'synthetic signed input placeholder');assets.append({'path':str(path),'sha256':sha(path.read_bytes())})
        receipt=root/'prior-clean.json';receipt.write_text(json.dumps({'nativePollSnapshotValid':True,'cleanShutdownConfirmed':True,'shutdownConfirmationInferred':False,'nativeStateDirty':False,'nativeStorageFailed':False,'nativeEngineExit':0}))
        seed={'assets':assets,'avd':'pixel8g','recordId':'a'*32,'recordPins':{k:'c'*64 for k in ('record.json','base/device.lock.json','assets/bootrom.bin','assets/guest.itpack')},'controllerSha256':sha(controller.read_bytes()),'newLauncherSha256':sha(launcher.read_bytes()),'supervisorArgv':['--user','synthetic','--adb','/synthetic/adb','--pixel-memory-mb','8192'],'priorCleanStopReceipt':{'path':str(receipt),'sha256':sha(receipt.read_bytes())}}
        return plans.build(seed,prepared,root/'attempt04',source,launcher,cold_recovery=False,token='b'*32)

    def test_single_identity_builds_every_stage_normal_clean(self):
        with tempfile.TemporaryDirectory() as d:
            p=self.fixture(Path(d));self.assertTrue(plans.validate(p,files=True))
            self.assertEqual(plans.option(p['afterStartAdmissionCommand'],'--token'),p['attemptToken'])
            self.assertEqual(plans.extra(p['instrumentationArgs'],'pixelColdRecovery'),'false')
            self.assertIn('--record-id',p['supervisorArgv']);self.assertNotIn('--fresh-library',p['supervisorArgv'])
            self.assertEqual(p['pixelGraphicsProvider'],'platform')
            self.assertEqual(plans.extra(p['instrumentationArgs'],'pixelGraphicsProvider'),'platform')

    def test_provider_is_closed_pinned_and_angle_is_only_explicit(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);p=self.fixture(root)
            angle=plans.build(p,root/'prepared',p['attempt'],root/'source',root/'prepared'/'launcher',token=p['attemptToken'],pixel_graphics_provider='angle-gl')
            self.assertEqual(plans.extra(angle['instrumentationArgs'],'pixelGraphicsProvider'),'angle-gl')
            self.assertTrue(plans.validate(angle,files=True))
            fresh=plans.build(angle,root/'prepared',p['attempt'],root/'source',root/'prepared'/'launcher',token=p['attemptToken'])
            self.assertEqual(fresh['pixelGraphicsProvider'],'platform')
            for value in (None,'auto','host','ANGLE','',0,['platform']):
                q=copy.deepcopy(p);q['pixelGraphicsProvider']=value
                with self.subTest(value=value),self.assertRaises(ValueError):plans.validate(q)
            for args in ([*p['instrumentationArgs'],'-e','pixelGraphicsProvider','angle-gl'],
                    [x if i!=p['instrumentationArgs'].index('pixelGraphicsProvider')+1 else 'angle-gl' for i,x in enumerate(p['instrumentationArgs'])]):
                q=copy.deepcopy(p);q['instrumentationArgs']=args
                with self.assertRaises(ValueError):plans.validate(q)

    def test_attach_identity_is_declared_pinned_live_checked_and_never_a_new_launch_flag(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);p=self.fixture(root);p['supervisorArgv']+=['--attach-avd-owner',str(root/'owner.json')]
            with self.assertRaisesRegex(ValueError,'Undeclared'):plans.validate(p)
            owner=root/'owner.json';owner.write_text('synthetic actual owner receipt')
            p['persistentOwnerReceipt']=str(owner)
            p=plans.build(p,root/'prepared',p['attempt'],root/'source',root/'prepared'/'launcher',token=p['attemptToken'])
            import persistent_avd
            with mock.patch.object(persistent_avd,'load_owner') as live:
                plans.validate(p,files=True);live.assert_called_once()
            owner.write_text('changed')
            with self.assertRaisesRegex(ValueError,'receipt hash'):plans.validate(p,files=True)

    def test_actual03_stale_token_regression_rejected_before_launch(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);p=self.fixture(root);p['afterStartAdmissionCommand'][-1]='d'*32
            with self.assertRaises(ValueError):plans.validate(p,files=True)
            manifest=root/'prepared'/'launch-command.json';manifest.write_text(json.dumps(p))
            result=subprocess.run([sys.executable,str(Path(plans.__file__)),'--manifest',str(manifest),'--launch'],capture_output=True)
            self.assertNotEqual(result.returncode,0);self.assertFalse((manifest.parent/'launch-validation-consumed.json').exists())
            self.assertNotIn(b'synthetic VM must not execute',result.stderr)

    def test_stage_output_record_mode_and_duplicate_tokens_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=self.fixture(Path(d))
            mutations=[lambda q:q['preflightCommands'][1].__setitem__(3,'/wrong'),lambda q:q['instrumentationArgs'].__setitem__(q['instrumentationArgs'].index('recordId')+1,'d'*32),lambda q:q['instrumentationArgs'].__setitem__(q['instrumentationArgs'].index('pixelColdRecovery')+1,'true'),lambda q:q.__setitem__('priorDirtyExpected',True),lambda q:q['afterStartAdmissionCommand'].extend(['--token',q['attemptToken']])]
            for mutate in mutations:
                q=copy.deepcopy(p);mutate(q)
                with self.assertRaises(ValueError):plans.validate(q)

    def test_changed_actual_source_controller_launcher_apk_or_clean_receipt_rejected(self):
        for kind in ('source','controller','launcher','apk','clean'):
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as d:
                p=self.fixture(Path(d));paths={'source':Path(p['sourceRoot'])/'admit_observation.py','controller':Path(p['controller']),'launcher':Path(plans.option(p['supervisorArgv'],'--emulator')),'apk':Path(p['assets'][0]['path']),'clean':Path(p['priorCleanStopReceipt']['path'])};paths[kind].write_text('changed')
                with self.assertRaises(ValueError):plans.validate(p,files=True)

    def test_measured56s_setup_can_fit_full300_with_original_client_budget(self):
        origin=10**12;start=origin+192*10**9
        budget=plans.client_budget_seconds(origin,start)
        self.assertGreater(budget,56+300);self.assertEqual(budget,608)
        self.assertEqual(plans.client_budget_seconds(origin,origin+799*10**9),1)
        with self.assertRaises(TimeoutError):plans.client_budget_seconds(origin,origin+800*10**9)

    def test_normal_mode_cannot_omit_real_clean_evidence_or_change_original_bounds(self):
        with tempfile.TemporaryDirectory() as d:
            p=self.fixture(Path(d))
            for field,value in (('priorCleanStopReceipt',None),('observationSeconds',299),('boundsSeconds',{'ui':241,'readback':810,'stop':840,'hard':900})):
                q=copy.deepcopy(p);q[field]=value
                with self.assertRaises(ValueError):plans.validate(q)


if __name__=='__main__':unittest.main()
