#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read and bind an existing selected dirty record; no import, Start or state writes."""
import argparse
import hashlib
import json
import re
import shlex
import subprocess
import time
from pathlib import Path
from diagnostic_control import Capture, validate_binding, PACKAGE, SERIAL
from memory_monitor import Sampler, process_ticks, worker_identity
from plan_manifest import graphics_provider


def normal_marker_absence(capture, root, output):
    """Require a positive scoped status; exec-out may merge remote diagnostics."""
    if not re.fullmatch(r'files/library/records/[0-9a-f]{32}', root):
        raise ValueError('Invalid record presence scope')
    state=root+'/state';path=state+'/.dirty'
    checks=''
    parts=state.split('/')
    for i in range(1,len(parts)+1):
        checks += f"if [ -L {shlex.quote('/'.join(parts[:i]))} ]; then printf 'LT_MARKER_UNKNOWN\\n'; exit 0; fi; "
    script=(checks+f"if [ ! -d {state} ] || [ ! -x {state} ]; then printf 'LT_MARKER_UNKNOWN\\n'; "
        f"elif [ -e {path} ] || [ -L {path} ]; then printf 'LT_MARKER_PRESENT\\n'; "
        "else printf 'LT_MARKER_ABSENT\\n'; fi")
    argv=[capture.adb,'-s',SERIAL,'exec-out',shlex.join(['run-as',PACKAGE,'sh','-c',script])]
    before=capture.now()
    if before>=capture.deadline:raise TimeoutError('Original preflight deadline expired')
    try:
        result=capture.run(argv,capture_output=True,timeout=min(1.5,(capture.deadline-before)/1e9))
    except subprocess.TimeoutExpired as error:
        result=subprocess.CompletedProcess(argv,124,error.stdout or b'',error.stderr or b'')
    ended=capture.now()
    receipt={'argv':argv,'beginNs':before,'endNs':ended,'adbExit':result.returncode,
        'stdout':result.stdout.decode(errors='replace'),'stderr':result.stderr.decode(errors='replace'),
        'confirmedAbsent':ended<=capture.deadline and result.returncode==0 and result.stdout==b'LT_MARKER_ABSENT\n' and not result.stderr,
        'guestAcceptancePassed':False}
    with (Path(output)/'normal-marker-presence.json').open('x') as stream:
        stream.write(json.dumps(receipt,indent=2)+'\n')
    if not receipt['confirmedAbsent']:
        raise ValueError('Normal Start requires positive scoped marker absence; denied/unknown is unavailable')
    return receipt


def match_ready(prior, ready):
    validate_binding(prior)
    if prior.get('schema') != 1 or type(prior.get('priorDirty')) is not bool or prior.get('coldRecoveryUsed') is not prior['priorDirty'] or prior.get('pinsVerified') is not True:
        raise ValueError('Explicit actual dirty recovery binding required')
    for key in ('appUid', 'workerPid', 'workerStartTicks', 'bootId', 'recordId', 'attemptToken', 'originToken'):
        if ready.get(key) != prior[key]:
            raise ValueError('Recovery Start changed original bound identity: ' + key)
    if ready.get('priorDirty') is not prior['priorDirty'] or ready.get('coldRecoveryUsed') is not prior['coldRecoveryUsed']:
        raise ValueError('Ready did not use declared dirty recovery')
    chosen=graphics_provider(prior.get('graphicsProviderRequested'))
    if ready.get('graphicsProviderRequested')!=chosen or ready.get('graphicsProviderObserved')!=chosen or ready.get('graphicsProviderFixed') is not True:
        raise ValueError('Actual fixed graphics provider differs from pinned preflight; fallback is forbidden')


def recheck_before_start(controller, binding):
    """Read actual proc bytes immediately before Start; NUL padding is normal."""
    validate_binding(binding)
    for path, expected in (("/proc/sys/kernel/random/boot_id", binding["bootId"]),
                           ("files/library/selected", binding["recordId"])):
        data = controller.run("pre-start-"+path.rsplit("/",1)[-1],
            ["exec-out","run-as",PACKAGE,"cat",path],1,490)
        if data.strip() != expected.encode():
            raise ValueError("Original pre-Start boot/selection changed")
    data = {}
    for name in ("stat", "status", "cmdline", "stat"):
        data[name] = controller.run("pre-start-"+name,
            ["exec-out","run-as",PACKAGE,"cat",f"/proc/{binding['workerPid']}/{name}"],1,490)
        if name == "stat" and "firstStat" not in data:
            data["firstStat"] = data[name]
    # Never compare cmdline with a guessed argv-buffer length or strip whitespace.
    for stat in (data["firstStat"], data["stat"]):
        if not worker_identity(stat.decode(), data["status"].decode(), data["cmdline"].decode(), binding):
            raise ValueError("Original pre-Start worker PID/start/UID/role changed")
    return {"workerPid": binding["workerPid"], "workerStartTicks": binding["workerStartTicks"],
            "cmdlineBytes": len(data["cmdline"]), "identityMatched": True, "guestAcceptancePassed": False}


def bind(adb, attempt, token, record, pins, run=subprocess.run, now=time.monotonic_ns, cold_recovery=True, pixel_graphics_provider='platform'):
    chosen_provider=graphics_provider(pixel_graphics_provider)
    out = Path(attempt)
    context_raw = (out/'clock-context.json').read_bytes(); context=json.loads(context_raw)
    monitor=json.loads((out/'memory-monitor.json').read_text())
    origin=context['originNs']
    if context['supervisorStartTicks'] * 1_000_000_000 // context['clockTicksPerSecond'] != origin:
        raise ValueError('Original process clock was reset/backdated')
    if monitor['originNs'] != origin or process_ticks(context['supervisorPid']) != context['supervisorStartTicks']:
        raise ValueError('Original supervisor/monitor identity differs')
    if not re.fullmatch('[0-9a-f]{32}', record) or not re.fullmatch('[0-9a-f]{32}', token):
        raise ValueError('Declared record/token required')
    if set(pins) != {'record.json','base/device.lock.json','assets/bootrom.bin','assets/guest.itpack'} or any(not re.fullmatch('[0-9a-f]{64}', p) for p in pins.values()):
        raise ValueError('All record/base/bootrom/guest pins required')
    deadline=min(now()+10_000_000_000, origin+490_000_000_000)
    if deadline <= now(): raise TimeoutError('Original pre-Start budget expired')
    sampler=Sampler(adb, monitor['ownerPid'], monitor['ownerStartTicks'], deadline, run=run, now=now)
    def first(path, app=False):
        result=sampler.remote(path, app)
        if not result['available']: raise ValueError('Identity read unavailable: '+path)
        return result['text']
    uid_text=first('/proc/self/status', True)
    uid=re.search(r'^Uid:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)$', uid_text, re.M)
    if not uid or len(set(uid.groups()))!=1 or int(uid[1])<=10000: raise ValueError('App UID unavailable')
    if process_ticks(monitor['ownerPid'])!=monitor['ownerStartTicks']: raise ValueError('Owned AVD gone/reused')
    command=[adb,'-s',SERIAL,'exec-out','pidof',PACKAGE+':engine']
    result=run(command,capture_output=True,timeout=min(.75,(deadline-now())/1e9))
    if result.returncode or not re.fullmatch(rb'[0-9]+\s*',result.stdout): raise ValueError('Exactly one original worker required')
    pid=int(result.stdout)
    stat=first(f'/proc/{pid}/stat',True)
    binding={'schema':1,'appUid':int(uid[1]),'workerPid':pid,'workerStartTicks':int(stat.rsplit(')',1)[1].split()[19]),
        'bootId':first('/proc/sys/kernel/random/boot_id',True).strip(),'recordId':record,'attemptToken':token,
        'originNs':origin,'originToken':hashlib.sha256(context_raw).hexdigest()}
    validate_binding(binding)
    attached=out/'attached-owner.json'
    if attached.exists():
        owner=json.loads(attached.read_text())
        if owner['originToken']!=hashlib.sha256(context_raw).hexdigest() or owner['owner']['bootId']!=binding['bootId']:
            raise ValueError('Persistent owner/original guest clock/boot differs')
        from persistent_avd import load_owner
        load_owner(owner['ownerReceipt'],context['avd'],adb,run=run)
    capture=Capture(adb,binding,out/'recovery-preflight',now=now,run=run)
    capture.deadline=deadline
    def read(path, cap=4*1024*1024):
        data, receipt=capture.read(path,cap)
        if receipt['adbExit'] or receipt['remoteExit']!=0 or receipt['truncated']: raise ValueError('Actual scoped record read failed: '+path)
        return data
    root='files/library/records/'+record
    if read('files/library/selected').strip()!=record.encode(): raise ValueError('Actual record not selected')
    actual={}
    for suffix,pin in pins.items():
        data=read(root+'/'+suffix)
        if hashlib.sha256(data).hexdigest()!=pin: raise ValueError('Actual record pin differs: '+suffix)
        actual[suffix]={'sha256':pin,'bytes':len(data)}
        if suffix=='record.json': metadata=json.loads(data)
    if metadata.get('schema')!=1 or metadata.get('id')!=record or metadata.get('base_sha256')!=pins['base/device.lock.json'] or metadata.get('bootrom_sha256')!=pins['assets/bootrom.bin'] or metadata.get('guest_package_sha256')!=pins['assets/guest.itpack']:
        raise ValueError('Record metadata pins differ')
    if type(cold_recovery) is not bool: raise ValueError('Explicit boolean Start mode required')
    if cold_recovery:
        dirty=read(root+'/state/.dirty',65536)  # Successful regular-file read; never deletes it.
        dirty_receipt={'bytes':len(dirty),'sha256':hashlib.sha256(dirty).hexdigest(),'remoteReadSucceeded':True}
    else:
        read(root+'/state/.base-identity',65536)  # Prove actual traversable initialized state.
        dirty_receipt=normal_marker_absence(capture,root,out)

    status=read(f'/proc/{pid}/status').decode()
    role=read(f'/proc/{pid}/cmdline').decode()
    after_stat=read(f'/proc/{pid}/stat').decode()
    if not worker_identity(after_stat,status,role,binding): raise ValueError('Original worker identity changed')
    if read('/proc/sys/kernel/random/boot_id').strip()!=binding['bootId'].encode() or read('files/library/selected').strip()!=record.encode(): raise ValueError('Boot/selection changed during preflight')
    from admit_observation import public_device_clock
    clock_probe=public_device_clock(adb,binding,out/'pre-start-device-clock.json',deadline,now=now,run=run)
    binding.update(priorDirty=cold_recovery,coldRecoveryUsed=cold_recovery,pinsVerified=True,pins=actual,
        graphicsProviderRequested=chosen_provider,
        dirtyReceipt=dirty_receipt,
        preStartClockProbe=clock_probe,
        beforeStart=True,guestAcceptancePassed=False,cleanInitialAdmissionClaimed=False,
        captureCommands=capture.commands,completedNs=now())
    with (out/('recovery-record-bound.json' if cold_recovery else 'normal-record-bound.json')).open('x') as stream: stream.write(json.dumps(binding,indent=2)+'\n')
    return binding


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for flag in ('adb','attempt','token','record','record-sha256','lock-sha256','rom-sha256','guest-sha256'):p.add_argument('--'+flag,required=True)
    p.add_argument('--cold-recovery',choices=('true','false'),default='true')
    p.add_argument('--pixel-graphics-provider',choices=('platform','angle-gl'),default='platform')
    a=p.parse_args()
    pins={'record.json':a.record_sha256,'base/device.lock.json':a.lock_sha256,'assets/bootrom.bin':a.rom_sha256,'assets/guest.itpack':a.guest_sha256}
    print(json.dumps(bind(a.adb,a.attempt,a.token,a.record,pins,cold_recovery=a.cold_recovery=='true',pixel_graphics_provider=a.pixel_graphics_provider),indent=2))
