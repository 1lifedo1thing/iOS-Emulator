#!/usr/bin/env python3
"""Consume verified engine bundles without generating or broadening their notices."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil


def checked_path(root, relative):
    parts = relative.split('/')
    if not parts or any(p in ('', '.', '..') or not re.fullmatch(r'[A-Za-z0-9._-]+', p) for p in parts):
        raise ValueError(f'Invalid notice path: {relative!r}')
    result = root
    for part in parts:
        result = result / part
        if result.is_symlink():
            raise ValueError(f'Symlink in notice path: {relative}')
    if not result.is_file():
        raise ValueError(f'Notice is not a regular file: {relative}')
    return result


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def stage(destination, engine_root, bundles):
    # Verify every bundle before changing generated assets.
    verified = []
    for abi, source in bundles:
        if abi not in ('arm64-v8a', 'x86_64'):
            raise ValueError(f'Unsupported ABI: {abi}')
        root = Path(source).resolve(strict=True)
        manifest_path = checked_path(root, 'manifest.json')
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        if manifest['schema'] != 1 or manifest['library_sha256'] != sha(engine_root / abi / 'libqemu-arm.so'):
            raise ValueError(f'Notice bundle does not match {abi} engine')
        runtime = next(c for c in manifest['components'] if c['name'] == 'Android NDK runtime/toolchain notices')
        if runtime['runtime_files']['libc++_shared.so'] != sha(engine_root / abi / 'libc++_shared.so'):
            raise ValueError(f'Notice bundle does not match {abi} C++ runtime')
        files = [('manifest.json', manifest_path)]
        names = {'manifest.json'}
        for row in manifest['files']:
            name, expected = row['path'], row['sha256']
            path = checked_path(root, name)
            if name in names or not re.fullmatch(r'[0-9a-f]{64}', expected) or sha(path) != expected:
                raise ValueError(f'Invalid or changed {abi} notice: {name}')
            names.add(name)
            files.append((name, path))
        verified.append((abi, files))
    destination.mkdir(parents=True, exist_ok=True)
    for previous in destination.glob('engine-*'):
        if previous.is_symlink() or not previous.is_dir():
            raise ValueError('Unexpected generated notice destination')
        shutil.rmtree(previous)
    for abi, files in verified:
        for name, source in files:
            output = destination / f'engine-{abi}' / name
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, output)
    print(f'Staged {len(verified)} engine notice bundles; complete APK/source/relink review remains separate')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('destination', type=Path)
    parser.add_argument('--engine-root', type=Path, required=True)
    parser.add_argument('--bundle', nargs=2, action='append', default=[], metavar=('ABI', 'DIRECTORY'))
    args = parser.parse_args()
    if len({abi for abi, _ in args.bundle}) != len(args.bundle):
        parser.error('Duplicate ABI bundle')
    stage(args.destination, args.engine_root, args.bundle)
