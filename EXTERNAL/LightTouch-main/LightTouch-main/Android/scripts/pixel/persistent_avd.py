#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Persistent Android ownership; bounded guest attempts never dispose this AVD."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

PACKAGE='org.lighttouch.android'
SERIAL='emulator-5554'


def identity(pid):
    try:
        fields=Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()
        return int(fields[19]) if fields[0]!='Z' else None
    except (OSError,ValueError,IndexError):return None


def private(path):
    p=Path(path).resolve()
    if any((x/'.git').exists() for x in (p,*p.parents)):raise ValueError('Persistent state must be outside Git')
    return p


def write(path,value):
    with Path(path).open('x') as f:f.write(json.dumps(value,indent=2)+'\n')


class Handle:
    def __init__(self, receipt):
        self.receipt=receipt;self.pid=receipt['wrapperPid']
    def poll(self):return None if identity(self.pid)==self.receipt['wrapperStartTicks'] else 1


def load_owner(path, avd, adb, run=subprocess.run):
    raw=private(path).read_bytes();v=json.loads(raw)
    if v.get('schema')!=1 or v.get('avd')!=avd or v.get('serial')!=SERIAL or v.get('pageBytes')!=16384 or v.get('ramMB')!=8192:
        raise ValueError('Persistent AVD allocation differs')
    if any(type(v.get(k)) is not int or v[k]<=0 for k in ('wrapperPid','wrapperStartTicks','qemuPid','qemuStartTicks')):
        raise ValueError('Persistent AVD process identities unavailable')
    if identity(v['wrapperPid'])!=v['wrapperStartTicks'] or identity(v['qemuPid'])!=v['qemuStartTicks']:
        raise ValueError('Persistent AVD process gone/reused')
    r=run([adb,'-s',SERIAL,'exec-out','cat','/proc/sys/kernel/random/boot_id'],capture_output=True,timeout=1.5)
    if r.returncode or r.stderr or r.stdout.strip().decode()!=v['bootId']:raise ValueError('Persistent Android boot changed')
    return v,hashlib.sha256(raw).hexdigest()


def force_bound_worker(adb, out, deadline, run=subprocess.run, now=time.monotonic):
    """Last resort only: original worker identity, never AVD or replacement PID."""
    out=private(out);receipt={'guestShutdownConfirmed':False,'cleanShutdownConfirmed':False,
        'emulatorStopRequested':False,'replacementWorkerKillAdmitted':False}
    try:
        from diagnostic_control import validate_binding
        choices=[out/n for n in ('pixel-bound.json','normal-record-bound.json','recovery-record-bound.json') if (out/n).exists()]
        if not choices:raise ValueError('No bound original guest worker')
        b=validate_binding(json.loads(choices[0].read_text()));clock=(out/'clock-context.json').read_bytes()
        if b['originToken']!=hashlib.sha256(clock).hexdigest():raise ValueError('Original guest attempt clock differs')
        prefix=[adb,'-s',SERIAL,'exec-out','run-as',PACKAGE]
        def read(path):
            remaining=deadline-now()
            if remaining<=0:raise TimeoutError('Original guest deadline expired')
            r=run(prefix+['cat',path],capture_output=True,timeout=min(.75,remaining))
            if r.returncode or r.stderr:raise ValueError('Original guest identity unavailable')
            return r.stdout
        if read('/proc/sys/kernel/random/boot_id').strip().decode()!=b['bootId'] or read('files/library/selected').strip().decode()!=b['recordId']:
            raise ValueError('Original guest boot/record changed')
        from memory_monitor import worker_identity
        stat=read(f"/proc/{b['workerPid']}/stat").decode()
        status=read(f"/proc/{b['workerPid']}/status").decode()
        role=read(f"/proc/{b['workerPid']}/cmdline").decode()
        if not worker_identity(stat,status,role,b):raise ValueError('Original worker gone/replaced')
        remaining=deadline-now()
        if remaining<=0:raise TimeoutError('Original guest deadline expired')
        r=run(prefix+['kill','-9',str(b['workerPid'])],capture_output=True,timeout=min(.75,remaining))
        receipt.update(workerPid=b['workerPid'],workerStartTicks=b['workerStartTicks'],
            originalWorkerKillRequested=True,killExit=r.returncode,stdout=r.stdout.decode(errors='replace'),stderr=r.stderr.decode(errors='replace'))
    except Exception as error:receipt.update(error=str(error),originalWorkerKillRequested=False)
    return receipt


def guest_watchdog(adb,out,deadline,complete):
    # Independent guest hard bound; there is deliberately no AVD root/PID here.
    while not complete.wait(.2):
        if time.monotonic()>=deadline-8:
            result=force_bound_worker(adb,out,deadline)
            result['hardGuestDeadlineFallback']=True
            write(Path(out)/'guest-hard-deadline.json',result)
            return


def attach_attempt(attempt,path):
    owner,pin=load_owner(path,attempt.args.avd,attempt.args.adb)
    attempt.avd=Handle(owner);attempt.attached_owner=owner
    # Identity for ordinary guest Stop only; Android ownership stays external.
    attempt.avd_start_ticks=owner['wrapperStartTicks']
    clock=(attempt.output/'clock-context.json').read_bytes()
    attempt.write('attached-owner.json',{'ownerReceipt':str(private(path)),'ownerReceiptSha256':pin,
        'owner':owner,'originToken':hashlib.sha256(clock).hexdigest(),
        'normalGuestCleanupStopsAvd':False,'guestAcceptancePassed':False})
    attempt.write('launch.json',{'attachedExistingAvd':True,'wrapperPid':owner['wrapperPid'],
        'wrapperStartTicks':owner['wrapperStartTicks'],'bootId':owner['bootId'],
        'originalGuestOrigin':attempt.start,'stopAtSeconds':840,'hardLimitSeconds':900})
    # Reuse the existing Linux fork context; this watchdog owns no AVD identity.
    import multiprocessing
    attempt.watcher=multiprocessing.get_context('fork').Process(target=guest_watchdog,
        args=(attempt.args.adb,str(attempt.output),attempt.deadline,attempt.complete))
    attempt.watcher.start()
    attempt.attached_watcher_ticks=identity(attempt.watcher.pid)


def cleanup_attempt(attempt):
    """Guest-only retirement. No emulator command or signal appears here."""
    try:
        if not attempt.stop_attempted:attempt.stop()
    finally:
        try:
            result=force_bound_worker(attempt.args.adb,attempt.output,attempt.deadline)
            attempt.write('guest-retirement.json',result)
        finally:
            try:
                if attempt.logcat is not None and attempt.logcat.poll() is None:
                    attempt.logcat.terminate()
                    try:attempt.logcat.wait(timeout=.5)
                    except subprocess.TimeoutExpired:attempt.logcat.kill();attempt.logcat.wait(timeout=.5)
            finally:
                attempt.complete.set()
                if attempt.watcher is not None:
                    attempt.watcher.join(timeout=.5)
                    if attempt.watcher.is_alive() and identity(attempt.watcher.pid)==attempt.attached_watcher_ticks:
                        attempt.watcher.kill();attempt.watcher.join(timeout=.5)
                attempt.write('terminal.json',{'elapsedSeconds':time.monotonic()-attempt.start,
                    'attachedExistingAvd':True,'emulatorPreserved':attempt.avd.poll() is None,
                    'avdWrapperPid':attempt.avd.pid,'stopAttempted':attempt.stop_attempted,
                    'guestWatchdogStillAlive':attempt.watcher.is_alive() if attempt.watcher else False,
                    'guestShutdownConfirmed':False,'acceptanceRequiresIndependentReceipts':True})


def owned_qemu(wrapper):
    spec=importlib.util.spec_from_file_location('owned_base',Path(__file__).resolve().parents[1]/'supervise_16k_retry.py');base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
    owned={};base.retain_descendants(wrapper,owned);q=[]
    for pid,ticks in owned.items():
        try:
            args=Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
            if b'qemu-system-x86_64' in args[0]:q.append((pid,ticks))
        except OSError:pass
    return owned,q


def launch_owner(plan_path):
    plan_path=private(plan_path);raw=plan_path.read_bytes();p=json.loads(raw);out=private(p['output'])
    grant=json.loads((plan_path.parent/'ROOT-PERSISTENT-AVD-GRANT.json').read_text())
    if grant.get('owner')!='/root/pixel' or grant.get('persistentAvdAuthorized') is not True:
        raise ValueError('Explicit persistent AVD allocation required')
    for name,pin in p.get('sourceHashes',{}).items():
        if Path(name).name!=name or hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest()!=pin:
            raise ValueError('Persistent owner source hash differs')
    if p['avd']!='LightTouch_pixel_10_API36_16384_24_8192' or p['ramMB']!=8192 or p['pageBytes']!=16384:
        raise ValueError('Unexpected retained COW allocation')
    launcher=Path(p['launcher'])
    if hashlib.sha256(launcher.read_bytes()).hexdigest()!=p['launcherSha256']:raise ValueError('Owner launcher hash changed')
    result=subprocess.run([p['adb'],'devices'],capture_output=True,timeout=3)
    if result.returncode or any(x.split() and x.split()[0]==SERIAL.encode() for x in result.stdout.splitlines()):
        raise ValueError('Persistent serial already present or enumeration unavailable')
    out.mkdir(parents=True,exist_ok=False)
    command=['xvfb-run','-a','-s','-screen 0 1280x1024x24',str(launcher),'-avd',p['avd'],'-port','5554',
        '-no-window','-no-snapshot','-no-audio','-gpu','swiftshader','-cores','4','-memory','8192']
    with (out/'avd.log').open('wb') as log:avd=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    candidate={'wrapperPid':avd.pid,'wrapperStartTicks':identity(avd.pid),'ownerPid':os.getpid(),
        'ownerStartTicks':identity(os.getpid()),'argv':command,'planSha256':hashlib.sha256(raw).hexdigest()}
    write(out/'owner-start.json',candidate)
    # Startup bound limits readiness work, never silently shuts Android down.
    deadline=time.monotonic()+210
    owner=None
    while time.monotonic()<deadline and avd.poll() is None:
        try:
            def probe(tail):
                remaining=deadline-time.monotonic()
                if remaining<=0:raise TimeoutError('Original210 readiness budget expired')
                return subprocess.run([p['adb'],'-s',SERIAL]+tail,capture_output=True,timeout=min(2,remaining))
            boot=probe(['shell','getprop','sys.boot_completed'])
            if boot.returncode==0 and boot.stdout.strip()==b'1':
                r=probe(['exec-out','cat','/proc/sys/kernel/random/boot_id'])
                pages=probe(['shell','getconf','PAGE_SIZE'])
                if r.returncode==0 and not r.stderr and pages.returncode==0 and pages.stdout.strip()==b'16384':
                    if not re.fullmatch(rb'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}',r.stdout.strip()):
                        raise OSError('Original Android boot identity not yet readable')
                    owned,q=owned_qemu(avd.pid)
                    if len(q)!=1:raise OSError('Unique owned QEMU not yet observed')
                    if time.monotonic()>=deadline:raise TimeoutError('Readiness completed after original210')
                    owner={'schema':1,**candidate,'avd':p['avd'],'serial':SERIAL,'pageBytes':16384,'ramMB':8192,
                        'qemuPid':q[0][0],'qemuStartTicks':q[0][1],'ownedIdentities':owned,'bootId':r.stdout.strip().decode(),
                        'normalGuestCleanupStopsAvd':False,'guestAcceptancePassed':False}
                    write(out/'owner.json',owner);print(json.dumps(owner),flush=True);break
        except (OSError,subprocess.TimeoutExpired,TimeoutError) as error:
            with (out/'readiness-probe-errors.jsonl').open('a') as f:
                f.write(json.dumps({'observedAtMonotonic':time.monotonic(),'type':type(error).__name__,
                    'error':str(error),'argv':getattr(error,'cmd',None),'stdout':(getattr(error,'stdout',None) or b'').decode(errors='replace'),
                    'stderr':(getattr(error,'stderr',None) or b'').decode(errors='replace'),
                    'emulatorPreserved':avd.poll() is None,'guestAcceptancePassed':False})+'\n')
        time.sleep(min(1,max(0,deadline-time.monotonic())))
    if owner is None:write(out/'readiness-unconfirmed.json',{'startupBoundExpired':True,'emulatorPreserved':avd.poll() is None,'guestAcceptancePassed':False})
    print('Persistent Android retained; only explicit REQUEST-STOP-AVD disposes it.',flush=True)
    while avd.poll() is None:
        if (out/'REQUEST-STOP-AVD').exists():
            subprocess.run([p['adb'],'-s',SERIAL,'emu','kill'],capture_output=True,timeout=3)
            avd.wait(timeout=10)
            break
        time.sleep(.5)
    write(out/'owner-terminal.json',{'exit':avd.poll(),'explicitStopRequested':(out/'REQUEST-STOP-AVD').exists()})


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--plan',required=True)
    launch_owner(parser.parse_args().plan)
