#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build/check every Pixel stage from one identity, before any VM launch."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import secrets


def option(argv, key):
    if argv.count(key) != 1 or argv.index(key)+1 >= len(argv):
        raise ValueError('Missing/duplicate stage option: '+key)
    return argv[argv.index(key)+1]


def extra(argv, key):
    pairs=[argv[i+2] for i in range(len(argv)-2) if argv[i:i+2]==['-e',key]]
    if len(pairs)!=1: raise ValueError('Missing/duplicate instrumentation identity: '+key)
    return pairs[0]


def graphics_provider(value):
    if value not in ('platform', 'angle-gl'):
        raise ValueError('pixelGraphicsProvider must be platform or angle-gl')
    return value


def instrumentation(p):
    main=next(a for a in p['assets'] if Path(a['path']).name=='app-debug.apk')
    return ['shell','am','instrument','-w','-r','--no-restart','-e','class','org.lighttouch.android.GuestPixelBootDiagnosisTest',
        '-e','diagnoseVirtualPixel','true','-e','expectedMainApkSha256',main['sha256'],'-e','recordId',p['recordId'],
        '-e','pixelAttemptToken',p['attemptToken'],'-e','pixelOriginToken','ACTUAL_ORIGINAL_CLOCK_SHA256',
        '-e','pixelGraphicsProvider',graphics_provider(p['pixelGraphicsProvider']),
        '-e','pixelColdRecovery',str(p['coldRecovery']).lower(),'-e','pixelObserveSeconds','300','-e','allowOneSleepWake','true',
        'org.lighttouch.android.test/androidx.test.runner.AndroidJUnitRunner']


def build(seed, prepared, attempt, source, launcher, cold_recovery=False, token=None, pixel_graphics_provider='platform'):
    p=copy.deepcopy(seed);prepared=Path(prepared).resolve();source=Path(source).resolve()
    p.update(schema=2,attempt=str(Path(attempt).resolve()),attemptToken=token or secrets.token_hex(16),
        controller=str(prepared/'controller.py'),coldRecovery=cold_recovery,priorDirtyExpected=cold_recovery,
        sourceRoot=str(source),status='SOURCE_PREPARED_ROOT_REVIEW_GRANT_REQUIRED',launchReady=False)
    # Each new plan makes a fresh explicit choice; never inherit a historical ANGLE default.
    p['pixelGraphicsProvider']=graphics_provider(pixel_graphics_provider)
    old=seed['supervisorArgv'];main=next(a for a in p['assets'] if Path(a['path']).name=='app-debug.apk')
    p['supervisorArgv']=['python3','-u',str(source/'pixel_supervisor.py'),'--allocation-granted','--user',option(old,'--user'),
        '--adb',option(old,'--adb'),'--emulator',str(Path(launcher).resolve()),'--avd',p['avd'],'--expected-page-bytes','16384',
        '--cores','4','--pixel-memory-mb',option(old,'--pixel-memory-mb'),'--lock-sha256',p['recordPins']['base/device.lock.json'],
        '--rom-sha256',p['recordPins']['assets/bootrom.bin'],'--guest-sha256',p['recordPins']['assets/guest.itpack'],'--output',p['attempt']]
    p['supervisorArgv'] += ['--fresh-library'] if cold_recovery else ['--record-id',p['recordId'],'--record-sha256',p['recordPins']['record.json']]
    if p.get('persistentOwnerReceipt'):
        owner=Path(p['persistentOwnerReceipt']).resolve();p['persistentOwnerReceipt']=str(owner)
        p['persistentOwnerSha256']=hashlib.sha256(owner.read_bytes()).hexdigest()
        p['supervisorArgv'] += ['--attach-avd-owner',str(owner)]
    prefix=['python3',p['controller'],'--attempt',p['attempt']]
    p['preflightCommands']=[prefix+[stage] for stage in ('prepare-ui','bind-existing','diagnose')]
    p['approvalCommand']=prefix+['approve-ui','--image','ACTUAL_CAPTURE_BASENAME_OR_SAME_ATTEMPT_ABSOLUTE_PATH','--sha256','ACTUAL_MODEL_INSPECTED_SHA256']
    p['clockCaptureCommand']=['python3',str(source/'capture_origin.py'),'--attempt',p['attempt'],'--supervisor-pid','ACTUAL_SUPERVISOR_PID','--exec-session','ACTUAL_EXEC_SESSION','--avd',p['avd'],'--attach-exec-session']
    p['afterStartAdmissionCommand']=['python3',str(source/'admit_observation.py'),'--adb',option(old,'--adb'),'--attempt',p['attempt'],'--token',p['attemptToken']]
    p['instrumentationArgs']=instrumentation(p)
    manifest=str(prepared/'launch-command.json')
    p['launchCheckedCommand']=['python3',str(source/'plan_manifest.py'),'--manifest',manifest,'--launch']
    p['readyWatcherCommand']=['python3','-u',str(source/'admit_ready.py'),'--manifest',manifest]
    p['boundsSeconds']={'ui':240,'readback':810,'stop':840,'hard':900};p['observationSeconds']=300
    p['sourceHashes']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in source.glob('*.py')}
    p['supervisorSha256']=p['sourceHashes']['pixel_supervisor.py']
    for field,path in (('controllerSha256',prepared/'controller.py'),('newLauncherSha256',Path(launcher))):
        if path.is_file():p[field]=hashlib.sha256(path.read_bytes()).hexdigest()
    validate(p)
    return p


def validate(p, files=False):
    graphics_provider(p.get('pixelGraphicsProvider'))
    for key,n in (('attemptToken',32),('recordId',32)):
        if not isinstance(p.get(key),str) or not re.fullmatch('[0-9a-f]{'+str(n)+'}',p[key]): raise ValueError('Invalid '+key)
    if type(p.get('coldRecovery')) is not bool or p.get('priorDirtyExpected') is not p['coldRecovery']:raise ValueError('Start mode/actual prior-dirty expectation inconsistent')
    if p.get('boundsSeconds')!={'ui':240,'readback':810,'stop':840,'hard':900} or p.get('observationSeconds')!=300:raise ValueError('Original acceptance budgets changed')
    attempt=Path(p['attempt'])
    if not attempt.is_absolute() or any((d/'.git').exists() for d in (attempt,*attempt.parents)):raise ValueError('Private absolute output required')
    if p.get('instrumentationArgs')!=instrumentation(p):raise ValueError('Unexpected instrumentation arguments/mode/token')
    argv=p['supervisorArgv']
    for key,expected in (('--output',p['attempt']),('--avd',p['avd']),('--expected-page-bytes','16384'),
            ('--lock-sha256',p['recordPins']['base/device.lock.json']),('--rom-sha256',p['recordPins']['assets/bootrom.bin']),('--guest-sha256',p['recordPins']['assets/guest.itpack'])):
        if option(argv,key)!=expected:raise ValueError('Supervisor identity differs: '+key)
    if p['coldRecovery']:
        if argv.count('--fresh-library')!=1 or '--record-id' in argv:raise ValueError('Explicit dirty service-only mode required')
    elif '--fresh-library' in argv or option(argv,'--record-id')!=p['recordId'] or option(argv,'--record-sha256')!=p['recordPins']['record.json']:
        raise ValueError('Normal clean Start must use actual pinned record gate')
    if 'persistentOwnerReceipt' in p:
        owner=Path(p['persistentOwnerReceipt'])
        if not owner.is_absolute() or any((x/'.git').exists() for x in (owner,*owner.parents)):
            raise ValueError('Persistent owner must be a private absolute receipt')
        if option(argv,'--attach-avd-owner')!=str(owner) or not re.fullmatch('[0-9a-f]{64}',p.get('persistentOwnerSha256','')):
            raise ValueError('Persistent attachment identity differs')
    elif any(x=='--attach-avd-owner' or x.startswith('--attach-avd-owner=') for x in argv):
        raise ValueError('Undeclared persistent attachment')
    manifest=str(Path(p['controller']).parent/'launch-command.json')
    if p['launchCheckedCommand']!=['python3',str(Path(p['sourceRoot'])/'plan_manifest.py'),'--manifest',manifest,'--launch']:raise ValueError('Unchecked VM entrypoint')
    if p['readyWatcherCommand']!=['python3','-u',str(Path(p['sourceRoot'])/'admit_ready.py'),'--manifest',manifest]:raise ValueError('Readiness watcher manifest differs')
    expected_preflight=[['python3',p['controller'],'--attempt',p['attempt'],stage] for stage in ('prepare-ui','bind-existing','diagnose')]
    if p['preflightCommands'] != expected_preflight:raise ValueError('Unexpected stage script/action/arguments')
    expected_admit=['python3',str(Path(p['sourceRoot'])/'admit_observation.py'),'--adb',option(argv,'--adb'),'--attempt',p['attempt'],'--token',p['attemptToken']]
    if p['afterStartAdmissionCommand'] != expected_admit:raise ValueError('Admission stage identity/token differs')
    if argv[0:3] != ['python3','-u',str(Path(p['sourceRoot'])/'pixel_supervisor.py')]:raise ValueError('Unexpected supervisor executable')
    commands=p['preflightCommands']+[p['approvalCommand'],p['clockCaptureCommand'],p['afterStartAdmissionCommand']]
    for cmd in commands:
        if option(cmd,'--attempt')!=p['attempt']:raise ValueError('Stage output differs')
    if p['clockCaptureCommand'][:2]!=['python3',str(Path(p['sourceRoot'])/'capture_origin.py')]:raise ValueError('Clock executable differs')
    if p['approvalCommand'][:5]!=['python3',p['controller'],'--attempt',p['attempt'],'approve-ui']:raise ValueError('Approval action differs')
    if option(p['clockCaptureCommand'],'--avd')!=p['avd']:raise ValueError('Clock AVD differs')
    if p['clockCaptureCommand'].count('--attach-exec-session')!=1:raise ValueError('Original clock must be captured before AVD, then tool session attached separately')
    if option(p['afterStartAdmissionCommand'],'--token')!=p['attemptToken']:raise ValueError('Stale admission token')
    main=next(a for a in p['assets'] if Path(a['path']).name=='app-debug.apk')
    for key,expected in (('pixelAttemptToken',p['attemptToken']),('recordId',p['recordId']),('pixelColdRecovery',str(p['coldRecovery']).lower()),
                         ('pixelObserveSeconds','300'),('pixelGraphicsProvider',p['pixelGraphicsProvider']),('expectedMainApkSha256',main['sha256']),('pixelOriginToken','ACTUAL_ORIGINAL_CLOCK_SHA256')):
        if extra(p['instrumentationArgs'],key)!=expected:raise ValueError('Instrumentation identity differs: '+key)
    required={'pixel_supervisor.py','capture_origin.py','admit_observation.py','admit_ready.py','plan_manifest.py','bind_recovery.py','memory_monitor.py','ui_approval.py','diagnostic_control.py','host_lifecycle.py','persistent_avd.py'}
    if not required.issubset(p['sourceHashes']):raise ValueError('Required stage/helper source hashes unavailable')
    hashes=list(p['recordPins'].values())+list(p['sourceHashes'].values())+[a['sha256'] for a in p['assets']]+[p['controllerSha256'],p['newLauncherSha256']]
    if any(not re.fullmatch('[0-9a-f]{64}',h) for h in hashes):raise ValueError('Invalid frozen hash')
    if not p['coldRecovery']:
        evidence=p.get('priorCleanStopReceipt')
        if not isinstance(evidence,dict) or not re.fullmatch('[0-9a-f]{64}',evidence.get('sha256','')):
            raise ValueError('Normal next Start requires disclosed genuine prior clean Stop')
    if files:
        if 'persistentOwnerReceipt' in p:
            owner=Path(p['persistentOwnerReceipt'])
            if hashlib.sha256(owner.read_bytes()).hexdigest()!=p['persistentOwnerSha256']:raise ValueError('Persistent owner receipt hash changed')
            from persistent_avd import load_owner
            load_owner(owner,p['avd'],option(argv,'--adb'))
        if not p['coldRecovery']:
            evidence=p['priorCleanStopReceipt'];path=Path(evidence['path'])
            if hashlib.sha256(path.read_bytes()).hexdigest()!=evidence['sha256']:raise ValueError('Prior clean Stop receipt hash changed')
            receipt=json.loads(path.read_text())
            if receipt.get('nativePollSnapshotValid') is not True or receipt.get('cleanShutdownConfirmed') is not True or receipt.get('shutdownConfirmationInferred') is not False or receipt.get('nativeStateDirty') is not False or receipt.get('nativeStorageFailed') is not False or receipt.get('nativeEngineExit')!=0:
                raise ValueError('Prior clean Stop receipt does not confirm native clean terminal')
        checks=[(Path(a['path']),a['sha256']) for a in p['assets']]
        checks += [(Path(p['sourceRoot'])/name,h) for name,h in p['sourceHashes'].items()]
        checks += [(Path(p['controller']),p['controllerSha256']),(Path(option(argv,'--emulator')),p['newLauncherSha256'])]
        for path,expected in checks:
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=expected:raise ValueError('Actual prelaunch file hash differs: '+str(path))
    return True


def client_budget_seconds(origin_ns, now_ns):
    # Observation fits original790; reserve10s finish and10s scoped readback by810.
    remaining=(origin_ns+800_000_000_000-now_ns)/1e9
    if remaining<=0: raise TimeoutError('Original client/readback budget expired')
    return remaining


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--manifest',required=True);parser.add_argument('--launch',action='store_true');a=parser.parse_args()
    path=Path(a.manifest);p=json.loads(path.read_text());validate(p,files=True)
    if a.launch:
        grant=json.loads((path.parent/'ROOT-LAUNCH-GRANT.json').read_text())
        if grant.get('owner')!='/root/pixel' or grant.get('oneBoundedAttempt') is not True or grant.get('exclusiveAvdGuiHeavy') is not True:raise ValueError('Root single-attempt allocation unavailable')
        with (path.parent/'launch-validation-consumed.json').open('x') as stream:json.dump({'manifestSha256':hashlib.sha256(path.read_bytes()).hexdigest(),'allStageIdentitiesAndActualHashesVerified':True,'guestAcceptancePassed':False},stream)
        argv=p['supervisorArgv'];os.execvp(argv[0],argv)
    print('All stage identities and actual hashes verified; no VM launched.')
