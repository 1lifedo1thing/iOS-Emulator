#!/usr/bin/env python3
"""Synthetic offline accounting tests; no guest or performance qualification."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('feasibility',Path(__file__).resolve().parents[1]/'contrib/portable/ir-cache-feasibility.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.p=Path(self.tmp.name)
        self.log=self.p/'native.log';self.cache=self.p/'cache.json';self.result=self.p/'result.json'
        self.row={'mode':'cold','status':'FAIL','before':{'records':0},
            'after':{'mappedImageSHA256':'a'*64,'records':2,'bytes':400,'entries':{'x':{'bytes':150},'y':{'bytes':250}}},
            'native_final':{'hits':0,'misses':5,'rejected':0,'stored':2}}
        self.result_row={'status':'FAIL','confirmed_clean_receipt':False,'dirty':True}
        self.publish()
    def publish(self):
        self.cache.write_text(json.dumps(self.row));self.result_row['cache_evidence_sha256']=hashlib.sha256(self.cache.read_bytes()).hexdigest()
        self.result.write_text(json.dumps(self.result_row))
        if self.row['mode']=='off':self.log.write_bytes(b'')
        else:self.log.write_text('LightTouch TCG IR cache: experimental enabled, mapped-image-sha256='+ 'a'*64+'; 32MiB/4096 records maximum\n'+self.line(self.row['native_final']))
    def line(self,row):return 'LightTouch TCG IR cache: '+' '.join(k+'='+str(row[k]) for k in ('hits','misses','rejected','stored'))+'; frontend IR only, fresh native code\n'
    def observe(self):return m.observe(self.log,self.cache,self.result)
    def test_failed_dirty_evidence_remains_failed_and_unqualified(self):
        report=self.observe();self.assertEqual(report['sourceRuntimeStatus'],'FAIL');self.assertTrue(report['sourceDirty'])
        self.assertFalse(report['guestQualified']);self.assertFalse(report['performanceQualified'])
        self.assertEqual(report['statistics']['fsyncCallLowerBoundFromSuccessfulPublishes'],4)
    def test_unknown_denominators_and_durations_are_explicit_null(self):
        stats=self.observe()['statistics']
        for name in ['a32KeyEligibleFractionOfAllTranslatedTbs','translatedArmTbAttempts','translatedThumbTbAttempts','candidateCaptureRejects','measuredFileFsyncWallTimeNS','measuredSaturationMonotonicNS','measuredFrontendDecodeCpuTimeNS']:
            self.assertIsNone(stats[name])
    def test_candidate_denominator_includes_found_but_rejected_records(self):
        self.row['mode']='reuse';self.row['before']['records']=1
        self.row['native_final']=dict(hits=2,misses=3,rejected=5,stored=1);self.publish()
        stats=self.observe()['statistics'];self.assertEqual(stats['candidateLookupAttempts'],10)
        self.assertEqual(stats['candidateLookupHitFraction'],.2)
    def test_saturation_yield_is_reported_without_inventing_timing(self):
        self.row['after'].update(records=4096,bytes=4096*150,entries={str(n):{'bytes':150} for n in range(4096)})
        self.row['native_final']=dict(hits=0,misses=16000,rejected=0,stored=4096);self.publish()
        self.log.write_text(self.log.read_text().split('LightTouch TCG IR cache: hits=')[0]+self.line(dict(hits=0,misses=12000,rejected=0,stored=4096))+self.line(self.row['native_final']))
        stats=self.observe()['statistics'];self.assertEqual(stats['recordCapacityFraction'],1)
        self.assertLess(stats['byteCapacityFraction'],.1);self.assertAlmostEqual(stats['coldStoreYieldAtCountSaturation'],4096/12000)
        self.assertIsNone(stats['measuredSaturationMonotonicNS'])
    def test_receipt_binding_rejects_modified_inventory(self):
        self.cache.write_text('{}')
        with self.assertRaisesRegex(ValueError,'bind supplied'):self.observe()
    def test_counter_and_inventory_mismatch_rejected(self):
        for mutate in [lambda r:r['after'].update(bytes=401),lambda r:r['after'].update(records=3),
                       lambda r:r['native_final'].update(stored=1),lambda r:r['after'].update(mappedImageSHA256='b'*64)]:
            original=copy.deepcopy(self.row);mutate(self.row);self.publish()
            with self.assertRaises(ValueError):self.observe()
            self.row=original
    def test_nonmonotonic_or_truncated_telemetry_rejected(self):
        base=self.log.read_text()
        for suffix in [self.line(dict(hits=0,misses=1,rejected=0,stored=1)),self.line(self.row['native_final']).rstrip('\n')]:
            self.log.write_text(base+suffix)
            with self.assertRaises(ValueError):self.observe()
    def test_off_mode_has_no_fabricated_coverage(self):
        self.row={'mode':'off','status':'SCOPED_PASS'};self.publish()
        report=self.observe();self.assertIsNone(report['statistics']);self.assertFalse(report['performanceQualified'])
        self.log.write_text(self.line(dict(hits=1,misses=0,rejected=0,stored=0)))
        with self.assertRaises(ValueError):self.observe()
    def test_bounded_read_does_not_load_oversized_evidence(self):
        with self.log.open('wb') as stream:stream.truncate(m.session.MAX_LOG+1)
        with self.assertRaisesRegex(ValueError,'bounded offline'):self.observe()

if __name__=='__main__':unittest.main()
