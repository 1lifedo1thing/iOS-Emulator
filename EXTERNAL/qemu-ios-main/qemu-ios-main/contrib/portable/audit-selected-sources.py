#!/usr/bin/env python3
"""Inventory actual linked sources/headers and immutable notices; no legal verdict."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('build', type=pathlib.Path)
p.add_argument('--output', type=pathlib.Path)
a = p.parse_args()
build = a.build.resolve()
src = pathlib.Path(__file__).resolve().parents[2]
manifest = json.loads((build / 'portable-manifest.json').read_text())
library = build / 'libqemu-arm.so'
if hashlib.sha256(library.read_bytes()).hexdigest() != manifest['library_sha256']:
    raise RuntimeError('Shared library differs from its manifest')
if manifest['source_dirty']:
    raise RuntimeError('Selected-source audit requires a clean recorded revision')
revision = manifest['source_revision']
objects = {pathlib.Path(name).resolve() for name in manifest['link_inputs']
           if name.endswith('.o')}
archive_objects = {pathlib.Path(name).resolve()
                   for members in manifest.get('archive_member_inputs', {}).values()
                   for name in members if name.endswith('.o')}
selected = {}
for entry in json.loads((build / 'compile_commands.json').read_text()):
    obj = (build / entry['output']).resolve()
    if obj in objects | archive_objects:
        selected[obj] = (pathlib.Path(entry['directory']) / entry['file']).resolve()
for name, path in [('qemu-ios-entry', src / 'contrib/ios-app/qemu-ios-entry.c'),
                   ('qemu-ios-ui', src / 'contrib/ios-app/qemu-ios-ui.c')]:
    obj = build / (name + '.o')
    if obj in objects:
        selected[obj] = path

dependencies = subprocess.check_output(['ninja', '-t', 'deps'], cwd=build, text=True)
headers = set()
archive_headers = set()
current = None
for line in dependencies.splitlines():
    if not line.startswith(' '):
        current = (build / line.split(': #deps ', 1)[0]).resolve() if ': #deps ' in line else None
    elif current in selected:
        (headers if current in objects else archive_headers).add((build / line.strip()).resolve())

def notice(path):
    try:
        relative = str(path.relative_to(src))
    except ValueError:
        return None
    expected = manifest['source_inputs'].get(relative)
    if expected is None:
        return None
    data = subprocess.check_output(['git', 'show', revision + ':' + relative], cwd=src)
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError('Source manifest differs from recorded commit: ' + relative)
    text = data.decode(errors='replace')
    spdx = re.findall(r'SPDX-License-Identifier:\s*([^\n*]+)', text)
    # Text heuristics remain review candidates, never license determinations.
    first = text[:8000]
    version_two = re.search(r'(?:only\s+)?version\s+2\s+(?:of\s+the\s+License|of\s+this\s+license)', first, re.I)
    only_text = bool(version_two and not re.search(r'(?:any\s+)?later\s+version',
                          first[version_two.start():version_two.end() + 250], re.I))
    return relative, {'sha256': expected, 'spdx': spdx,
                      'gpl2_only_text_review_candidate': only_text}

sources, archive_candidates, included, archive_includes = {}, {}, {}, {}
for obj, path in selected.items():
    item = notice(path)
    if item:
        name, info = item
        info['object'] = str(obj)
        (sources if obj in objects else archive_candidates)[name] = info
for path in sorted(headers):
    item = notice(path)
    if item:
        name, info = item
        included[name] = info
for path in sorted(archive_headers - headers):
    item = notice(path)
    if item:
        name, info = item
        archive_includes[name] = info
result = {'schema': 2, 'source_revision': revision,
          'library_sha256': manifest['library_sha256'],
          'selected_source_count': len(sources),
          'selected_include_count': len(included),
          'selected_sources': sources, 'selected_includes': included,
          'archive_source_candidates': archive_candidates,
          'archive_include_candidates': archive_includes,
          'archive_candidate_limitation': 'Thin archive members are a superset of extracted linked objects; retain notices/source for all supplied members without claiming every member entered the engine.',
          'linked_libraries': manifest.get('resolved_libraries', {}),
          'limitation': 'Source/header notice inventory only. Non-SPDX and generated sources, external archives, application closure and distribution obligations require review. No distribution eligibility claim.'}
output = a.output or build / 'selected-license-inventory.json'
output.write_text(json.dumps(result, indent=2) + '\n')
print(output)
print('Actual selected sources:', len(sources), 'tracked source/includes:', len(included))
