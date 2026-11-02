#!/usr/bin/env python3
"""Relink a finished MinGW engine build as libqemu-arm.dll, the portable engine the desktop app loads.

Usage: make-shared-windows.py BUILD OUTPUT_DIR

The Windows counterpart of make-shared.py: ninja's own link line for qemu-system-arm.exe, with system/main.c
swapped for the iOS entry, UI and control objects, linked -shared with a .def exporting qemu_ios_* only. BUILD is
read, never written (no ninja run): new objects, the .def and the DLL go to OUTPUT_DIR.
"""
import argparse, json, pathlib, shlex, subprocess

p = argparse.ArgumentParser()
p.add_argument('build', type=pathlib.Path)
p.add_argument('output', type=pathlib.Path)
a = p.parse_args()
build = a.build.resolve()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)

commands = json.loads((build / 'compile_commands.json').read_text())
entry = next(c for c in commands if c['file'].endswith('/system/main.c'))
src = (build / entry['file']).resolve().parents[1]
compile_args = shlex.split(entry['command'])
compiler = compile_args[:1]
base, skip = [], False
for arg in compile_args[1:]:
    if skip:
        skip = False
        continue
    if arg in ('-o', '-MF', '-MQ', '-MT'):
        skip = True
        continue
    if arg in ('-c', '-MD', '-MMD') or arg.endswith('system/main.c'):
        continue
    base.append(arg)
base += ['-iquote', str(src / 'target/arm'), '-iquote', str(build / 'arm-softmmu'),
         '-I' + str(src / 'contrib/ios-app'), '-I' + str(src / 'contrib/macos-app')]

# The same generated extras as make-shared.py (Mach-O identity out, controls through the input queue).
extras = (src / 'contrib/macos-app/qemu-macos-extras.c').read_text().replace('#include <mach-o/loader.h>', '')
extras = extras.replace('#include <dlfcn.h>', '')  # only qemu_ios_build_id used it, replaced below
extras = extras.replace('#include "qemu-macos-extras.h"', '#include "qemu-macos-extras.h"\n#include "qemu-ios-input.h"')
schedule = 'aio_bh_schedule_oneshot(qemu_get_aio_context(), qmp_bh, (void *)fn);'
if extras.count(schedule) != 1:
    raise RuntimeError('Control scheduling recipe changed')
extras = extras.replace(schedule, 'ios_input_enqueue(qmp_bh, (void *)fn, NULL);')
extras = extras.replace('fn == qmp_system_reset || fn == qmp_quit || fn == qmp_system_powerdown',
                        'fn == qmp_stop || fn == qmp_system_reset || fn == qmp_quit || fn == qmp_system_powerdown')
start = extras.index('const char *qemu_ios_build_id(void)')
end = extras.index('/* Guest output capture', start)
revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=src, text=True).strip()
dirty = bool(subprocess.check_output(['git', 'status', '--porcelain=v1'], cwd=src, text=True))
identity = revision + ('-dirty' if dirty else '')
extras = extras[:start] + f'const char *qemu_ios_build_id(void) {{ return "{identity}"; }}\n\n' + extras[end:]
(out / 'qemu-portable-extras.c').write_text(extras)

objects = []
for source in [src / 'contrib/ios-app/qemu-ios-entry.c', src / 'contrib/ios-app/qemu-ios-ui.c',
               out / 'qemu-portable-extras.c']:
    obj = out / (source.stem + '.obj')
    objects.append(str(obj))
    subprocess.run(compiler + base + ['-c', str(source), '-o', str(obj)], cwd=build, check=True)

# Export the app ABI only: every qemu_ios_* function the new objects define.
nm = compiler[0].replace('gcc-posix', 'nm').replace('-gcc', '-nm')
if not nm.endswith('nm'):
    nm = 'x86_64-w64-mingw32-nm'
names = set()
for obj in objects:
    for line in subprocess.check_output([nm, '-g', '--defined-only', obj], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] == 'T' and fields[2].startswith('qemu_ios_'):
            names.add(fields[2])
(out / 'libqemu-arm.def').write_text('LIBRARY libqemu-arm.dll\nEXPORTS\n' + ''.join(f'    {n}\n' for n in sorted(names)))

response = build / 'qemu-system-arm.exe.rsp'
link = shlex.split(response.read_text())
args, skip = [], False
for arg in link:
    if skip:
        skip = False
        continue
    if arg == '-o':
        skip = True
        continue
    if arg.endswith('system_main.c.obj'):
        args.extend(objects)
        continue
    if arg in ('-mwindows', '-municode') or arg.endswith('.rc_version.o') or arg.startswith('-Wl,--subsystem'):
        continue
    args.append(arg)
output = out / 'libqemu-arm.dll'
# C++ objects (the PVRTC decoder) need the C++ driver for their runtime.
linker = [compiler[0].replace('-gcc', '-g++')] + compiler[1:]
args = linker + args + ['-shared', str(out / 'libqemu-arm.def'), '-Wl,--out-implib,' + str(out / 'libqemu-arm.dll.a'),
                          '-o', str(output)]
(out / 'link.json').write_text(json.dumps(args, indent=2) + '\n')
subprocess.run(args, cwd=build, check=True)
print(output)
