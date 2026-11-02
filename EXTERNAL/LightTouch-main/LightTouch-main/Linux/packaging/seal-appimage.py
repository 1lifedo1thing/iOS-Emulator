#!/usr/bin/env python3
"""Seal a previously audited AppDir with fixed, hash-verified AppImage tools."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import time

TOOL_SHA256 = '95cbe7cce9717fce90c484e34052ee7c7f1d7635b33c12525b4776826a7d29b6'
RUNTIME_SHA256 = '156f4bdbde9c52d01814600013e0a273f0118dc2de98975f3c8c63427ec79074'


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def available():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) * 1024
    raise RuntimeError('Missing host memory admission value')


def limits():
    resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
    resource.setrlimit(resource.RLIMIT_CPU, (300, 300))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('appdir', 'tool', 'runtime', 'output', 'evidence'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    if sha(a.tool) != TOOL_SHA256 or sha(a.runtime) != RUNTIME_SHA256:
        raise RuntimeError('AppImage tool/runtime hash mismatch')
    if a.output.exists() or a.evidence.exists():
        raise RuntimeError('Refuse to overwrite a prior package or sealing attempt')
    manifest = json.loads((a.appdir / 'runtime-manifest.json').read_text())
    for name, expected in manifest['files'].items():
        path = a.appdir / name
        if path.is_symlink() or not path.is_file() or sha(path) != expected:
            raise RuntimeError('AppDir changed after staging audit: ' + name)
    actual = {str(path.relative_to(a.appdir)) for path in a.appdir.rglob('*') if path.is_file()}
    if actual != set(manifest['files']) | {'runtime-manifest.json'}:
        raise RuntimeError('Unmanifested AppDir content')
    if available() < 6 * 1024**3:
        raise RuntimeError('Sealing needs at least6GiB available host memory')
    a.evidence.mkdir(parents=True)
    # appimagetool replaces .DirIcon in its input. Preserve the audited AppDir
    # (which may be root-owned from Docker) and mutate only this private copy.
    scratch = a.evidence / 'sealing.AppDir'
    shutil.copytree(a.appdir, scratch)
    command = [str(a.tool.resolve()), '--appimage-extract-and-run', '--no-appstream',
               '--runtime-file', str(a.runtime.resolve()), '--comp', 'zstd',
               '--mksquashfs-opt', '-processors', '--mksquashfs-opt', '2',
               '--mksquashfs-opt', '-mem', '--mksquashfs-opt', '256M',
               str(scratch.resolve()), str(a.output.resolve())]
    start = time.monotonic()
    error = None
    with (a.evidence / 'seal.log').open('w') as log:
        child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True, preexec_fn=limits,
                                 env=dict(os.environ, ARCH='x86_64'))
        try:
            while child.poll() is None:
                if time.monotonic() - start > 300 or available() < 6 * 1024**3:
                    raise RuntimeError('Sealing exceeded original5min/host6GiB limit')
                time.sleep(0.5)
        except BaseException as failure:
            error = repr(failure)
            os.killpg(child.pid, signal.SIGTERM)
            child.wait(timeout=5)
    result = dict(schema=1, elapsed_seconds=time.monotonic() - start,
                  tool_sha256=TOOL_SHA256, runtime_sha256=RUNTIME_SHA256,
                  appdir_manifest_sha256=sha(a.appdir / 'runtime-manifest.json'),
                  command=command, exit_code=child.returncode, error=error,
                  package_sha256=sha(a.output) if a.output.is_file() else None,
                  guest_acceptance=False)
    (a.evidence / 'SEAL-RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    if error or child.returncode:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
