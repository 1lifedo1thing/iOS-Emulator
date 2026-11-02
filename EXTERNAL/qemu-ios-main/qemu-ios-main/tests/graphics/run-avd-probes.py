#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run frozen probes on an already allocated emulator-5554; never launch an AVD."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--adb', required=True, type=Path)
parser.add_argument('--frozen', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--core-diagnostic', action='store_true')
parser.add_argument('--combine-only', action='store_true',
                    help='Run only the raw two-unit GLES1 combine leaf; no full renderer qualification')
parser.add_argument('--provider', choices=['platform', 'angle-gl', 'angle-vulkan'], default='platform')
args = parser.parse_args()
if args.combine_only and args.core_diagnostic:
    parser.error('--combine-only cannot be combined with --core-diagnostic')
args.output.mkdir(parents=True, exist_ok=False)
manifest = json.loads((args.frozen / 'manifest.json').read_text())
adb = [str(args.adb), '-s', 'emulator-5554']


def call(arguments, timeout=20):
    return subprocess.run(adb + arguments, capture_output=True, text=True, timeout=timeout)


probes = ['combine'] if args.combine_only else ['render', 'pixels']
if args.core_diagnostic:
    probes.insert(0, 'core-fbo')
receipt = {'serial': 'emulator-5554', 'frozen': str(args.frozen), 'binaries': {},
           'provider': args.provider, 'libraries': {},
           'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
for key, command in [('page_size', ['shell', 'getconf', 'PAGE_SIZE']),
                     ('fingerprint', ['shell', 'getprop', 'ro.build.fingerprint'])]:
    result = call(command)
    if result.returncode or not result.stdout.strip():
        raise RuntimeError(f'Missing target identity: {key}')
    receipt[key] = result.stdout.strip()
for probe in probes:
    binary_name = f'egl-{probe}-x86_64'
    source = args.frozen / 'bin' / binary_name
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if not source.stat().st_size or digest != manifest['binaries'][binary_name]:
        raise RuntimeError(f'Local frozen binary mismatch: {binary_name}')
    remote = f'/data/local/tmp/lt-egl-{probe}'
    for operation in [['push', str(source), remote], ['shell', 'chmod', '755', remote]]:
        result = call(operation)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
    result = call(['shell', 'sha256sum', remote])
    if result.returncode or not result.stdout.split() or result.stdout.split()[0] != digest:
        raise RuntimeError(f'Remote binary mismatch: {remote}: {result.stdout}{result.stderr}')
    receipt['binaries'][binary_name] = digest
if args.provider != 'platform':
    remote_libraries='/data/local/tmp/lt-egl-angle'
    result=call(['shell', 'mkdir', '-p', remote_libraries])
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    for name in ['libEGL_lighttouch_angle.so', 'libGLESv1_CM_lighttouch_angle.so',
                 'libGLESv2_lighttouch_angle.so']:
        source=args.frozen / 'lib' / name
        digest=hashlib.sha256(source.read_bytes()).hexdigest()
        if not source.stat().st_size or digest != manifest['libraries'][name]:
            raise RuntimeError(f'Local frozen provider library mismatch: {name}')
        remote=remote_libraries + '/' + name
        result=call(['push', str(source), remote])
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        result=call(['shell', 'sha256sum', remote])
        if result.returncode or not result.stdout.split() or result.stdout.split()[0] != digest:
            raise RuntimeError(f'Remote provider library mismatch: {name}')
        receipt['libraries'][name]=digest
result = call(['shell', 'sync'])
if result.returncode:
    raise RuntimeError(result.stdout + result.stderr)
(args.output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

jobs = [(probe, '') for probe in probes]
if args.core_diagnostic:
    jobs.insert(1, ('core-fbo', '--oes'))
results = []
for probe, option in jobs:
    label = probe + ('-oes' if option else '')
    command = f'/data/local/tmp/lt-egl-{probe} {option}; probe_rc=$?; echo ACTUAL_PROBE_EXIT=$probe_rc; exit "$probe_rc"'
    if args.provider != 'platform':
        command = f'LD_LIBRARY_PATH={remote_libraries} LIGHTTOUCH_GLES_PROVIDER={args.provider} LT_GLES_DRIVER_DIAGNOSTIC=1 ' + command
    try:
        result = call(['shell', command], timeout=45)
        text = result.stdout + result.stderr
        expected = {'render': 'PASS private object names, packed depth/stencil',
                    'pixels': 'PASS multirow RGB stride+alignment',
                    'core-fbo': 'PASS real GLES1 fixed-function pixel',
                    'combine': 'PASS raw GLES1 two-unit ADD independent of active state'}[probe]
        passed = result.returncode == 0 and expected in text and 'ACTUAL_PROBE_EXIT=0' in text
        if args.provider != 'platform':
            passed = passed and f'provider={args.provider}; verified public EGL/GLES dispatch' in text
        record = {'probe': label, 'adb_exit': result.returncode, 'passed': passed}
        if result.returncode == 0 and not passed:
            record['reason'] = 'Missing real probe PASS output or explicit exit marker'
        (args.output / f'{label}.log').write_text(text + f'\nADB_SHELL_EXIT={result.returncode}\n')
    except subprocess.TimeoutExpired as error:
        record = {'probe': label, 'passed': False, 'reason': '45-second transport deadline'}
        (args.output / f'{label}.log').write_bytes((error.stdout or b'') + (error.stderr or b''))
    results.append(record)
    print(json.dumps(record), flush=True)
(args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
# Experimental core failures are diagnostic. Leaf success cannot qualify full rendering.
required = {'combine'} if args.combine_only else {'render', 'pixels'}
raise SystemExit(0 if all(item['passed'] for item in results if item['probe'] in required) else 2)
