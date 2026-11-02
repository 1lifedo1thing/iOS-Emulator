#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Watch actual ready concurrently through original490, then admit immediately."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
from admit_observation import admit
from plan_manifest import option, validate


def watch(manifest, run=subprocess.run, now=time.monotonic_ns, sleep=time.sleep):
    p=json.loads(Path(manifest).read_text());validate(p,files=True)
    out=Path(p['attempt']);context=json.loads((out/'clock-context.json').read_text());origin=context['originNs']
    deadline=origin+490_000_000_000;token=p['attemptToken'];adb=option(p['supervisorArgv'],'--adb')
    path='files/pixel-boot-'+token+'/ready.json'
    argv=[adb,'-s','emulator-5554','exec-out','run-as','org.lighttouch.android','cat',path]
    while now()<deadline:
        if (out/'request-stop').exists() or (out/'terminal.json').exists():raise RuntimeError('Actual attempt already stopping; no late admission')
        before=now()
        try:result=run(argv,capture_output=True,timeout=min(1,(deadline-before)/1e9))
        except subprocess.TimeoutExpired as error:result=subprocess.CompletedProcess(argv,124,error.stdout or b'',error.stderr or b'')
        try:ready=json.loads(result.stdout)
        except ValueError:ready=None
        with (out/'ready-arrival-observations.jsonl').open('a') as stream:stream.write(json.dumps({'beforeNs':before,'afterNs':now(),'exit':result.returncode,'bytes':len(result.stdout),'parsed':ready is not None})+'\n')
        if result.returncode==0 and ready is not None:
            # Retain exact actual bytes BEFORE admission can fail or cleanup starts.
            (out/'actual-after-start-ready.json').write_bytes(result.stdout)
            (out/'actual-after-start-ready-sha256.txt').write_text(hashlib.sha256(result.stdout).hexdigest()+'\n')
            return admit(adb,out,token,now=now,run=run)
        sleep(.1)
    raise TimeoutError('No actual ready before original490; no restart/shortening')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--manifest',required=True);a=parser.parse_args()
    print(json.dumps(watch(a.manifest),indent=2))
