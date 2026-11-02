#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded readonly memory observations for an allocated Pixel AVD, never a verdict."""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time

SERIAL = 'emulator-5554'
PACKAGE = 'org.lighttouch.android'
HOST_FILES = ('/proc/meminfo', '/proc/vmstat', '/proc/swaps',
              '/proc/pressure/memory', '/proc/pressure/cpu', '/proc/pressure/io')
GUEST_FILES = ('/proc/meminfo', '/proc/vmstat', '/proc/swaps', '/proc/pressure/memory',
               '/proc/pressure/cpu', '/proc/pressure/io')


def read_local(path, reader=lambda p: Path(p).read_text()):
    try:
        return {'available': True, 'text': reader(path)}
    except OSError as error:
        return {'available': False, 'error': str(error), 'text': None}


def host_sample(reader=lambda p: Path(p).read_text(), zram_paths=None):
    paths = zram_paths if zram_paths is not None else sorted(Path('/sys/block').glob('zram*'))
    return {'monotonicNs': time.monotonic_ns(), 'hostPageBytes': os.sysconf('SC_PAGE_SIZE'),
            'files': {p: read_local(p, reader) for p in HOST_FILES},
            'zram': {str(p): {name: read_local(str(p / name), reader) for name in
                     ('disksize', 'comp_algorithm', 'mm_stat', 'io_stat', 'bd_stat')}
                     for p in paths}, 'guestAcceptancePassed': False}


def swap_delta(previous, current, page_bytes):
    def counters(text):
        return {m[0]: int(m[1]) for m in re.findall(r'^(pswpin|pswpout) (\d+)$', text, re.M)}
    a, b = counters(previous), counters(current)
    if set(a) != {'pswpin', 'pswpout'} or set(b) != set(a) or any(b[k] < a[k] for k in a):
        return {'available': False, 'error': 'Missing/reset swap counters'}
    return {'available': True, 'pages': {k: b[k] - a[k] for k in a},
            'bytes': {k: (b[k] - a[k]) * page_bytes for k in a}}


def process_ticks(pid):
    try:
        return int(Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[19])
    except (OSError, ValueError, IndexError):
        return None


def worker_identity(stat, status, cmdline, binding):
    try:
        ticks = int(stat.rsplit(')', 1)[1].split()[19])
        uid = int(re.search(r'^Uid:\s+(\d+)', status, re.M)[1])
        return ticks == binding['workerStartTicks'] and uid == binding['appUid'] and cmdline.rstrip('\x00') == PACKAGE + ':engine'
    except (ValueError, IndexError, TypeError, KeyError):
        return False


class Sampler:
    def __init__(self, adb, owner_pid, owner_ticks, deadline_ns, run=subprocess.run,
                 now=time.monotonic_ns, identity=process_ticks, disabled=lambda: False):
        self.adb, self.owner_pid, self.owner_ticks = adb, owner_pid, owner_ticks
        self.deadline_ns, self.run, self.now, self.identity = deadline_ns, run, now, identity
        self.disabled = disabled

    def remote(self, path, app=False):
        if self.disabled():
            return {'available': False, 'error': 'Guest observation disabled for owned cleanup', 'text': None}
        if self.identity(self.owner_pid) != self.owner_ticks:
            return {'available': False, 'error': 'Owned AVD wrapper gone/reused', 'text': None}
        remaining = (self.deadline_ns - self.now()) / 1e9
        if remaining <= 0:
            return {'available': False, 'error': 'Original deadline expired', 'text': None}
        marker = '__LT_MEMORY_EXIT__='
        script = f'head -c 131073 {shlex.quote(path)}; lt_mem_exit=$?; printf "\\n{marker}%s\\n" "$lt_mem_exit"'
        prefix = ['run-as', PACKAGE, 'sh', '-c', script] if app else ['sh', '-c', script]
        argv = [self.adb, '-s', SERIAL, 'exec-out', shlex.join(prefix)]
        before = self.now()
        try:
            result = self.run(argv, capture_output=True, timeout=min(.75, remaining))
        except subprocess.TimeoutExpired as error:
            return {'available': False, 'error': 'ADB timeout', 'text': None,
                    'partial': (error.stdout or b'')[:131072].decode(errors='replace'), 'beginNs': before, 'endNs': self.now()}
        match = re.search(rb'\n__LT_MEMORY_EXIT__=(\d+)\n$', result.stdout)
        data = result.stdout[:match.start()] if match else result.stdout
        remote_exit = int(match[1]) if match else None
        ok = result.returncode == 0 and remote_exit == 0 and len(data) <= 131072
        return {'available': ok, 'text': data.decode(errors='replace') if ok else None,
                'partial': None if ok else data[:131072].decode(errors='replace'),
                'adbExit': result.returncode, 'remoteExit': remote_exit,
                'stderr': result.stderr.decode(errors='replace'), 'beginNs': before, 'endNs': self.now()}

    def guest(self, binding=None):
        result = {'files': {p: self.remote(p) for p in GUEST_FILES}, 'worker': None}
        if binding is None:
            return result
        pid = binding.get('workerPid')
        if type(pid) is not int or pid < 2:
            result['worker'] = {'available': False, 'error': 'Invalid bound worker PID'}
            return result
        boot = self.remote('/proc/sys/kernel/random/boot_id')
        selected = self.remote('files/library/selected', True)
        if not (boot['available'] and selected['available'] and boot['text'].strip() == binding.get('bootId') and selected['text'].strip() == binding.get('recordId')):
            result['worker'] = {'available': False, 'error': 'Boot/selected record identity mismatch or unavailable', 'boot': boot, 'selected': selected}
            return result
        before = {name: self.remote(f'/proc/{pid}/{name}', True) for name in ('stat', 'status', 'cmdline')}
        valid = all(v['available'] for v in before.values()) and worker_identity(
            before['stat']['text'], before['status']['text'], before['cmdline']['text'], binding)
        result['worker'] = {'identityBefore': before, 'identityMatched': valid, 'smapsRollup': None}
        if valid:
            rollup = self.remote(f'/proc/{pid}/smaps_rollup', True)
            after = {name: self.remote(f'/proc/{pid}/{name}', True) for name in ('stat', 'status', 'cmdline')}
            stable = all(v['available'] for v in after.values()) and worker_identity(
                after['stat']['text'], after['status']['text'], after['cmdline']['text'], binding)
            result['worker'].update(identityAfter=after, identityMatched=stable,
                                   smapsRollup=rollup, attributionValid=stable)
        return result


def monitor(adb, output, owner_pid, owner_ticks, origin_ns, deadline_ns, stop_event, guest_disable, guest_idle, cadence=2):
    """Requested cadence; guest reads quiesce before cleanup, host reads continue."""
    os.setsid()  # Parent may retire this monitor and its ADB child as one owned group.
    output = Path(output)
    sampler = Sampler(adb, owner_pid, owner_ticks, deadline_ns, disabled=guest_disable.is_set)
    previous = None
    while time.monotonic_ns() < deadline_ns and not stop_event.is_set():
        before = time.monotonic_ns()
        sample = {'beginNs': before, 'elapsedSeconds': (before - origin_ns) / 1e9,
                  'host': host_sample(), 'phase': read_local(str(output / 'memory-phase.txt'))}
        current = sample['host']['files']['/proc/vmstat']
        if previous and previous['available'] and current['available']:
            sample['hostSwapDelta'] = swap_delta(previous['text'], current['text'], sample['host']['hostPageBytes'])
        previous = current
        try:
            paths=[output/name for name in ('pixel-bound.json','recovery-record-bound.json','normal-record-bound.json') if (output/name).exists()]
            binding = json.loads(paths[0].read_text()) if paths else None
            if binding is None: raise ValueError('No actual worker binding yet')
            if binding['originNs'] != origin_ns:
                raise ValueError('Worker binding original origin differs')
        except (OSError, ValueError, KeyError):
            binding = None
        sample['guest'] = sampler.guest(binding) if not guest_disable.is_set() else {'available': False, 'error': 'Cleanup: host observations only'}
        if guest_disable.is_set():
            guest_idle.set()
        sample.update(endNs=time.monotonic_ns(), guestAcceptancePassed=False)
        with (output / 'memory-continuous.jsonl').open('a') as stream:
            stream.write(json.dumps(sample) + '\n')
        wait_seconds = max(0, cadence - (time.monotonic_ns() - before) / 1e9)
        if guest_disable.is_set():
            stop_event.wait(wait_seconds)
        else:
            guest_disable.wait(wait_seconds)  # Wake promptly at the cleanup barrier.
