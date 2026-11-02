#!/usr/bin/env python3
"""Finite owner-checked perf capture and offline native CPU attribution.

No guest control, new engine flags, perfmap/jitdump or kernel setting changes.
Collection needs an exact external plan and matching ROOT-GRANT.json.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import select
import signal
import stat
import subprocess
import time

MAX_BYTES = 256 * 1024 * 1024
PERF = '/usr/bin/perf'
PRESETS = {
    'user-40s': {'event': 'cpu-clock:u', 'seconds': 40, 'timeout_seconds': 45,
                 'outer_seconds': 48, 'max_bytes': MAX_BYTES, 'max_threads': 64,
                 'include_misc': False},
    'user-kernel-10s': {'event': 'cpu-clock', 'seconds': 10, 'timeout_seconds': 15,
                       'outer_seconds': 18, 'max_bytes': 64 * 1024 * 1024,
                       'max_threads': 2, 'include_misc': True},
}


def capture_spec(plan):
    # Absent capture is the original schema-1 plan; retain its exact defaults.
    capture = plan.get('capture')
    if capture is None:
        return dict(PRESETS['user-40s'], preset='user-40s')
    require(isinstance(capture, dict), 'invalid capture specification')
    preset = capture.get('preset')
    require(isinstance(preset, str) and preset in PRESETS, 'unknown capture preset')
    expected = dict(PRESETS[preset], preset=preset)
    require(capture == expected, 'capture specification differs from fixed preset')
    return expected


def child_limits(max_bytes=MAX_BYTES):
    resource.setrlimit(resource.RLIMIT_FSIZE, (max_bytes, max_bytes))


def owned_session_members(sid):
    members = {}
    for entry in Path('/proc').iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(fields[3]) == sid and fields[0] not in ('Z', 'X'):
                members[int(entry.name)] = fields[19]
        except (FileNotFoundError, ProcessLookupError):
            continue
    return members


def retire_collector(child, child_start):
    # Session numbers cannot be reused while members remain. The session was
    # created by our own unreaped Popen child, never by the sampled worker.
    for action, grace in [('INT', 5), ('KILL', 3)]:
        members = owned_session_members(child.pid)
        if not members:
            child.wait(timeout=grace)
            return
        # If a session leader is present, it must still be our exact child,
        # including after Popen has reaped it (never accept a reused SID).
        if child.pid in members and child_start is not None:
            require(members[child.pid] == child_start, 'collector identity changed')
        if child.poll() is None and child_start is not None:
            require(identity(child.pid)['start_ticks'] == child_start, 'collector identity changed')
        groups = set()
        for pid, start in members.items():
            try:
                fields = (Path('/proc') / str(pid) / 'stat').read_text().rsplit(')', 1)[1].split()
                require(int(fields[3]) == child.pid and fields[19] == start, 'collector member changed')
                groups.add(int(fields[2]))
            except FileNotFoundError:
                continue
        for group in groups:
            subprocess.run(['sudo', '-n', '/usr/bin/kill', '--signal', action,
                            '--', '-' + str(group)], timeout=3, check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + grace
        while owned_session_members(child.pid) and time.monotonic() < deadline:
            time.sleep(.05)
    require(not owned_session_members(child.pid), 'owned profiler session failed to retire')
    child.wait(timeout=1)


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, data):
    with path.open('x') as stream:
        json.dump(data, stream, indent=2)
        stream.write('\n')


def identity(pid, tid=None):
    root = Path('/proc') / str(pid)
    task = root if tid is None else root / 'task' / str(tid)
    stat = (task / 'stat').read_text().rsplit(')', 1)[1].split()
    require(stat[0] not in ('Z', 'X'), 'worker/thread retired')
    return {'start_ticks': stat[19], 'comm': (task / 'comm').read_text().strip(),
            'uid': task.stat().st_uid, 'user_ticks': int(stat[11]),
            'system_ticks': int(stat[12]), 'state': stat[0],
            'schedstat': (task / 'schedstat').read_text().strip()}


def validate(plan, verify_binary=True):
    observed_begin = time.monotonic_ns()
    spec = capture_spec(plan)
    require(plan['schema'] == 1 and type(plan['pid']) is int and plan['pid'] > 0,
            'invalid plan/worker PID')
    require(plan['uid'] == os.getuid() and plan['uid'] != 0, 'plan must identify this ordinary owner')
    require(Path('/proc/sys/kernel/random/boot_id').read_text().strip() == plan['host_boot_id'],
            'host boot changed')
    worker = identity(plan['pid'])
    require(worker['uid'] == plan['uid'] and worker['start_ticks'] == plan['start_ticks'],
            'worker identity changed')
    root = Path('/proc') / str(plan['pid'])
    require((root / 'cmdline').read_bytes().decode().split('\0')[:-1] == plan['argv'],
            'worker command changed')
    require('--worker' in plan['argv'], 'expected isolated Native14 worker')
    require(plan['threads'] and len(plan['threads']) <= spec['max_threads'], 'explicit bounded thread list required')
    require(len({t['tid'] for t in plan['threads']}) == len(plan['threads']), 'duplicate thread')
    threads = {}
    for thread in plan['threads']:
        require(type(thread['tid']) is int and thread['tid'] > 0, 'invalid TID')
        current = identity(plan['pid'], thread['tid'])
        require(current['uid'] == plan['uid'] and current['start_ticks'] == thread['start_ticks'] and
                current['comm'] == thread['comm'], 'thread identity/name changed')
        require(isinstance(thread['role'], str) and thread['role'], 'owner-assigned role required')
        threads[str(thread['tid'])] = current
    maps = (root / 'maps').read_text()
    engine = Path(plan['engine']['path']).resolve()
    require(any(line.endswith(' ' + str(engine)) and 'x' in line.split()[1]
                for line in maps.splitlines()), 'exact engine is not executable-mapped')
    if verify_binary:
        require(sha(engine) == plan['engine']['sha256'], 'engine hash changed')
        require(sha(PERF) == plan['perf_sha256'], 'perf binary changed')
    return {'worker': worker, 'threads': threads, 'maps': maps,
            'observed_begin_monotonic_ns': observed_begin,
            'observed_monotonic_ns': time.monotonic_ns(),
            'clock_ticks_per_second': os.sysconf('SC_CLK_TCK')}


def profile_argv(plan, output):
    spec = capture_spec(plan)
    require(0 < len(plan['threads']) <= spec['max_threads'], 'explicit bounded thread list required')
    tids = ','.join(str(t['tid']) for t in plan['threads'])
    return ['sudo', '-n', '/usr/bin/timeout', '--signal=INT', '--kill-after=3s', str(spec['timeout_seconds']) + 's',
            '/usr/bin/prlimit', '--fsize=' + str(spec['max_bytes']) + ':' + str(spec['max_bytes']), '--',
            PERF, 'record', '--no-buildid-cache', '-e', spec['event'], '-c', '20000000', '--no-inherit',
            '-t', tids, '-m', '8', '--call-graph', 'dwarf,8192', '--clockid', 'mono',
            '-o', str(output / 'perf.data'), '--', '/usr/bin/sleep', str(spec['seconds'])]


def outside_git(path):
    require(path.is_absolute() and not path.exists() and not path.is_symlink(), 'fresh absolute output required')
    require(not any((p / '.git').exists() for p in (path, *path.parents)), 'private output outside Git required')


def prepare(args):
    outside_git(args.plan)
    outside_git(args.output)
    worker = identity(args.pid)
    threads = []
    for selected in args.thread:
        tid_text, role = selected.split(':', 1)
        tid = int(tid_text)
        current = identity(args.pid, tid)
        threads.append({'tid': tid, 'start_ticks': current['start_ticks'],
                        'comm': current['comm'], 'role': role})
    engine = args.engine.resolve()
    plan = {'schema': 1, 'pid': args.pid, 'uid': os.getuid(),
            'host_boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
            'start_ticks': worker['start_ticks'],
            'argv': (Path('/proc') / str(args.pid) / 'cmdline').read_bytes().decode().split('\0')[:-1],
            'threads': threads, 'engine': {'path': str(engine), 'sha256': sha(engine)},
            'perf_sha256': sha(PERF), 'output': str(args.output),
            'capture': dict(PRESETS[args.preset], preset=args.preset)}
    validate(plan)
    save(args.plan, plan)
    print(json.dumps({'plan_sha256': sha(args.plan), 'tool_sha256': sha(__file__),
                      'attach_performed': False}), flush=True)


def collect(args):
    plan = json.loads(args.plan.read_text())
    spec = capture_spec(plan)
    before = validate(plan)
    output = Path(plan['output'])
    outside_git(output)
    argv = profile_argv(plan, output)
    print(json.dumps({'preflight': 'PASS', 'argv': argv, 'capture': spec, 'attach_performed': False}), flush=True)
    if not args.execute_approved_sha256:
        return
    require(sha(args.plan) == args.execute_approved_sha256, 'approved plan differs')
    grant = json.loads(args.plan.with_name('ROOT-GRANT.json').read_text())
    require(grant['plan_sha256'] == sha(args.plan) and grant['tool_sha256'] == sha(__file__),
            'Root allocation differs')
    output.mkdir(mode=0o700)
    save(output / 'plan.json', plan)
    save(output / 'before.json', before)
    pidfd = os.pidfd_open(plan['pid'])
    retired = select.poll()
    retired.register(pidfd, select.POLLIN)
    child = None
    child_start = None
    after = None
    failure = None
    cleanup_failure = None
    data_sha256 = None
    began = time.monotonic_ns()
    def interrupted(number, frame):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        raise KeyboardInterrupt('profile collector interrupted')
    old_handlers = {s: signal.signal(s, interrupted) for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    original_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM, signal.SIGHUP})
    try:
        require(not retired.poll(0), 'owned worker retired before recording')
        validate(plan)
        with (output / 'record.stdout').open('xb') as stdout, (output / 'record.stderr').open('xb') as stderr:
            try:
                def child_setup():
                    child_limits(spec['max_bytes'])
                    signal.pthread_sigmask(signal.SIG_SETMASK, original_mask)
                child = subprocess.Popen(argv, stdout=stdout, stderr=stderr, stdin=subprocess.DEVNULL, start_new_session=True,
                                         preexec_fn=child_setup)
                child_start = identity(child.pid)['start_ticks']
                save(output / 'started.json', {'pid': child.pid, 'start_ticks': child_start,
                    'begin_monotonic_ns': began, 'argv': argv})
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, original_mask)
            while child.poll() is None:
                require(time.monotonic_ns() - began < spec['outer_seconds'] * 1_000_000_000, 'collector deadline expired')
                require(not retired.poll(0), 'owned worker retired during profile')
                validate(plan, verify_binary=False)
                for name in ['perf.data', 'record.stdout', 'record.stderr']:
                    p = output / name
                    require(not p.exists() or p.stat().st_size <= spec['max_bytes'], 'profile output ceiling exceeded')
                time.sleep(.5)
            require(child.returncode == 0, 'perf recording failed')
            after = validate(plan)
            data = output / 'perf.data'
            require(stat.S_ISREG(data.lstat().st_mode) and not data.is_symlink(), 'regular perf output required')
            # sudo perf creates its private output as root. Return only this
            # exact newly-created artifact to its ordinary owner for reporting.
            subprocess.run(['sudo', '-n', '/usr/bin/chown', str(os.getuid()) + ':' + str(os.getgid()),
                            '--', str(data)], check=True, timeout=3)
            data_sha256 = sha(data)
    except BaseException as error:
        failure = repr(error)
        raise
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.pthread_sigmask(signal.SIG_SETMASK, original_mask)
        try:
            if child is not None:
                retire_collector(child, child_start)
        except BaseException as error:
            cleanup_failure = repr(error)
        finally:
            os.close(pidfd)
            for number, handler in old_handlers.items():
                signal.signal(number, handler)
            save(output / 'terminal.json', {'begin_monotonic_ns': began, 'end_monotonic_ns': time.monotonic_ns(),
                'exit': child.returncode if child else None, 'failure': failure, 'cleanup_failure': cleanup_failure,
                'perf_data_sha256': data_sha256,
                'after': after, 'guest_scene_or_speedup_verified': False, 'native_sample_classification_pending': True,
                'capture': spec,
                'observer': str(spec['seconds']) + 's ' + spec['event'] +
                    ' samples, DWARF8192, 0.5s identity/maps reads; no guest input or capture'})
        if cleanup_failure is not None and failure is None:
            raise RuntimeError(cleanup_failure)


def category(symbol, dso):
    symbol = symbol.split('+0x', 1)[0]
    if symbol in ('[unknown]', 'unknown', '0') or symbol.startswith('0x'):
        return 'unresolved'  # No blanket anonymous-IP -> guest/JIT inference.
    if symbol.startswith('tcg_optimize'):
        return 'native-tcg-ir-optimization'
    if re.match(r'(tcg_reg_alloc|tcg_out_)', symbol):
        return 'native-host-code-emission-or-register-allocation'
    if re.match(r'(tb_gen_code|setjmp_gen_code|tcg_gen_code|aarch64_tr_|arm_tr_)', symbol):
        return 'native-translation-driver-or-decoder'
    if re.match(r'(gles_|egl|gl[A-Z]|mesa_|_mesa|lp_|LLVM)', symbol) or any(x in dso.lower() for x in ('libegl', 'libgl', 'dri.so', 'libllvm')):
        return 'native-graphics'
    if re.match(r'(fmss|bdrv_|blk_|pread|pwrite|read|write|fsync|fdatasync|aio_.*(read|write))($|_)', symbol):
        return 'native-storage-or-io-wrapper'
    if re.match(r'(io_read|io_write|memory_region_dispatch|ipod_touch_|s5l|pcf50633)', symbol):
        return 'native-mmio-or-device'
    if re.match(r'(cpu_tb_exec|tb_lookup|tb_find|tlb_|helper_|cpu_ld|cpu_st|address_space_)', symbol):
        return 'native-execution-helper-or-mmu'
    return 'other-resolved-native'


def report(args):
    root = args.recording.resolve()
    terminal = json.loads((root / 'terminal.json').read_text())
    require(terminal['exit'] == 0 and terminal['failure'] is None and
            terminal['cleanup_failure'] is None, 'failed recording; retain raw evidence')
    plan = json.loads((root / 'plan.json').read_text())
    require(sha(PERF) == plan['perf_sha256'], 'reporting perf binary differs')
    require(sha(plan['engine']['path']) == plan['engine']['sha256'], 'reporting engine symbols differ')
    require(sha(root / 'perf.data') == terminal['perf_data_sha256'], 'recording hash differs')
    spec = capture_spec(plan)
    output = args.output.resolve()
    outside_git(output)
    output.mkdir(mode=0o700)
    commands = [
        ('native-self.txt', ['report', '--stdio', '--no-children', '--stdio-color', 'never', '--percent-limit', '0']),
        ('native-inclusive.txt', ['report', '--stdio', '--children', '--stdio-color', 'never']),
        ('samples.txt', ['script', '--ns', '-G', '-F', 'pid,tid,time,period,' + ('misc,' if spec['include_misc'] else '') + 'ip,sym,dso']),
        ('mappings-lost.txt', ['script', '--ns', '--show-mmap-events', '--show-task-events', '--show-lost-events']),
        ('header.txt', ['report', '--header-only', '--stdio']),
    ]
    for name, options in commands:
        env = dict(os.environ, DEBUGINFOD_URLS='', PERF_PAGER='cat')
        with (output / name).open('xb') as stream, (output / (name + '.stderr')).open('xb') as error:
            result = subprocess.run([PERF, *options, '-i', str(root / 'perf.data')], stdout=stream,
                                    stderr=error, env=env, timeout=30,
                                    preexec_fn=lambda: child_limits(spec['max_bytes']))
        require(result.returncode == 0, 'offline report failed: ' + name)
    parsed = parse_samples((output / 'samples.txt').read_text(), include_misc=spec['include_misc'])
    save(output / 'sample-bins.json', parsed)
    save(output / 'receipt.json', {'perf_data_sha256': terminal['perf_data_sha256'],
        'engine': plan['engine'], 'perf_sha256': plan['perf_sha256'],
        'files': {p.name: sha(p) for p in sorted(output.iterdir())},
        'capture': spec, 'classification_valid': parsed['classification_valid'],
        'loss_checked': False, 'workload_and_latency_qualified': False})


def parse_samples(source, include_misc=False):
    bins, modes, per_thread, rows, rejected = (collections.Counter(), collections.Counter(),
                                             collections.defaultdict(collections.Counter), [], [])
    # perf prints misc immediately after PID/TID, regardless of -F field order.
    mode_pattern = r'([UK])\s+' if include_misc else ''
    pattern = re.compile(r'^\s*(\d+)[\s/]+(\d+)\s+' + mode_pattern + r'([0-9]+\.[0-9]+):\s+(\d+)\s+((?:0x)?[0-9a-fA-F]+)\s+(.*?)\s+\((.*)\)\s*$')
    for line in source.splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        match = pattern.fullmatch(line)
        if not match:
            rejected.append(line)
            continue
        groups = list(match.groups())
        mode = groups.pop(2) if include_misc else 'U'
        pid, tid, clock, period, ip, symbol, dso = groups
        context = 'kernel' if mode == 'K' else 'user'
        modes[context] += int(period)
        per_thread[tid][context] += int(period)
        # Kernel symbols must never enter native QEMU/graphics/helper bins.
        label = ('kernel-unresolved' if category(symbol, dso) == 'unresolved' else
                 'kernel-resolved') if mode == 'K' else category(symbol, dso)
        bins[label] += int(period)
        rows.append({'pid': int(pid), 'tid': int(tid), 'monotonic_seconds': clock,
                     'period_ns': int(period), 'ip': ip, 'symbol': symbol, 'dso': dso, 'category': label, 'cpu_mode': context})
    return {'samples': rows, 'period_ns_by_self_category': bins,
        'period_ns_by_cpu_mode': modes, 'period_ns_by_thread_and_cpu_mode': per_thread, 'unparsed_rows': rejected, 'classification_valid': bool(rows) and not rejected,
        'limits': ['Statistical self CPU allocation, not exact elapsed translation time.',
                   'Inclusive reports overlap and must not be added to self bins.',
                   'Unknown/anonymous remains unresolved without recording-time JIT mapping evidence.',
                   'First/warm scene and latency require separately inspected input/frame evidence.',
                   ('User and kernel CPU sampled; kernel symbols may remain unresolved; not off-CPU or GPU wait.'
                    if include_misc else 'Kernel time excluded.'),
                   'Raw lost-event report must be reviewed separately.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='operation', required=True)
    preparation = sub.add_parser('prepare')
    preparation.add_argument('--preset', choices=PRESETS, default='user-40s')
    preparation.add_argument('--pid', type=int, required=True)
    preparation.add_argument('--thread', action='append', required=True, help='exact TID:owner-verified-role')
    preparation.add_argument('--engine', type=Path, required=True)
    preparation.add_argument('--plan', type=Path, required=True)
    preparation.add_argument('--output', type=Path, required=True)
    capture = sub.add_parser('collect')
    capture.add_argument('--plan', type=Path, required=True)
    capture.add_argument('--execute-approved-sha256')
    offline = sub.add_parser('report')
    offline.add_argument('--recording', type=Path, required=True)
    offline.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    {'prepare': prepare, 'collect': collect, 'report': report}[args.operation](args)


if __name__ == '__main__':
    main()
