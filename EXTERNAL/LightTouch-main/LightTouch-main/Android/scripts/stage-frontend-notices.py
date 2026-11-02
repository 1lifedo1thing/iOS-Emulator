#!/usr/bin/env python3
"""Consume an upstream-owned frontend bundle bound to the selected runtime graph."""
from collections import Counter
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
from urllib.parse import urlsplit


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked_file(root, name):
    path = root
    for part in name.split('/'):
        if part in ('', '.', '..') or not re.fullmatch(r'[A-Za-z0-9._-]+', part):
            raise ValueError('Invalid frontend notice path')
        path /= part
        if path.is_symlink():
            raise ValueError('Frontend notice symlink rejected')
    if not path.is_file():
        raise ValueError('Missing declared frontend notice')
    return path


def stage(destination, bundle, manifest_sha, inventory):
    manifest_path = checked_file(bundle, 'manifest.json')
    if not re.fullmatch(r'[0-9a-f]{64}', manifest_sha) or sha(manifest_path) != manifest_sha:
        raise ValueError('Frontend notice manifest pin changed')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    reference = urlsplit(manifest['source_reference'])
    if manifest['schema'] != 1 or reference.scheme != 'https' or not reference.hostname or reference.username or reference.password:
        raise ValueError('Invalid frontend notice schema/source reference')
    if not re.fullmatch(r'[0-9a-f]{40}', manifest['source_revision']):
        raise ValueError('Invalid frontend notice source revision')
    document = json.loads(inventory.read_text(encoding='utf-8'))
    if document['schema'] != 1 or document['configuration'] != manifest['runtime_configuration']:
        raise ValueError('Frontend bundle does not describe the intended runtime variant')
    occurrences = Counter()
    for row in document['artifacts']:
        if not re.fullmatch(r'[0-9a-f]{64}', row['sha256']) or sha(Path(row['artifact_path'])) != row['sha256']:
            raise ValueError('Selected runtime input changed after inventory')
        occurrences[(row['coordinate'], row['file'], row['sha256'])] += 1
    graph = [{'coordinate': key[0], 'file': key[1], 'sha256': key[2], 'selection_occurrences': count}
             for key, count in sorted(occurrences.items())]
    encoded = (json.dumps(graph, indent=2, ensure_ascii=True, sort_keys=True) + '\n').encode()
    if hashlib.sha256(encoded).hexdigest() != manifest['selected_graph_sha256']:
        raise ValueError('Frontend notice bundle does not match the selected runtime graph')
    if manifest['missing_primary_notice_components'] or manifest['notice_byte_coverage'] != 'ALL_SELECTED_INPUTS_REPRESENTED':
        raise ValueError('Incomplete frontend notice bundle')
    if manifest['selected_artifact_rows'] != sum(occurrences.values()) or manifest['selected_unique_artifacts'] != len(occurrences):
        raise ValueError('Frontend runtime occurrence counts changed')
    files = [('manifest.json', manifest_path)]
    seen = {'manifest.json'}
    for row in manifest['files']:
        path = checked_file(bundle, row['path'])
        if row['path'] in seen or not re.fullmatch(r'[0-9a-f]{64}', row['sha256']) or sha(path) != row['sha256']:
            raise ValueError('Invalid/changed frontend notice payload')
        seen.add(row['path']); files.append((row['path'], path))
    # Verify the complete native-independent bundle before replacing old assets.
    clear(destination)
    for name, path in files:
        output = destination / name
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, output)
    print(f'Staged {len(files)} frontend files for {len(occurrences)} selected artifacts; source/relink clearance remains separate')


def clear(destination):
    if destination.is_symlink():
        raise ValueError('Unexpected generated frontend directory')
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('destination', type=Path)
    parser.add_argument('--bundle', type=Path)
    parser.add_argument('--manifest-sha256')
    parser.add_argument('--inventory', type=Path)
    args = parser.parse_args()
    inputs = [args.bundle, args.manifest_sha256, args.inventory]
    if not any(inputs):
        clear(args.destination)
    elif not all(inputs):
        parser.error('Provide frontend bundle, manifest pin and selected inventory together')
    else:
        stage(args.destination, args.bundle.resolve(strict=True), args.manifest_sha256, args.inventory)
