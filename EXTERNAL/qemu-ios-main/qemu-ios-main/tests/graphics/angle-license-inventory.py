#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record retained ANGLE source pins and license metadata; not a license approval."""
import argparse
import hashlib
import json
import os
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--checkout', required=True, type=Path,
                    help='Private directory containing angle/, depot_tools/ and source-receipt.txt')
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
root = args.checkout.resolve()
receipt = root / 'source-receipt.txt'
if not receipt.is_file() or not (root / 'angle/DEPS').is_file():
    parser.error('Completed source fetch and its revision receipt are required')
if args.output.exists():
    parser.error('Use a new inventory output path')
files = []
for directory, children, names in os.walk(root):
    children[:] = [name for name in children if name not in
                   ('.git', 'out', '__pycache__', '.cipd', '.vpython-root')]
    for name in sorted(names):
        upper = name.upper()
        if not (upper.startswith(('LICENSE', 'COPYING', 'NOTICE')) or name == 'README.chromium'):
            continue
        path = Path(directory) / name
        if path.is_symlink() or not path.is_file():
            continue
        files.append({'path': str(path.relative_to(root)),
                      'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
metadata = ['.gclient', 'source-receipt.txt', 'angle/DEPS', 'angle/LICENSE']
record = {
    'description': 'Retained source/revision and license-file inventory. Build output dependency closure and distribution obligations require separate review.',
    'source_revisions': receipt.read_text(),
    'metadata_sha256': {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                       for name in metadata},
    'license_files': sorted(files, key=lambda item: item['path']),
}
args.output.write_text(json.dumps(record, indent=2) + '\n')
print(f'Recorded {len(files)} retained license/notice/README files; no distribution approval implied')
