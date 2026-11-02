#!/usr/bin/env python3
"""Prove actual ARM TCG instructions execute through the shared engine.

Private temporary legal guest contains four ARM instructions (MOV,MOV,STR,B).
This is CPU/JIT smoke evidence, never iOS boot or graphics acceptance.
"""
import argparse,json,pathlib,struct,subprocess,tempfile,time,socket
p=argparse.ArgumentParser();p.add_argument('runner');p.add_argument('library');a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='lt-tcg-') as tmp:
 root=pathlib.Path(tmp);guest=root/'guest.bin';dump=root/'word.bin';qmp=root/'qmp.sock'
 guest.write_bytes(struct.pack('<4I',0xe3a0002a,0xe3a01101,0xe5810100,0xeafffffe))
 args=[a.runner,a.library,'qemu','-M','virt','-cpu','cortex-a15','-m','16M','-nodefaults','-display','none','-serial','none','-monitor','none','-qmp',f'unix:{qmp},server=on,wait=off','-device',f'loader,file={guest},addr=0x40200000,cpu-num=0']
 with open(root/'engine.log','w') as log:
  process=subprocess.Popen(args,stdout=log,stderr=log)
  try:
   deadline=time.monotonic()+20
   while not qmp.exists():
    if process.poll() is not None:raise RuntimeError((root/'engine.log').read_text())
    if time.monotonic()>deadline:raise TimeoutError('engine QMP startup timeout')
    time.sleep(.05)
   client=socket.socket(socket.AF_UNIX);client.settimeout(10);client.connect(str(qmp));stream=client.makefile('rwb',buffering=0)
   greeting=json.loads(stream.readline());assert 'QMP' in greeting
   def command(name,args=None):
    stream.write((json.dumps({'execute':name,**({'arguments':args} if args else {})})+'\n').encode())
    while True:
     reply=json.loads(stream.readline())
     if 'event' in reply:continue
     if 'error' in reply:raise RuntimeError(reply)
     return reply['return']
   command('qmp_capabilities')
   while time.monotonic()<deadline:
    command('pmemsave',{'val':0x40000100,'size':4,'filename':str(dump)})
    if dump.read_bytes()==struct.pack('<I',42):break
    time.sleep(.05)
   else:raise RuntimeError('TCG guest did not store expected word42')
   command('quit');process.wait(timeout=10);assert process.returncode==0
   print('PASS: actual shared QEMU ARM TCG MOV/MOV/STR guest produced RAM word42; fresh worker clean exit')
  except BaseException:
   print('engine exit:',process.poll(),flush=True)
   print((root/'engine.log').read_text(),flush=True)
   raise
  finally:
   if process.poll() is None:process.kill();process.wait()
