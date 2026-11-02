#!/usr/bin/env python3
"""Compare production portable crypto under sanitizers using a QEMU build's flags."""
import argparse
import json
import os
import pathlib
import shlex
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('build', type=pathlib.Path)
p.add_argument('--compile-only', action='store_true', help='Emit an unsanitized target test for the device owner')
p.add_argument('--output', type=pathlib.Path)
p.add_argument('--crypto-prefix', type=pathlib.Path)
p.add_argument('--reference-prefix', type=pathlib.Path)
a = p.parse_args()
if a.compile_only and not a.output:
    p.error('--compile-only requires --output')
src = pathlib.Path(__file__).resolve().parents[1]
build = a.build.resolve()
commands = json.loads((build / 'compile_commands.json').read_text())
aes = next(c for c in commands if c['file'].endswith('/crypto/aes.c'))
argv = shlex.split(aes['command'])
flags, skip = [], False
for arg in argv[1:]:
    if skip:
        skip = False
        continue
    if arg in ['-o', '-MF', '-MQ', '-MT']:
        skip = True
        continue
    if arg in ['-c', '-MD', '-MMD'] or arg.endswith('/crypto/aes.c'):
        continue
    flags.append(arg)
sanitize = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-fno-omit-frame-pointer', '-g']
if a.compile_only:
    sanitize = ['-Wl,-z,max-page-size=16384']
env = os.environ.copy()
env['ASAN_OPTIONS'] = 'detect_leaks=1'
env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
prefixes = [p.resolve() / 'lib/pkgconfig' for p in [a.crypto_prefix, a.reference_prefix] if p]
if prefixes:
    env['PKG_CONFIG_PATH'] = ':'.join(map(str, prefixes))
    if a.compile_only:
        env['PKG_CONFIG_LIBDIR'] = env['PKG_CONFIG_PATH']
with tempfile.TemporaryDirectory(prefix='portable-crypto-') as temp:
    obj = pathlib.Path(temp) / 'aes.o'
    subprocess.run([argv[0]] + flags + sanitize + ['-c', str(src / 'crypto/aes.c'),
                   '-o', str(obj)], cwd=build, check=True)
    pkg = shlex.split(subprocess.check_output(
        ['pkg-config', '--static', '--cflags', '--libs', 'libgcrypt', 'openssl'], text=True, env=env))
    exe = a.output.resolve() if a.output else pathlib.Path(temp) / 'check'
    exe.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([argv[0], '-std=gnu11', '-Wno-deprecated-declarations',
                    '-I' + str(src / 'include')] + sanitize +
                   [str(src / 'tests/portable-crypto.c'),
                    str(src / 'tests/portable-crypto-reference.c'), str(obj)] + pkg +
                   ['-o', str(exe)], cwd=build, check=True)
    if a.compile_only:
        print('Target vector executable (not run):', exe)
    else:
        subprocess.run([str(exe)], env=env, check=True)
