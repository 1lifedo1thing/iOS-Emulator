#!/usr/bin/env python3
"""One fresh-process, pinned qemu_ios_main adapter for asset-free probes only."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re


def checked_library(path, expected):
    path = Path(path).resolve(strict=True)
    if not re.fullmatch('[a-f0-9]{64}', expected):
        raise ValueError('Exact shared engine SHA256 required')
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(65536), b''):
            value.update(block)
    if value.hexdigest() != expected:
        raise ValueError('Shared engine changed before load')
    return path


def mapping_witness(path, expected):
    info = path.stat()
    expected_dev = '%02x:%02x' % (os.major(info.st_dev), os.minor(info.st_dev))
    matches = [line for line in Path('/proc/self/maps').read_text().splitlines()
               if line.endswith(' ' + str(path))]
    if not matches or any(line.split()[3] != expected_dev or int(line.split()[4]) != info.st_ino
                          for line in matches):
        raise RuntimeError('Exact shared engine mapping absent or changed')
    return {'scope': 'EXACT_SHARED_ENGINE_MAPPED_BEFORE_ENTRY', 'path': str(path),
            'sha256': expected, 'pid': os.getpid(), 'device': expected_dev,
            'inode': info.st_ino, 'maps': matches, 'entrypoint': 'qemu_ios_main'}


def run_library(path, expected, arguments):
    path = checked_library(path, expected)
    library = ctypes.CDLL(str(path), mode=os.RTLD_NOW | os.RTLD_LOCAL)
    entry = library.qemu_ios_main
    entry.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
    entry.restype = ctypes.c_int
    # Record the loaded artifact, not Python's /proc/self/exe identity. Recheck
    # bytes after load as well; no executable probe stands for this .so.
    checked_library(path, expected)
    print('SHARED_ENGINE_MAPPING ' + json.dumps(mapping_witness(path, expected)), flush=True)
    words = [os.fsencode(path)] + [os.fsencode(value) for value in arguments]
    argv = (ctypes.c_char_p * (len(words) + 1))(*words, None)
    # Called once, synchronously. Each matrix trial starts a new Python process;
    # QMP quit drives normal native cleanup and return, with no second engine.
    return entry(len(words), argv)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
    return run_library(args.library, args.sha256, arguments)


if __name__ == '__main__':
    raise SystemExit(main())
