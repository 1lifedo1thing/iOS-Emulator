#!/usr/bin/env python3
"""Exercise actual reproducibility guards without compiler/firmware inputs."""
import json,pathlib,sys,tempfile
sys.dont_write_bytecode=True
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'contrib/portable'))
from cache import check_cache
with tempfile.TemporaryDirectory() as tmp:
 root=pathlib.Path(tmp);marker=root/'config.mak';marker.touch()
 identity={'abi':'arm64-v8a','compiler':'clang19','archive':'hash1','patch':'hash2','recipe':'hash3','flags':'PIC/static'}
 try:check_cache(root,identity,[marker],'test')
 except RuntimeError:pass
 else:raise AssertionError('unstamped compiled cache accepted')
 marker.unlink();stamp=check_cache(root,identity,[marker],'test');saved=stamp.read_bytes()
 marker.touch();check_cache(root,identity,[marker],'test');assert(stamp.read_bytes()==saved)
 for field in identity:
  changed=dict(identity);changed[field]+='-changed'
  try:check_cache(root,changed,[marker],'test')
  except RuntimeError:pass
  else:raise AssertionError('changed '+field+' accepted')
  assert(stamp.read_bytes()==saved)
print('PASS: actual cache guard rejects unstamped compiled state and changed ABI/compiler/archive/patch/recipe/options; matching replay preserves stamp')
