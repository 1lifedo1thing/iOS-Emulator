#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Emit, but never execute, small external-COW Pixel staging commands."""
import argparse
import json
from pathlib import Path
import re


def plan(source, target, avd, qemu_img, emulator, ram_mb):
    source, target = Path(source).resolve(), Path(target).resolve()
    if source == target or source in target.parents or target in source.parents:
        raise ValueError('Preserved parent and child directories must be disjoint')
    if ram_mb not in (4096, 8192) or not re.fullmatch(r'[A-Za-z0-9_]+', avd):
        raise ValueError('Explicit 4/8GiB RAM and safe AVD name required')
    for root in (source, target):
        if any((p / '.git').exists() for p in (root, *root.parents)):
            raise ValueError('Private state must be outside Git')
    names = ('userdata-qemu.img.qcow2', 'encryptionkey.img.qcow2', 'cache.img.qcow2')
    commands, metadata = [], []
    for name, child in zip(names, ('userdata-branch.img', 'encryptionkey-branch.img', 'cache-branch.img')):
        original = source / name
        if original.is_symlink() or not original.is_file():
            raise ValueError('Regular preserved active QCOW file required: ' + name)
        if (target / child).exists():
            raise ValueError('Child target already exists; never overwrite state')
        stat = original.stat()
        metadata.append({'path': str(original), 'bytes': stat.st_size, 'mtimeNs': stat.st_mtime_ns,
                         'sha256': None, 'hashPinRequiredBeforeLaunch': True})
        commands.append([str(qemu_img), 'create', '-f', 'qcow2', '-F', 'qcow2', '-b', str(original), str(target / child)])
    config = (source / 'config.ini').read_text()
    replacements = {'avd.id': avd, 'avd.name': avd, 'hw.ramSize': str(ram_mb),
                    'disk.dataPartition.path': str(target / 'userdata-branch.img'),
                    'fastboot.forceFastBoot': 'no', 'fastboot.forceColdBoot': 'yes',
                    'firstboot.bootFromDownloadableSnapshot': 'no', 'firstboot.bootFromLocalSnapshot': 'no',
                    'firstboot.saveToLocalSnapshot': 'no'}
    lines = []
    for line in config.splitlines():
        key = line.partition('=')[0]
        lines.append(key + '=' + replacements.pop(key) if key in replacements else line)
    lines.extend(key + '=' + value for key, value in replacements.items())
    extra = ['-data', str(target / 'userdata-branch.img'), '-encryption-key', str(target / 'encryptionkey-branch.img'),
             '-cache', str(target / 'cache-branch.img'), '-datadir', str(target), '-no-snapshot']
    # Private launcher lets the unchanged common supervisor retain ownership/watchdog.
    launcher = ('#!/usr/bin/env python3\nimport json,os,sys\n'
                'args=sys.argv[1:]\n'
                'assert args[args.index("-avd")+1]==' + repr(avd) + '\n'
                'assert not any(x in args for x in ("-data","-encryption-key","-cache","-datadir","-wipe-data"))\n'
                'argv=' + repr([str(emulator)]) + '+args+' + repr(extra) + '\n'
                'with open(' + repr(str(target / 'actual-emulator-argv.json')) + ',"x") as f: json.dump(argv,f)\n'
                'os.execv(argv[0],argv)\n')
    return {'schema': 1, 'executed': False, 'guestAcceptancePassed': False,
            'avd': avd, 'ramMb': ram_mb, 'parentMetadata': metadata,
            'mkdirRequired': str(target), 'createCommands': commands,
            'smallTextFiles': {'config.ini': '\n'.join(lines) + '\n', 'launch-emulator.py': launcher},
            'avdIni': {'filename': avd + '.ini', 'text': 'avd.ini.encoding=UTF-8\npath=' + str(target) + '\ntarget=android-36\n'},
            'supervisorEmulatorArgument': str(target / 'launch-emulator.py'),
            'emulatorExtraArguments': extra,
            'requiresActualPreGuestValidation': ['All parent backing-chain hashes unchanged before/after',
                'Effective data/key/cache images open as child QCOW and parents read-only',
                'Android encrypted userdata mounts, installed package identity and retained selected dirty record match',
                'No implicit import/start; fresh worker PID before explicit recoverStart'],
            'unsupportedRouteFallbackRequiresRootDecision': True}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', required=True); p.add_argument('--target', required=True)
    p.add_argument('--avd', required=True); p.add_argument('--qemu-img', required=True)
    p.add_argument('--emulator', required=True); p.add_argument('--ram-mb', type=int, choices=(4096, 8192), required=True)
    a = p.parse_args()
    print(json.dumps(plan(a.source, a.target, a.avd, a.qemu_img, a.emulator, a.ram_mb), indent=2))
