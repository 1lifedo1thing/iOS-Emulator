#!/usr/bin/env python3
"""Build pinned static/PIC libgcrypt without changing firmware's OpenSSL prefix."""
import argparse
import json
import os
import pathlib
import subprocess
import tarfile
import urllib.request
from cache import check_cache, digest, tool_version

p = argparse.ArgumentParser()
p.add_argument('output', type=pathlib.Path)
p.add_argument('--abi', choices=['linux', 'arm64-v8a', 'x86_64'], default='linux')
p.add_argument('--ndk', type=pathlib.Path)
p.add_argument('--prepare-only', action='store_true')
a = p.parse_args()
src = pathlib.Path(__file__).resolve().parents[2]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
prefix = out / 'prefix'
env = os.environ.copy()
packages = json.loads((src / 'contrib/portable/crypto-dependencies.json').read_text())['packages']
trees = {}
for package in packages:
    archive = out / package['archive']
    if not archive.exists():
        urllib.request.urlretrieve(package['url'], archive)
    if digest(archive) != package['sha256']:
        raise RuntimeError('Checksum mismatch: ' + str(archive))
    tree = out / package['directory']
    if not tree.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(out, filter='data')
    trees[package['name']] = tree
if a.prepare_only:
    print('Verified crypto sources extracted in', out)
    raise SystemExit(0)

cc, ar, ranlib = 'cc', 'ar', 'ranlib'
host = []
if a.abi != 'linux':
    if not a.ndk:
        raise RuntimeError('--ndk required')
    bin = a.ndk.resolve() / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    triple = 'aarch64-linux-android' if a.abi == 'arm64-v8a' else 'x86_64-linux-android'
    cc = str(bin / (triple + '28-clang'))
    ar, ranlib = str(bin / 'llvm-ar'), str(bin / 'llvm-ranlib')
    env['PATH'] = str(bin) + ':/usr/bin:/bin:' + env['PATH']
    env['OBJDUMP'] = str(bin / 'llvm-objdump')
    host = ['--host=' + triple]
env.update(CC=cc, CC_FOR_BUILD='/usr/bin/cc', AR=ar, RANLIB=ranlib, CFLAGS='-fPIC',
           PKG_CONFIG_LIBDIR=str(prefix / 'lib/pkgconfig'),
           PKG_CONFIG_PATH=str(prefix / 'lib/pkgconfig'))
options = ['--prefix=' + str(prefix), '--libdir=' + str(prefix / 'lib'),
           '--disable-shared', '--enable-static', '--disable-doc'] + host
gcrypt_options = ['--with-libgpg-error-prefix=' + str(prefix), '--disable-asm']
recipe = {'schema': 1, 'abi': a.abi, 'compiler_path': cc,
          'compiler': tool_version([cc]), 'ar': tool_version([ar]),
          'ndk_properties_sha256': digest(a.ndk / 'source.properties') if a.ndk else None,
          'archives': {p['name']: p['sha256'] for p in packages},
          'recipe_sha256': digest(__file__),
          'patches': {'libgpg-error-android-lock.patch': digest(src / 'contrib/portable/libgpg-error-android-lock.patch')},
          'cache_guard_sha256': digest(src / 'contrib/portable/cache.py'),
          'configure_options': options, 'gcrypt_options': gcrypt_options,
          'cflags': env['CFLAGS'], 'library': 'static/PIC'}
check_cache(out, recipe, [prefix / 'lib/libgcrypt.a', prefix / 'lib/libgpg-error.a'] +
            [t / 'Makefile' for t in trees.values()], 'crypto-dependencies')

def run(argv, cwd):
    print(' '.join(map(str, argv)), flush=True)
    subprocess.run(list(map(str, argv)), cwd=cwd, env=env, check=True)

if a.abi != 'linux':
    tree = trees['libgpg-error']
    marker = tree / '.lighttouch-android-lock-patched'
    if not marker.exists():
        with (src / 'contrib/portable/libgpg-error-android-lock.patch').open('rb') as patch:
            subprocess.run(['patch', '-p1'], cwd=tree, stdin=patch, check=True)
        marker.touch()

for name in ['libgpg-error', 'libgcrypt']:
    tree = trees[name]
    if not (tree / 'Makefile').exists():
        run([tree / 'configure'] + options + (gcrypt_options if name == 'libgcrypt' else []), tree)
    run(['make', '-j2'], tree)
    run(['make', 'install'], tree)

libraries = {name: digest(prefix / 'lib' / name)
             for name in ['libgcrypt.a', 'libgpg-error.a']}
notices = {}
notice_dir = prefix / 'share/lighttouch-licenses'
notice_dir.mkdir(parents=True, exist_ok=True)
for name, tree in trees.items():
    for filename in ['COPYING', 'COPYING.LIB', 'LICENSES', 'AUTHORS', 'README']:
        path = tree / filename
        if path.exists():
            (notice_dir / (name + '-' + filename)).write_bytes(path.read_bytes())
            notices[name + '/' + filename] = digest(path)
(out / 'dependency-manifest.json').write_text(json.dumps(
    {'schema': 1, 'abi': a.abi, 'packages': packages, 'recipe': recipe,
     'libraries': libraries, 'notices': notices}, indent=2) + '\n')
print(prefix)
