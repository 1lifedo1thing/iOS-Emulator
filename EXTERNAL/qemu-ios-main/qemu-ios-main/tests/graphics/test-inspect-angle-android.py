#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the production ELF inspector with bounded synthetic readelf output."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT=Path(__file__).with_name('inspect-angle-android.py')
NAMES=[f'lib{x}_lighttouch_angle.so' for x in ('EGL','GLESv1_CM','GLESv2')]
class Inspector(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        for name in NAMES:(self.root/name).write_bytes(b'synthetic-content-not-an-ELF')
        self.state={'machine':'Advanced Micro Devices X86-64','endian':"2's complement, little endian",'alignment':'0x4000','soname':None,'needed':'libc.so'}
        self.tool=self.root/'fake-readelf'
        self.tool.write_text('''#!/usr/bin/env python3
import json,sys
from pathlib import Path
p=Path(__file__).parent
with (p/'calls').open('a') as f:f.write(json.dumps(sys.argv)+'\\n')
s=json.loads((p/'metadata.json').read_text());name=Path(sys.argv[-1]).name
if '-h' in sys.argv:print("Class: ELF64\\nData: "+s['endian']+"\\nMachine: "+s['machine'])
elif '-l' in sys.argv:print("  LOAD 0x000000 0x000000 0x000000 0x1000 0x1000 R E "+s['alignment'])
else:print(" 0x000000000000000e (SONAME) Library soname: ["+(s['soname'] or name)+"]\\n 0x0000000000000001 (NEEDED) Shared library: ["+s['needed']+"]")
''');self.tool.chmod(0o700)
    def run_inspector(self,*extra):
        (self.root/'metadata.json').write_text(json.dumps(self.state))
        return subprocess.run([sys.executable,str(SCRIPT),'--libraries',str(self.root),'--readelf',str(self.tool),'--output',str(self.root/'result.json'),*extra],capture_output=True,text=True,timeout=5)
    def test_original_default_x86(self):
        r=self.run_inspector();self.assertEqual(r.returncode,0,r.stderr)
        report=json.loads((self.root/'result.json').read_text());self.assertEqual(report['abi'],'x86_64');self.assertEqual(set(report['objects']),set(NAMES))
    def test_explicit_arm64(self):
        self.state['machine']='AArch64';r=self.run_inspector('--abi','arm64');self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(json.loads((self.root/'result.json').read_text())['abi'],'arm64-v8a')
    def test_x86_cannot_be_relabelled_arm(self):
        self.assertNotEqual(self.run_inspector('--abi','arm64').returncode,0);self.assertFalse((self.root/'result.json').exists())
    def test_default_rejects_arm(self):
        self.state['machine']='AArch64';self.assertNotEqual(self.run_inspector().returncode,0)
    def test_invalid_abi_fails_before_readelf(self):
        self.assertNotEqual(self.run_inspector('--abi','riscv64').returncode,0);self.assertFalse((self.root/'calls').exists())
    def test_big_endian_rejected(self):
        self.state['endian']="2's complement, big endian";self.assertNotEqual(self.run_inspector().returncode,0)
    def test_small_alignment_rejected(self):
        self.state['alignment']='0x1000';self.assertNotEqual(self.run_inspector().returncode,0)
    def test_wrong_soname_rejected(self):
        self.state['soname']='libEGL.so';self.assertNotEqual(self.run_inspector().returncode,0)
    def test_linux_loader_rejected(self):
        self.state['needed']='ld-linux-aarch64.so.1';self.assertNotEqual(self.run_inspector().returncode,0)
    def test_unbundled_dependency_rejected(self):
        self.state['needed']='libother.so';self.assertNotEqual(self.run_inspector().returncode,0)
if __name__=='__main__':unittest.main()
