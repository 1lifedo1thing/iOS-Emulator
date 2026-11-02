#!/usr/bin/env python3
"""Controller contract checks; these are never native/guest cache acceptance."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('engine_probe',Path(__file__).with_name('portable-ir-cache-engine.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
spec=importlib.util.spec_from_file_location('library_entry',Path(__file__).with_name('portable-ir-cache-library-entry.py'))
entry=importlib.util.module_from_spec(spec);spec.loader.exec_module(entry)

class Tests(unittest.TestCase):
    def test_shared_adapter_argv_binds_actual_library_and_loader_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);library=root/'engine.so';adapter=root/'entry.py'
            library.write_bytes(b'not an ELF; host source contract only');adapter.write_bytes(b'not executed')
            actual=m.launch_arguments(library,m.digest(library),['-M','none','-S'],adapter,m.digest(adapter))
            self.assertEqual(actual,[m.sys.executable,str(adapter),'--library',str(library),'--sha256',m.digest(library),'--','-M','none','-S'])
            with self.assertRaisesRegex(RuntimeError,'adapter changed'):
                m.launch_arguments(library,m.digest(library),[],adapter,'0'*64)
            with patch.object(entry.ctypes,'CDLL') as load:
                with self.assertRaisesRegex(ValueError,'changed before load'):
                    entry.run_library(library,'0'*64,[])
                load.assert_not_called()

    def test_shared_entry_marshals_one_native_call_with_null_terminated_argv(self):
        with tempfile.TemporaryDirectory() as temporary:
            library=Path(temporary)/'engine.so';library.write_bytes(b'host-only fake library')
            calls=[]
            class Native:
                def __call__(self,argc,argv):
                    calls.append((argc,[argv[i] for i in range(argc)],argv[argc]));return 0
            class Library:qemu_ios_main=Native()
            with patch.object(entry.ctypes,'CDLL',return_value=Library()),patch.object(entry,'mapping_witness',return_value={'scope':'HOST_TEST_NOT_ACTUAL_MAPPING'}),patch('builtins.print'):
                self.assertEqual(entry.run_library(library,m.digest(library),['-M','none','-S']),0)
            self.assertEqual(calls,[(4,[bytes(library),b'-M',b'none',b'-S'],None)])

    def test_shared_mapping_record_requires_exact_loaded_file_pid_and_native_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            library=Path(temporary)/'engine.so';library.write_bytes(b'host-only fake map')
            info=library.stat();device='%02x:%02x'%(m.os.major(info.st_dev),m.os.minor(info.st_dev))
            record=dict(path=str(library),sha256=m.digest(library),pid=42,inode=info.st_ino,device=device,entrypoint='qemu_ios_main',
                        maps=['1000-2000 r-xp 0000 %s %d %s'%(device,info.st_ino,library)])
            log=lambda item:'SHARED_ENGINE_MAPPING '+m.json.dumps(item)+'\n'
            self.assertEqual(m.shared_mapping(log(record),library,m.digest(library),42),record)
            for changed in [dict(pid=43),dict(sha256='0'*64),dict(entrypoint='main'),dict(maps=[]),dict(maps=['bad map'])]:
                with self.subTest(changed=changed),self.assertRaises(RuntimeError):
                    m.shared_mapping(log(dict(record,**changed)),library,m.digest(library),42)
            with self.assertRaisesRegex(RuntimeError,'Exactly one'):
                m.shared_mapping(log(record)+log(record),library,m.digest(library),42)
    def rows(self):
        result={}
        for n,name in enumerate(['off','cold','reuse','different_cpu_mode','changed_guest_bytes','self_modifying_code']):
            result[name]={'status':'PASS','processRetired':True,'identity':{'pid':10+n,'startTicks':str(n),'hostBootId':'test'},'engineSHA256':'a'*64,'cacheEnabledWitness':name!='off','selfModify':name=='self_modifying_code','cacheCounters':None if name=='off' else {'hits':2 if name in ['reuse','self_modifying_code'] else 0,'stored':0 if name=='reuse' else 2,'misses':2,'rejected':0}}
        return result
    def test_matrix(self):
        self.assertEqual(m.validate_matrix(self.rows())['status'],'PASS')
    def test_fresh_process_required(self):
        rows=self.rows();rows['reuse']['identity']=rows['cold']['identity']
        with self.assertRaises(ValueError):m.validate_matrix(rows)
    def test_real_hit_required(self):
        rows=self.rows();rows['reuse']['cacheCounters']['hits']=0
        with self.assertRaises(ValueError):m.validate_matrix(rows)
    def test_off_witness_rejected(self):
        rows=self.rows();rows['off']['cacheEnabledWitness']=True
        with self.assertRaises(ValueError):m.validate_matrix(rows)
    def test_different_context_cannot_hit(self):
        rows=self.rows();rows['different_cpu_mode']['cacheCounters']['hits']=1
        with self.assertRaises(ValueError):m.validate_matrix(rows)
    def test_mixed_engine_rejected(self):
        rows=self.rows();rows['reuse']['engineSHA256']='b'*64
        with self.assertRaises(ValueError):m.validate_matrix(rows)
    def test_counts_monotonic(self):
        prefix='LightTouch TCG IR cache: '
        suffix='; frontend IR only, fresh native code\n'
        good=prefix+'hits=1 misses=2 rejected=0 stored=3'+suffix
        self.assertEqual(m.counters(good)['stored'],3)
        with self.assertRaises(ValueError):m.counters(good+prefix+'hits=0 misses=2 rejected=0 stored=3'+suffix)
    def test_legal_fixture(self):
        words=struct.unpack('<5I',m.words())
        branch=m.CODE_PC+16+8+((words[4]&0xffffff)<<2)
        self.assertEqual(branch,m.TARGET_PC)
        self.assertEqual(m.CODE_PC//1024,m.TARGET_PC//1024)
        self.assertNotEqual(m.CODE_PC%1024,0)
        self.assertEqual(struct.unpack('<5I',m.words(True))[1],0xe0400001)
    def test_delayed_first_execution_is_observed_without_stepping(self):
        class QMP:
            def __init__(self):self.commands=[]
            def command(self,name):self.commands.append(name)
        q=QMP();now=[0];states=iter([{'pc':m.CODE_PC},{'pc':m.CODE_PC},{'pc':m.TARGET_PC}])
        def sleep(delay):now[0]+=delay
        actual,observed=m.run_until_target(q,lambda:next(states),clock=lambda:now[0],sleep=sleep)
        self.assertEqual(actual['pc'],m.TARGET_PC)
        self.assertEqual(len(observed),3)
        self.assertEqual(q.commands,['cont','stop']*3)
    def test_unexecuted_guest_cannot_pass(self):
        class QMP:
            def command(self,name):pass
        now=[0]
        def sleep(delay):now[0]+=1
        with self.assertRaisesRegex(TimeoutError,'did not reach target'):
            m.run_until_target(QMP(),lambda:{'pc':m.CODE_PC},clock=lambda:now[0],sleep=sleep)
    def test_loop_only_hits_do_not_qualify_arithmetic_and_memory_replay(self):
        rows=self.rows();rows['reuse']['cacheCounters']['hits']=1
        with self.assertRaises(ValueError):m.validate_matrix(rows)
    def test_missing_self_modification_witness_is_rejected(self):
        rows=self.rows();rows['self_modifying_code']['selfModify']=False
        with self.assertRaises(ValueError):m.validate_matrix(rows)

if __name__=='__main__':unittest.main()
