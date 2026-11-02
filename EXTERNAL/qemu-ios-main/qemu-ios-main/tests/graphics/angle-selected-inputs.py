#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record actual ANGLE link inputs and whole-archive members, not broad GN deps."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', required=True, type=Path)
parser.add_argument('--compile-commands', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--headers-output', type=Path,
                    help='Record selected Khronos headers and optional dependency counts from actual compiler dependencies')
args = parser.parse_args()
build = args.build.resolve()
angle = build.parent.parent
if args.output.exists():
    parser.error('Use a new selection report')
if args.headers_output and args.headers_output.exists():
    parser.error('Use a new compiler dependency report')
commands = json.loads(args.compile_commands.read_text())
compiled = {}
for item in commands:
    for output in item['output'].split():
        if output.endswith('.o'):
            compiled[(Path(item['directory']) / output).resolve()] = item
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
relative = lambda path: str(path.resolve().relative_to(angle))


def object_record(path):
    item = compiled.get(path.resolve())
    record = {'object': relative(path), 'sha256': sha(path)}
    if item:
        source = (Path(item['directory']) / item['file']).resolve()
        record['source'] = relative(source)
        record['source_sha256'] = sha(source)
        record['compile_command'] = item['command'].replace(str(angle), 'ANGLE_ROOT')
    else:
        record['source_status'] = 'Prebuilt or source not mapped by this compile database'
    return record


toolchain = (build / 'toolchain.ninja').read_text()
if 'rspfile_content = -Wl,--whole-archive ${in} ${solibs} -Wl,--no-whole-archive ${libs}' not in toolchain:
    parser.error('Unreviewed solink response rule; do not infer archive selection')
ar = angle / 'third_party/llvm-build/Release+Asserts/bin/llvm-ar'
records = {}
selected_objects = set()
for target in ['libEGL', 'libGLESv1_CM', 'libGLESv2']:
    ninja = build / 'obj' / (target + '.ninja')
    text = ninja.read_text()
    line = next(line for line in text.splitlines() if line.startswith('build ') and ': solink ' in line)
    explicit = line.split(': solink ', 1)[1].split('|', 1)[0].split()
    libs = re.search(r'^  libs = (.*)$', text, re.M)
    solibs = re.search(r'^  solibs = (.*)$', text, re.M)
    record = {'ninja_file': relative(ninja), 'ninja_sha256': sha(ninja),
              'whole_archive_rule': True, 'objects': [], 'archives': [],
              'ordinary_link_libraries': libs.group(1).split() if libs else [],
              'shared_link_libraries': solibs.group(1).split() if solibs else []}
    for name in explicit:
        path = (build / name).resolve()
        if path.suffix == '.o':
            record['objects'].append(object_record(path)); selected_objects.add(path)
        elif path.suffix == '.a':
            members = subprocess.check_output([str(ar), 't', str(path)], text=True).splitlines()
            if path.read_bytes()[:8] != b'!<thin>\n':
                parser.error(f'Unexpected non-thin whole archive: {name}')
            member_paths = [Path(member) for member in members]
            if any(not member.is_absolute() for member in member_paths):
                parser.error(f'Unreviewed relative thin-archive member: {name}')
            record['archives'].append({'path': relative(path), 'sha256': sha(path),
                                      'link_selection': 'All members supplied through --whole-archive; final section GC is separate',
                                      'members': [object_record(member) for member in member_paths]})
            selected_objects.update(member_paths)
        else:
            parser.error(f'Unreviewed explicit link input: {name}')
    records[target] = record
result = {'description': 'Actual direct link objects and all forced thin-archive members. Ordinary compiler-rt extraction and final GC require separate evidence; host tool dependencies are excluded.',
          'libraries': records, 'unique_object_count': len(selected_objects)}
args.output.write_text(json.dumps(result, indent=2) + '\n')
if args.headers_output:
    targets = {str(path.relative_to(build)) for path in selected_objects}
    dependencies = subprocess.check_output(
        ['ninja', '-C', str(build), '-t', 'deps', *sorted(targets)], text=True)
    seen = set()
    paths = set()
    for line in dependencies.splitlines():
        if line and not line.startswith(' '):
            match = re.match(r'(.+): #deps .* \(VALID\)$', line)
            if not match or match.group(1) not in targets:
                parser.error(f'Unreviewed compiler dependency record: {line}')
            seen.add(match.group(1))
        elif line.startswith('    '):
            path = (build / line.strip()).resolve()
            if path.is_relative_to(angle):
                paths.add(path)
    if seen != targets:
        parser.error('Missing compiler dependencies for selected objects')
    headers = []
    for path in sorted(paths):
        name = relative(path)
        if any(prefix in name for prefix in ('third_party/vulkan-headers/',
                                            'third_party/spirv-headers/')):
            headers.append({'path': name, 'sha256': sha(path),
                            'spdx': re.findall(r'SPDX-License-Identifier:\s*([^\r\n]+)',
                                               path.read_text(errors='replace'))})
    args.headers_output.write_text(json.dumps({
        'description': 'Actual compiler dependencies across selected link objects; unselected host tools excluded',
        'objects_checked': len(seen),
        'optional_dependency_counts': {
            prefix: sum(prefix in relative(path) for path in paths)
            for prefix in ('third_party/abseil-cpp/', 'third_party/perfetto/')},
        'headers': headers}, indent=2) + '\n')
print(f'Recorded {len(selected_objects)} distinct whole-archive/direct objects in three Android libraries')
