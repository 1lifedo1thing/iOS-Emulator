#!/usr/bin/env python3
"""Preflight a pinned private candidate plan; execute only its approved hash.

No shell command evaluation, source-cache migration or device operations.
The plan is an operator-owned local artifact, not an untrusted job format.
The historical max_group_rss_kib input now bounds the sampled RSS sum of the
owned session, including Ninja job process groups. Shared pages may be counted
more than once; this polling guard is not a kernel-enforced memory limit.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def require(value, message):
    if not value:
        raise RuntimeError(message)


def source_check(plan):
    root = plan['source_root']
    require(subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
            == plan['source_revision'], 'source revision changed')
    require(not subprocess.check_output(['git', 'status', '--porcelain=v1', '--untracked-files=all'],
                                       cwd=root, text=True), 'source must be clean, including untracked files')


def preflight(plan):
    require(plan['schema'] == 1 and plan['android_api'] in (28, 29) and plan['jobs'] == 1,
            'only explicit API28/29, sequential j1 candidate plans are supported')
    if plan['android_api'] == 29:
        require([t['abi'] for t in plan['targets']] == ['arm64-v8a'],
                'API29 experiment requires exactly one ARM64 target')
    source_check(plan)
    for pin in plan['files']:
        require(sha(pin['path']) == pin['sha256'], 'input changed: ' + pin['path'])
    for tool in plan['tools']:
        version = subprocess.check_output(tool['version_argv'], text=True, stderr=subprocess.STDOUT,
                                          timeout=10).splitlines()[0]
        require(version == tool['version'], 'tool version changed: ' + tool['path'])
    for project in plan.get('subprojects', []):
        checkout = project['path']
        require(subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=checkout, text=True).strip()
                == project['revision'], 'subproject revision changed: ' + checkout)
        require(subprocess.check_output(['git', 'status', '--porcelain=v1', '--untracked-files=all'],
                                        cwd=checkout, text=True) == project['status'],
                'subproject contents changed: ' + checkout)
    require(os.environ.get('PATH', '') == plan['path_environment'], 'operator PATH differs from pinned plan')
    destinations = [Path(plan['run_output'])]
    for target in plan['targets']:
        destinations.extend(Path(target[key]) for key in ['build', 'notices'])
        if target.get('sourcekit'):
            destinations.append(Path(target['sourcekit']))
        require('--source-update' not in target['configure_argv'], 'cache migration is forbidden')
        if target['abi'] != 'linux':
            argv = target['configure_argv']
            require(argv.count('--android-api') == 1 and
                    argv[argv.index('--android-api') + 1] == str(plan['android_api']),
                    'Android compiler API differs')
    for destination in destinations:
        require(destination.is_absolute() and '..' not in destination.parts, 'invalid destination')
        require(not destination.exists() and not destination.is_symlink(), 'destination exists: ' + str(destination))
        require(not any((p / '.git').exists() for p in (destination, *destination.parents)),
                'candidate outputs must be outside Git')
    return {'preflight': 'PASS', 'source': plan['source_revision'], 'android_api': plan['android_api'],
            'targets': [t['abi'] for t in plan['targets']], 'native_compilation_performed': False,
            'tool_version_checks': len(plan['tools'])}


def session_processes(session):
    """Ninja creates job process groups, all in our start_new_session session."""
    members = []
    for path in Path('/proc').iterdir():
        if not path.name.isdigit():
            continue
        try:
            fields = (path / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(fields[3]) == session:  # session, field6
                members.append({'pid': int(path.name), 'pgrp': int(fields[2]),
                                'start': int(fields[19]), 'state': fields[0],
                                'rss_pages': max(0, int(fields[21]))})
        except (OSError, ValueError, IndexError):
            continue
    return members


def session_rss_kib(session):
    return sum(p['rss_pages'] for p in session_processes(session)) * os.sysconf('SC_PAGE_SIZE') // 1024


def capture_session(process):
    # Capture before poll()/wait() can reap even a very short-lived leader.
    leader = next((p for p in session_processes(process.pid) if p['pid'] == process.pid), None)
    require(leader is not None and leader['pgrp'] == process.pid, 'new command session identity unavailable')
    process.candidate_session_start = leader['start']


def owned_session(process):
    members = session_processes(process.pid)
    leader = next((p for p in members if p['pid'] == process.pid), None)
    # A reused leader PID is a different session, even if its numeric SID agrees.
    require(leader is None or leader['start'] == process.candidate_session_start,
            'command session identity changed')
    return members


def dispose_session(process):
    # Signal only groups with a still-matching member in the owned session.
    # Ninja's separate job groups must also retire when the scheduler exits.
    for number in (signal.SIGTERM, signal.SIGKILL):
        members = owned_session(process)
        for group in sorted({p['pgrp'] for p in members if p['state'] != 'Z'}):
            current = owned_session(process)
            matching = any(p['pgrp'] == group and p['state'] != 'Z' and
                           any(p['pid'] == old['pid'] and p['start'] == old['start']
                               for old in members) for p in current)
            if matching:
                require(group != os.getpgrp(), 'refusing operator process group')
                try:
                    os.killpg(group, number)
                except ProcessLookupError:
                    pass
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            process.poll()  # Reap the owned direct child, never another process.
            if not any(p['state'] != 'Z' for p in owned_session(process)):
                return True
            time.sleep(.05)
    return False


def run_step(plan, output, label, argv, cwd, timeout, events):
    source_check(plan)
    begin = time.monotonic()
    log = output / (label + '.log')
    peak = 0
    with log.open('x') as stream:
        command = ['/usr/bin/time', '-v', *map(str, argv)]
        original_mask = signal.pthread_sigmask(signal.SIG_BLOCK,
                                               {signal.SIGTERM, signal.SIGHUP, signal.SIGINT})
        process = None
        event = {'label': label, 'argv': command, 'cwd': str(cwd),
                 'begin_monotonic': begin, 'timeout_seconds': timeout}
        events.append(event)
        try:
            try:
                # This driver has one thread. The child must not inherit the
                # short parent admission mask after exec.
                process = subprocess.Popen(command, cwd=cwd, env=clean_environment(plan),
                    stdout=stream, stderr=subprocess.STDOUT, start_new_session=True,
                    preexec_fn=lambda: signal.pthread_sigmask(signal.SIG_SETMASK, original_mask))
                event['pid'] = process.pid
                capture_session(process)
            finally:
                # Pending host signals unwind only once the owned handle is
                # available to the surrounding finally.
                signal.pthread_sigmask(signal.SIG_SETMASK, original_mask)
            print(json.dumps(event), flush=True)
            last_report = begin
            while process.poll() is None:
                now = time.monotonic()
                rss = session_rss_kib(process.pid)
                peak = max(peak, rss)
                require(now - begin < timeout, 'command deadline expired: ' + label)
                require(rss <= plan['max_group_rss_kib'], 'command RSS ceiling exceeded: ' + label)
                if now - last_report >= 30:
                    print(json.dumps({'running': label, 'pid': process.pid,
                                      'seconds': now - begin, 'session_rss_kib': rss}), flush=True)
                    last_report = now
                time.sleep(.5)
            require(process.returncode == 0, 'command failed: ' + label)
        finally:
            cleanup_error = None
            if process is not None:
                try:
                    if hasattr(process, 'candidate_session_start'):
                        if process.poll() is None or owned_session(process):
                            event['cleanup_session_retired'] = dispose_session(process)
                    else:
                        # Session admission failed. The unreaped direct child
                        # is ours; do not signal unverified descendant groups.
                        for number in (signal.SIGTERM, signal.SIGKILL):
                            if process.poll() is not None:
                                break
                            process.send_signal(number)
                            try:
                                process.wait(timeout=2)
                            except subprocess.TimeoutExpired:
                                pass
                        event['cleanup_session_retired'] = not session_processes(process.pid)
                except BaseException as error:
                    event['cleanup_error'] = str(error)
                    cleanup_error = error
                event['exit'] = process.poll()
            event.update(seconds=time.monotonic() - begin,
                         sampled_peak_session_rss_kib=peak, log_sha256=sha(log))
            (output / 'commands.json').write_text(json.dumps(events, indent=2) + '\n')
            print(json.dumps(event), flush=True)
            if cleanup_error is not None:
                raise cleanup_error
            require(event.get('cleanup_session_retired', True), 'owned command session did not retire')


def clean_environment(plan):
    env = os.environ.copy()
    for key in ['CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CPATH',
                'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'LD_PRELOAD',
                'PKG_CONFIG', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR']:
        env.pop(key, None)
    env['PATH'] = plan['path_environment']
    return env


def shared_j1(build):
    # Execute the committed shared recipe. Only Ninja scheduling is narrowed;
    # preserve target compiler, arguments, linker inputs and output identity.
    import runpy
    import sys
    original = subprocess.run
    commands = []
    def single(*args, **kwargs):
        if args and isinstance(args[0], list) and args[0][0] == 'ninja':
            argv = ['-j1' if value == '-j2' else value for value in args[0]]
            commands.append(argv)
            args = (argv,) + args[1:]
        return original(*args, **kwargs)
    subprocess.run = single
    try:
        sys.argv = ['make-shared.py', build]
        runpy.run_path(str(Path(__file__).with_name('make-shared.py')), run_name='__main__')
    finally:
        subprocess.run = original
        Path(build, 'scheduler-actual-argv.json').write_text(json.dumps(commands, indent=2) + '\n')


def validate_elf(target, output):
    library = str(Path(target['build']) / 'libqemu-arm.so')
    reports = {}
    for label, options in [('dynamic', ['-dW']), ('segments', ['-lW']), ('symbols', ['--dyn-syms', '-W'])]:
        text = subprocess.check_output([target['readelf'], *options, library], text=True, timeout=15)
        reports[label] = text
        (output / (target['abi'] + '-elf-' + label + '.txt')).write_text(text)
    exports = set()
    for line in reports['symbols'].splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[4] in ['GLOBAL', 'WEAK'] and fields[6] != 'UND':
            exports.add(fields[7].split('@', 1)[0])
    require(exports == set(target['expected_exports']), 'public engine exports differ')
    needed = re.findall(r'\(NEEDED\).*\[([^]]+)\]', reports['dynamic'])
    require(set(needed) == set(target['expected_needed']), 'dynamic dependency closure differs')
    require(re.findall(r'\(SONAME\).*\[([^]]+)\]', reports['dynamic']) == ['libqemu-arm.so'], 'SONAME differs')
    alignments = [int(line.split()[-1], 16) for line in reports['segments'].splitlines()
                  if line.strip().startswith('LOAD ')]
    require(alignments and all(a == 16384 for a in alignments), 'ELF LOAD alignment differs')
    return {'exports': len(exports), 'needed': needed, 'load_alignment': alignments}


def freeze_files(build, frozen, names):
    frozen.mkdir(parents=True, exist_ok=False)
    for name in names:
        expected = sha(build / name)
        shutil.copyfile(build / name, frozen / name)
        require(sha(frozen / name) == expected and sha(build / name) == expected,
                'frozen copy differs: ' + name)
    for name in names:
        (frozen / name).chmod(0o444)
    frozen.chmod(0o555)


def execute(plan, plan_sha):
    output = Path(plan['run_output'])
    output.mkdir(parents=True, exist_ok=False)
    (output / 'approved-plan.json').write_text(json.dumps(plan, indent=2) + '\n')
    events = []
    artifacts = []
    try:
        for target in plan['targets']:
            abi, build = target['abi'], Path(target['build'])
            run_step(plan, output, abi + '-configure', target['configure_argv'], plan['source_root'], 180, events)
            run_step(plan, output, abi + '-ninja', ['ninja', '-j1', '-d', 'keeprsp', 'qemu-system-arm'],
                     build, plan['compile_timeout_seconds'], events)
            if target.get('cpp_input'):
                with Path(target['cpp_input']).open('rb') as src, (build / 'libc++_shared.so').open('xb') as dst:
                    shutil.copyfileobj(src, dst, 65536)
            run_step(plan, output, abi + '-shared', ['/usr/bin/python3', str(Path(__file__)),
                     '--shared-j1', str(build)], build, 180, events)
            for label, argv in target['post_commands']:
                run_step(plan, output, abi + '-' + label, argv, plan['source_root'], 300, events)
            manifest = json.loads((build / 'portable-manifest.json').read_text())
            require(not manifest['source_dirty'] and manifest['source_revision'] == plan['source_revision'],
                    'new engine producer mismatch')
            require(sha(build / 'libqemu-arm.so') == manifest['library_sha256'], 'new library hash mismatch')
            elf = validate_elf(target, output)
            if target.get('sourcekit'):
                require(sha(Path(target['sourcekit']) / 'bundle.json'), 'sourcekit receipt missing')
            frozen = output / 'frozen' / manifest['library_sha256']
            names = ['libqemu-arm.so', 'portable-manifest.json', 'scheduler-actual-argv.json']
            if target.get('cpp_input'):
                names.append('libc++_shared.so')
            freeze_files(build, frozen, names)
            artifacts.append({'abi': abi, 'frozen': str(frozen), 'library_sha256': manifest['library_sha256'],
                              'native_source_revision': manifest['source_revision'],
                              'packaging_tool_revision': plan['source_revision'],
                              'manifest_sha256': sha(frozen / 'portable-manifest.json'), 'elf': elf,
                              'notice_manifest_sha256': sha(Path(target['notices']) / 'manifest.json'),
                              'sourcekit_manifest_sha256': sha(Path(target['sourcekit']) / 'bundle.json')
                              if target.get('sourcekit') else None})
            (output / 'artifacts.json').write_text(json.dumps(artifacts, indent=2) + '\n')
        (output / 'frozen').chmod(0o555)
    finally:
        (output / 'terminal.json').write_text(json.dumps({'approved_plan_sha256': plan_sha,
            'completed_abis': [a['abi'] for a in artifacts], 'target_guest_verified': False,
            'full_source_replay_or_relink_verified': False, 'commands': events}, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path)
    parser.add_argument('--execute-approved-sha256', help='exact plan hash explicitly allocated by operator')
    parser.add_argument('--shared-j1', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.shared_j1:
        require(args.plan is None and args.execute_approved_sha256 is None, 'mixed execution modes')
        shared_j1(args.shared_j1)
        return
    require(args.plan is not None, '--plan required')
    plan = json.loads(args.plan.read_text())
    digest = sha(args.plan)
    print(json.dumps(preflight(plan)), flush=True)
    if args.execute_approved_sha256:
        require(args.execute_approved_sha256 == digest, 'approved plan hash differs')
        execute(plan, digest)


if __name__ == '__main__':
    def interrupted(number, frame):
        # Only in-memory handler work; active command cleanup lives in finally.
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        raise KeyboardInterrupt('operator interrupted candidate driver')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    main()
