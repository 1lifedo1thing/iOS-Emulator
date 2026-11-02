#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Create an offline schema1 notice bundle from selected ANGLE link evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--checkout', required=True, type=Path)
parser.add_argument('--selection', required=True, type=Path)
parser.add_argument('--headers', required=True, type=Path)
parser.add_argument('--elf', required=True, type=Path)
parser.add_argument('--compiler-rt-license', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
root=args.checkout.resolve(); angle=root/'angle'
args.output.mkdir(parents=True, exist_ok=False)
sha=lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
selection=json.loads(args.selection.read_text())
sources=[]
for library in selection['libraries'].values():
    sources.extend(obj['source'] for obj in library['objects'])
    sources.extend(obj['source'] for archive in library['archives'] for obj in archive['members'])
headers=json.loads(args.headers.read_text())
components=[('ANGLE','BSD-3-Clause','LICENSE','')]
for component,license_name,path,prefix in [
    ('Abseil','Apache-2.0','third_party/abseil-cpp/LICENSE','third_party/abseil-cpp/'),
    ('Perfetto','blessing AND BSD-3-Clause AND Apache-2.0','third_party/perfetto/LICENSE','third_party/perfetto/'),
    ('libc++','Apache-2.0 WITH LLVM-exception','third_party/libc++/src/LICENSE.TXT','third_party/libc++/'),
    ('libc++abi','Apache-2.0 WITH LLVM-exception','third_party/libc++abi/src/LICENSE.TXT','third_party/libc++abi/'),
    ('libunwind','Apache-2.0 WITH LLVM-exception','third_party/libunwind/src/LICENSE.TXT','third_party/libunwind/'),
    ('Volk','MIT','src/third_party/volk/LICENSE.md','src/third_party/volk/'),
    ('xxHash','BSD-2-Clause','src/common/third_party/xxhash/LICENSE','src/common/third_party/xxhash/'),
    ('zlib','Zlib','third_party/zlib/LICENSE','third_party/zlib/'),
]:
    if any(prefix in source for source in sources): components.append((component,license_name,path,prefix))
if any('third_party/vulkan-headers/' in item['path'] for item in headers['headers']):
    components.append(('Vulkan-Headers','MIT (selected from Apache-2.0 OR MIT)','third_party/vulkan-headers/src/LICENSES/MIT.txt',''))
if any('third_party/spirv-headers/' in item['path'] for item in headers['headers']):
    components.append(('SPIRV-Headers','MIT; see file-specific exceptions','third_party/spirv-headers/src/LICENSE',''))
files=[];notices=[]
for component,license_name,name,prefix in components:
    source=angle/name;directory=source.parent
    reference=subprocess.check_output(['git','-C',str(directory),'remote','get-url','origin'],text=True).strip()
    revision=subprocess.check_output(['git','-C',str(directory),'rev-parse','HEAD'],text=True).strip()
    public_root=Path(subprocess.check_output(['git','-C',str(directory),'rev-parse','--show-toplevel'],text=True).strip())
    filename_component=component.replace('+','x')
    relative=f'notices/{filename_component}-{source.name}'
    target=args.output/relative;target.parent.mkdir(exist_ok=True);shutil.copy2(source,target)
    item={'path':relative,'sha256':sha(target)};files.append(item)
    notices.append(dict(item,component=component,license=license_name,source_reference=reference,
                        source_revision=revision,source_path=str(source.relative_to(public_root))))
origin=json.loads((args.compiler_rt_license.parent/'origin.json').read_text())
relative='notices/compiler-rt-LICENSE.TXT';target=args.output/relative
shutil.copy2(args.compiler_rt_license,target)
item={'path':relative,'sha256':sha(target)};files.append(item)
notices.append(dict(item,component='compiler-rt',license='Apache-2.0 WITH LLVM-exception',
                    source_reference=origin['source_reference'],source_revision=origin['source_revision'],
                    source_path=origin['path'],provenance_qualification=origin['qualification']))
# The selected GLSL parser carries the Bison skeleton grant in its leading
# comment blocks; retain that exception rather than calling all ANGLE code BSD.
parser_source='src/compiler/translator/glslang_tab_autogen.cpp'
if parser_source in sources:
    source=angle/parser_source
    header=source.read_text().split('/* C LALR(1) parser skeleton',1)[0]
    if 'As a special exception' not in header:
        parser.error('Unreviewed Bison parser grant')
    relative='notices/Bison-parser-grant.txt';target=args.output/relative
    target.write_text(header)
    item={'path':relative,'sha256':sha(target)};files.append(item)
    notices.append(dict(item,component='Bison-generated GLSL parser',
                        license='GPL-3.0-or-later WITH Bison-exception-2.2',
                        source_reference='https://chromium.googlesource.com/angle/angle',
                        source_revision=subprocess.check_output(['git','-C',str(angle),'rev-parse','HEAD'],text=True).strip(),
                        source_path=parser_source,notice_extraction='Leading copyright and grant comment blocks'))
for source,name in [(args.selection,'selected-link-inputs.json'),(args.headers,'selected-khronos-headers.json'),
                    (args.elf,'elf-closure.json'),(args.compiler_rt_license.parent/'origin.json','compiler-rt-origin.json')]:
    target=args.output/name;shutil.copy2(source,target);files.append({'path':name,'sha256':sha(target)})
elf=json.loads(args.elf.read_text())
manifest={'schema':1,'source_reference':'https://chromium.googlesource.com/angle/angle',
          'source_revision':subprocess.check_output(['git','-C',str(angle),'rev-parse','HEAD'],text=True).strip(),
          'files':files,'notices':notices,
          'libraries':{name:item['sha256'] for name,item in elf['objects'].items()},
          'selected_input_inventory':{'path':'selected-link-inputs.json','sha256':sha(args.output/'selected-link-inputs.json')},
          'license_compatibility':'unresolved; selected grants and complete application combination require review',
          'compiler_rt_provenance':origin}
(args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
for path in args.output.rglob('*.json'):
    if '/home/' in path.read_text(): parser.exit(2,'Private absolute path in public bundle metadata\n')
for item in files:
    if any(not re.fullmatch(r'[A-Za-z0-9._-]+', component) or component in ('.', '..')
           for component in item['path'].split('/')):
        parser.exit(2, 'Notice path violates the common offline viewer grammar\n')
    if sha(args.output / item['path']) != item['sha256']:
        parser.exit(2, 'Notice file changed while constructing the manifest\n')
print(f'Created schema1 notice bundle: {len(notices)} component notices, {len(files)} hashed files')
