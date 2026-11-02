#!/usr/bin/env python3
"""Synthetic evidence contract checks only; no actual guest/performance pass."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('report',Path(__file__).resolve().parents[1]/'contrib/portable/ir-cache-session-report.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.path=Path(self.tmp.name)
        self.rows=[]
        def evidence(name,data):
            p=self.path/name;p.write_bytes(data);return {'path':str(p),'sha256':hashlib.sha256(data).hexdigest()}
        image=evidence('synthetic.image',b'SYNTHETIC UNIT FIXTURE; NOT A SCREENSHOT')
        for n,variant in enumerate(['off','cold','reuse','off','cold','reuse']):
            cache={'initialEntryCount':0,'afterInventorySHA256':'c'*64}
            if variant=='reuse':cache.update(producerTrialId='trial'+str(n-1),beforeInventorySHA256='c'*64)
            log=b'' if variant=='off' else ('LightTouch TCG IR cache: experimental enabled, fixture\nLightTouch TCG IR cache: hits='+str(int(variant=='reuse'))+' misses=2 rejected=0 stored=2; frontend IR only, fresh native code\n').encode()
            self.rows.append({'scope':'ACTUAL_IOS_GUEST','trialId':'trial'+str(n),'variant':variant,'identity':{**{key:'a'*64 for key in m.IDENTITY[:-1]},'hostBootId':'synthetic'},'worker':{'pid':10+n,'startTicks':100+n,'retired':True},'shutdown':{'confirmed':True,'dirty':False,'nativeExitCode':0,'forcedSignals':[]},'resources':{'admissionPassed':True,'continuousGuardPassed':True},'nativeLog':evidence(str(n)+'.log',log),'cache':cache,'actions':[{'name':'Photos','expectedScene':'photo','visualReview':{'inspected':True,'scene':'photo','beforeImage':image,'completeImage':image},'inputBeginNS':10,'inputEndNS':20,'lastTransitionSourceNS':30,'firstCompleteSourceNS':40,'workerCpuBeforeNS':100,'workerCpuAfterNS':200}]})
    def test_comparison_labels_owner_evidence(self):
        r=m.compare(self.rows);self.assertEqual(r['status'],'OWNER_EVIDENCE_COMPARED');self.assertTrue(r['limits'])
    def test_mismatched_engine_rejected(self):
        self.rows[-1]['identity']['engineSHA256']='b'*64
        with self.assertRaises(ValueError):m.compare(self.rows)
    def test_same_process_rejected(self):
        self.rows[2]['worker']=copy.deepcopy(self.rows[1]['worker'])
        with self.assertRaises(ValueError):m.compare(self.rows)
    def test_unreviewed_guest_scene_rejected(self):
        self.rows[2]['actions'][0]['visualReview']['inspected']=False
        with self.assertRaises(ValueError):m.compare(self.rows)
    def test_dirty_or_forced_exit_rejected(self):
        for key,value in [('dirty',True),('nativeExitCode',False),('forcedSignals',[15])]:
            rows=copy.deepcopy(self.rows);rows[0]['shutdown'][key]=value
            with self.assertRaises(ValueError):m.compare(rows)
    def test_reuse_producer_binding_required(self):
        self.rows[2]['cache']['beforeInventorySHA256']='b'*64
        with self.assertRaises(ValueError):m.compare(self.rows)
    def test_retained_bytes_required(self):
        Path(self.rows[2]['nativeLog']['path']).write_bytes(b'tampered')
        with self.assertRaises(ValueError):m.compare(self.rows)
    def test_one_run_not_performance_proof(self):
        with self.assertRaises(ValueError):m.compare(self.rows[:3])
    def test_malformed_or_truncated_counter_rejected(self):
        for log in [b'LightTouch TCG IR cache: hits=garbage\n',b'LightTouch TCG IR cache: hits=1 misses=2 rejected=0 stored=2; frontend IR only, fresh native code']:
            with self.assertRaises(ValueError):m.counter_records(log)
    def test_non_ios_scope_rejected(self):
        self.rows[0]['scope']='SYNTHETIC_NATIVE_FIXTURE'
        with self.assertRaises(ValueError):m.compare(self.rows)

if __name__=='__main__':unittest.main()
