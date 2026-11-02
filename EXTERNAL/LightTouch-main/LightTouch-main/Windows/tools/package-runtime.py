#!/usr/bin/env python3
"""Collect and audit the Windows PE DLL closure without executing target code."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

SYSTEM = set('advapi32 avrt bcrypt bcryptprimitives cabinet cfgmgr32 comctl32 comdlg32 crypt32 d3d9 d3d11 dbghelp dnsapi dwmapi dxgi gdi32 hid imm32 iphlpapi kernel32 kernelbase mpr msvcrt mswsock ncrypt normaliz ole32 oleaut32 opengl32 powrprof psapi rpcrt4 secur32 setupapi shell32 shlwapi user32 userenv ucrtbase usp10 version winhttp wininet winmm winspool wintrust ws2_32 wtsapi32'.split())


def imports(path):
    output = subprocess.check_output(['x86_64-w64-mingw32-objdump', '-p', str(path)], text=True)
    return re.findall(r'^\s*DLL Name:\s*(\S+)\s*$', output, flags=re.M)


def package(executable, search_dirs, destination):
    destination = pathlib.Path(destination).resolve()
    if destination.exists():
        raise SystemExit('Refusing existing runtime destination')
    destination.mkdir(parents=True)
    lookup = {}
    for directory in search_dirs:
        for file in pathlib.Path(directory).glob('*.dll'):
            name = file.name.lower()
            if name in lookup and hashlib.sha256(file.read_bytes()).digest() != hashlib.sha256(lookup[name].read_bytes()).digest():
                raise SystemExit('Ambiguous DLL versions: ' + name)
            lookup[name] = file
    pending = [pathlib.Path(executable).resolve()]
    done = set()
    result = []
    while pending:
        original = pending.pop(0)
        name = original.name.lower()
        if name in done:
            continue
        if name in ('msys-2.0.dll', 'cygwin1.dll'):
            raise SystemExit('MSYS/Cygwin runtime is forbidden')
        done.add(name)
        target = destination / original.name
        shutil.copy2(original, target)
        required = imports(original)
        for dll in required:
            key = dll.lower()
            stem = key.removesuffix('.dll')
            if stem in SYSTEM or key.startswith(('api-ms-win-', 'ext-ms-win-')):
                continue
            if key not in lookup:
                raise SystemExit(f'Missing runtime dependency: {original.name} -> {dll}')
            pending.append(lookup[key])
        result.append({'file': target.name, 'bytes': target.stat().st_size, 'sha256': hashlib.sha256(target.read_bytes()).hexdigest(), 'imports': required})
    (destination / 'runtime-manifest.json').write_text(json.dumps({'schema': 1, 'target': 'Windows10-or-later-x86_64', 'execution_tested': False, 'files': result}, indent=2) + '\n')
    print(json.dumps({'status': 'PASS', 'files': len(result), 'runtime_execution_tested': False}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--engine', required=True)
    parser.add_argument('--dll-dir', action='append', required=True)
    parser.add_argument('--destination', required=True)
    args = parser.parse_args()
    package(args.engine, args.dll_dir, args.destination)
