#!/usr/bin/env python3
"""Validate an offline engine bundle, then relink its recorded object inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate(bundle, expected=None):
    bundle = Path(bundle).resolve()
    receipt = bundle / 'bundle.json'
    if expected and digest(receipt) != expected:
        raise RuntimeError('Bundle receipt differs from supplied SHA256')
    metadata = json.loads(receipt.read_text())
    if metadata['schema'] != 1:
        raise RuntimeError('Unsupported bundle schema')
    actual = set()
    for path in bundle.rglob('*'):
        if path.is_symlink():
            raise RuntimeError('Symlink in immutable bundle')
        if path.is_file():
            actual.add(path.relative_to(bundle).as_posix())
    expected_files = {'bundle.json'} | set(metadata['files'])
    if actual != expected_files:
        raise RuntimeError('Missing or unexpected bundle files')
    for name, expected_sha in metadata['files'].items():
        path = Path(name)
        if path.is_absolute() or '..' in path.parts:
            raise RuntimeError('Unsafe bundle member path')
        if digest(bundle / path) != expected_sha:
            raise RuntimeError('Bundle file checksum mismatch: ' + name)
    for name, mode in metadata['source_modes'].items():
        if (bundle / name).stat().st_mode & 0o777 != mode:
            raise RuntimeError('Source executable mode differs: ' + name)
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('--manifest-sha256')
    parser.add_argument('--ndk', type=Path)
    parser.add_argument('--output', type=Path, help='fresh .so path outside the immutable bundle')
    parser.add_argument('--replace', action='append', default=[], metavar='ARCHIVE=PATH',
                        help='substitute a rebuilt LGPL archive, e.g. libavcodec.a=/path/libavcodec.a')
    args = parser.parse_args()
    bundle = args.bundle.resolve()
    metadata = validate(bundle, args.manifest_sha256)
    if not args.output:
        print(json.dumps({'status': 'VALID', 'manifest_sha256': digest(bundle / 'bundle.json'),
                          'source_revision': metadata['source_revision'],
                          'engine_sha256': metadata['engine_sha256']}, sort_keys=True))
        return
    if not args.ndk:
        parser.error('--ndk is required for relinking a Bionic bundle')
    ndk = args.ndk.resolve()
    output = args.output.absolute()
    if output.is_relative_to(bundle) or output.exists():
        parser.error('--output must be fresh and outside the immutable bundle')
    recipe = metadata['relink']
    if digest(ndk / 'source.properties') != recipe['ndk_properties_sha256']:
        parser.error('NDK properties differ from the recorded engine')
    compiler = ndk / recipe['compiler']
    if digest(compiler) != recipe['compiler_sha256']:
        parser.error('Compiler differs from the recorded engine')
    for name, expected in recipe['ndk_inputs'].items():
        if digest(ndk / name) != expected:
            parser.error('NDK input differs: ' + name)
    replacements = {}
    for request in args.replace:
        name, separator, path = request.partition('=')
        if not separator or name not in recipe['replaceable_archives'] or name in replacements:
            parser.error('Replacement must name one selected LGPL archive exactly once')
        replacement = Path(path).resolve()
        if not replacement.is_file():
            parser.error('Replacement archive does not exist: ' + name)
        replacements['@BUNDLE@/' + recipe['replaceable_archives'][name]] = str(replacement)
    argv = []
    for arg in recipe['argv']:
        arg = replacements.get(arg, arg)
        arg = arg.replace('@BUNDLE@', str(bundle)).replace('@NDK@', str(ndk)).replace('@OUTPUT@', str(output))
        argv.append(arg)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Explicit argv only; no shell, Ninja, configure or guest execution.
    result = subprocess.run(argv, cwd=bundle)
    if result.returncode:
        raise RuntimeError('Relink driver failed with exit code ' + str(result.returncode))
    runtimes = {}
    for name, item in recipe['runtime_files'].items():
        source = ndk / item['ndk_path']
        if digest(source) != item['sha256']:
            parser.error('NDK runtime differs from the recorded engine: ' + name)
        target = output.parent / name
        if target.exists() and digest(target) != item['sha256']:
            parser.error('Output directory contains a different runtime: ' + name)
        if not target.exists():
            shutil.copyfile(source, target)
        runtimes[name] = digest(target)
    record = {'schema': 1, 'bundle_manifest_sha256': digest(bundle / 'bundle.json'),
              'source_revision': metadata['source_revision'], 'library_sha256': digest(output),
              'original_library_sha256': metadata['engine_sha256'],
              'byte_identical_to_original': digest(output) == metadata['engine_sha256'],
              'replacements': {name: digest(path) for name, path in replacements.items()},
              'runtime_files': runtimes,
              'scope': 'actual relink result; guest execution/qualification is separate'}
    output.with_suffix('.relink.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, sort_keys=True))


if __name__ == '__main__':
    main()
