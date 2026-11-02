# SPDX-License-Identifier: GPL-2.0-or-later
"""Refuse mutable build-cache reuse without matching recorded inputs."""
import hashlib,json,pathlib,subprocess

def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()

def tool_version(argv):
    return subprocess.check_output(list(map(str,argv))+['--version'],text=True,stderr=subprocess.STDOUT).splitlines()[0]

def check_cache(output,identity,markers,name):
    stamp=pathlib.Path(output)/(name+'-inputs.json')
    if stamp.exists():
        previous=json.loads(stamp.read_text())
        if previous!=identity:
            raise RuntimeError('Build inputs changed; use a fresh output directory. Existing stamp: '+str(stamp))
    else:
        existing=[str(p) for p in markers if pathlib.Path(p).exists()]
        if existing:
            raise RuntimeError('Unstamped compiled cache cannot be reused; use a fresh output directory. Existing: '+', '.join(existing))
        stamp.write_text(json.dumps(identity,sort_keys=True,indent=2)+'\n')
    return stamp
