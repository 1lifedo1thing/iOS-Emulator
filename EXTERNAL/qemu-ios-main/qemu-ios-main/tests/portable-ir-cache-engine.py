#!/usr/bin/env python3
"""Real generated-code ARM1176 cache differential; no iOS assets or speed claim.

Uses one pinned qemu-system-arm executable or exact shared library through its
qemu_ios_main entry, machine=none and16MiB RAM. No compilation required. No stepping
or breakpoints (those disable caching). QMP stops a normal running guest, then
GDB observes register/RAM results. Every case starts a fresh process. The owner
must separately allocate the host/CPU slot; --execute is required for execution.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

COUNTERS = re.compile(r'^LightTouch TCG IR cache: hits=(\d+) misses=(\d+) rejected=(\d+) stored=(\d+); frontend IR only, fresh native code$', re.M)
CODE_PC = 0x1034
TARGET_PC = 0x1200  # Same code page: normal direct TB chaining is exercised.


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def counters(log):
    rows = [dict(zip(('hits', 'misses', 'rejected', 'stored'), map(int, m)))
            for m in COUNTERS.findall(log)]
    for previous, current in zip(rows, rows[1:]):
        if any(current[k] < previous[k] for k in current):
            raise ValueError('Mixed/reset cache counter epochs')
    return rows[-1] if rows else None


def words(subtract=False):
    # MOV r1,#7; ADD/SUB r0,r0,r1; MOV r2,#0x3000; STR r0,[r2]; B target.
    branch = 0xea000000 | ((TARGET_PC - (CODE_PC + 16 + 8)) >> 2)
    return struct.pack('<5I', 0xe3a01007,
                       0xe0400001 if subtract else 0xe0800001,
                       0xe3a02a03, 0xe5820000, branch)


class GDB:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(2)
        self.socket.connect(str(path))

    def byte(self):
        value = self.socket.recv(1)
        if not value:
            raise RuntimeError('GDB disconnected')
        return value

    def packet(self, command):
        body = command.encode()
        self.socket.sendall(b'$' + body + b'#' + f'{sum(body) & 255:02x}'.encode())
        while self.byte() != b'$':
            pass
        answer = bytearray()
        while (value := self.byte()) != b'#':
            answer += value
        checksum = self.byte() + self.byte()
        if int(checksum, 16) != sum(answer) & 255:
            raise RuntimeError('GDB checksum mismatch')
        self.socket.sendall(b'+')
        text = answer.decode()
        if text.startswith('E'):
            raise RuntimeError('GDB error ' + text)
        return text

    def set(self, reg, value):
        if self.packet(f'P{reg:x}=' + struct.pack('<I', value).hex()) != 'OK':
            raise RuntimeError('GDB register write rejected')

    def get(self, reg):
        value = bytes.fromhex(self.packet(f'p{reg:x}'))
        if len(value) != 4:
            raise RuntimeError('Unexpected GDB register width')
        return int.from_bytes(value, 'little')

    def write(self, address, value):
        if self.packet(f'M{address:x},{len(value):x}:' + value.hex()) != 'OK':
            raise RuntimeError('GDB memory write rejected')

    def read(self, address, size):
        value = bytes.fromhex(self.packet(f'm{address:x},{size:x}'))
        if len(value) != size:
            raise RuntimeError('GDB short memory read')
        return value


class QMP:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(2)
        self.socket.connect(str(path))
        self.stream = self.socket.makefile('rwb', buffering=0)
        greeting = json.loads(self.stream.readline())
        if 'QMP' not in greeting:
            raise RuntimeError('Missing QMP greeting')
        self.command('qmp_capabilities')

    def command(self, name):
        self.stream.write((json.dumps({'execute': name}) + '\n').encode())
        while True:
            line = self.stream.readline()
            if not line:
                raise RuntimeError('QMP disconnected')
            answer = json.loads(line)
            if 'event' in answer:
                continue
            if 'error' in answer:
                raise RuntimeError('QMP error ' + str(answer))
            return answer['return']


def proc_identity(pid):
    text = Path(f'/proc/{pid}/stat').read_text()
    fields = text[text.rfind(')') + 2:].split()
    return {'pid': pid, 'startTicks': fields[19],
            'hostBootId': Path('/proc/sys/kernel/random/boot_id').read_text().strip()}


def run_until_target(qmp, observe, *, clock=time.monotonic, sleep=time.sleep):
    """Observe normal execution completion, allowing first-use initialization.

    A short fixed run can stop during the mapped-engine identity hash before
    the first instruction executes. Each observation stops ordinary execution;
    it does not single-step, install breakpoints, or change TB eligibility.
    """
    deadline = clock() + 3
    observations = []
    while clock() < deadline:
        qmp.command('cont')
        sleep(.04)
        qmp.command('stop')
        state = observe()
        observations.append(state)
        if state['pc'] == TARGET_PC:
            return state, observations
    raise TimeoutError('Normal execution did not reach target loop: ' +
                       str(observations))


def launch_arguments(qemu, expected_hash, arguments, library_entry=None, entry_hash=None):
    if digest(qemu) != expected_hash:
        raise RuntimeError('Engine identity changed')
    if library_entry is None:
        return [str(qemu), *arguments]
    if not entry_hash or digest(library_entry) != entry_hash:
        raise RuntimeError('Pinned shared-library adapter changed')
    return [sys.executable, str(library_entry), '--library', str(qemu),
            '--sha256', expected_hash, '--', *arguments]


def shared_mapping(log, qemu, expected_hash, pid):
    entries = [json.loads(line.split(' ', 1)[1]) for line in log.splitlines()
               if line.startswith('SHARED_ENGINE_MAPPING ')]
    if len(entries) != 1:
        raise RuntimeError('Exactly one actual shared engine mapping witness required')
    entry = entries[0]
    info = qemu.stat()
    device = '%02x:%02x' % (os.major(info.st_dev), os.minor(info.st_dev))
    if (entry.get('path') != str(qemu) or entry.get('sha256') != expected_hash
            or entry.get('pid') != pid or entry.get('inode') != info.st_ino
            or entry.get('device') != device or entry.get('entrypoint') != 'qemu_ios_main'
            or not entry.get('maps')):
        raise RuntimeError('Shared engine mapping identity mismatch')
    for line in entry['maps']:
        pieces = line.split()
        if (len(pieces) < 6 or pieces[3] != device or pieces[4] != str(info.st_ino)
                or not line.endswith(' ' + str(qemu))):
            raise RuntimeError('Recorded shared engine mapping does not match artifact')
    return entry


def trial(qemu, expected_hash, output, cache, *, enabled, mode=0x13,
          subtract=False, self_modify=False, library_entry=None, entry_hash=None):
    if digest(qemu) != expected_hash:
        raise RuntimeError('Engine identity changed')
    output.mkdir(mode=0o700, exist_ok=False)
    started = time.monotonic()
    result = {'scope': 'REAL_TCG_ASSET_FREE_CORRECTNESS_ONLY', 'engineSHA256': expected_hash,
              'enabled': enabled, 'cpuMode': mode, 'subtract': subtract,
              'selfModify': self_modify, 'cases': []}
    process = qmp = gdb = None
    with tempfile.TemporaryDirectory(prefix='ltir-') as temporary, (output / 'engine.log').open('wb') as log:
        root = Path(temporary)
        qpath, gpath = root / 'q', root / 'g'
        arguments = ['-M', 'none', '-cpu', 'arm1176,has_el3=off',
                '-m', '16M', '-accel', 'tcg,thread=single,tb-size=8', '-nodefaults',
                '-display', 'none', '-serial', 'none', '-monitor', 'none', '-S',
                '-qmp', f'unix:{qpath},server=on,wait=off',
                '-gdb', f'unix:{gpath},server=on,wait=off']
        argv = launch_arguments(qemu, expected_hash, arguments, library_entry, entry_hash)
        environment = dict(os.environ)
        for key in ('LT_TCG_IR_CACHE_ENABLE', 'LT_TCG_IR_CACHE', 'LT_DYNARMIC_PROBE', 'IT_VCPU_ACCT', 'IT_VCPU_ACCT_LOG'):
            environment.pop(key, None)
        if enabled:
            environment.update(LT_TCG_IR_CACHE_ENABLE='experimental-v1', LT_TCG_IR_CACHE=str(cache))
        (output / 'command.json').write_text(json.dumps({'argv': argv, 'cacheEnvironment':
            {k: environment.get(k) for k in ('LT_TCG_IR_CACHE_ENABLE', 'LT_TCG_IR_CACHE')}}, indent=2) + '\n')
        try:
            process = subprocess.Popen(argv, env=environment, stdout=log, stderr=log,
                                       start_new_session=True)
            result['identity'] = proc_identity(process.pid)
            while not qpath.exists() or not gpath.exists():
                if process.poll() is not None:
                    raise RuntimeError('Engine exited before debug sockets')
                if time.monotonic() - started > 5:
                    raise TimeoutError('Debug socket deadline')
                time.sleep(.01)
            qmp, gdb = QMP(qpath), GDB(gpath)
            gdb.write(CODE_PC, words(subtract))
            gdb.write(TARGET_PC, struct.pack('<I', 0xeafffffe))
            for seed in (19, 111):
                if time.monotonic() - started > 12:
                    raise TimeoutError('Trial deadline')
                if self_modify and seed == 111:
                    # The first execution's translated block must be invalidated
                    # by the ordinary debug-memory write before a cached replay.
                    subtract = True
                    gdb.write(CODE_PC, words(subtract))
                gdb.set(25, mode)
                gdb.set(0, seed)
                gdb.set(15, CODE_PC)
                gdb.write(0x3000, b'\0' * 4)
                begin = time.monotonic_ns()
                # Read only after QMP stop; no single-step or breakpoint cflags.
                def observe():
                    return {'r0': gdb.get(0), 'r1': gdb.get(1), 'r2': gdb.get(2),
                            'pc': gdb.get(15), 'cpsr': gdb.get(25),
                            'ram': int.from_bytes(gdb.read(0x3000, 4), 'little')}
                state, observations = run_until_target(qmp, observe)
                stopped = time.monotonic_ns()
                expected = (seed - 7 if subtract else seed + 7) & 0xffffffff
                if state != {'r0': expected, 'r1': 7, 'r2': 0x3000, 'pc': TARGET_PC,
                             'cpsr': mode, 'ram': expected}:
                    raise RuntimeError('Actual generated-code state differs: ' + str(state))
                result['cases'].append({'seed': seed, 'actual': state,
                    'codeSHA256': hashlib.sha256(words(subtract)).hexdigest(),
                    'stoppedObservations': observations,
                    'continueBeginNS': begin, 'stoppedNS': stopped,
                    'limits': 'Completion polling includes startup/sleep/debug transport; not a speed metric.'})
            qmp.command('quit')
            exit_code = process.wait(timeout=3)
            if exit_code:
                raise RuntimeError('Engine nonzero exit ' + str(exit_code))
            result.update(status='PASS', engineExitCode=exit_code)
        except Exception as exc:
            result.update(status='FAIL', error=str(exc))
        finally:
            if gdb:
                gdb.socket.close()
            if qmp:
                qmp.stream.close(); qmp.socket.close()
            if process and process.poll() is None:
                result['forcedRetirement'] = True
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL); process.wait(timeout=1)
            if process:
                result['engineExitCode'] = process.returncode
                result['processRetired'] = process.poll() is not None
            result['elapsedSeconds'] = time.monotonic() - started
    text = (output / 'engine.log').read_text(errors='replace')
    result['cacheCounters'] = counters(text)
    result['cacheEnabledWitness'] = 'LightTouch TCG IR cache: experimental enabled,' in text
    if library_entry is not None:
        result['artifactKind'] = 'shared-library-qemu_ios_main'
        result['libraryEntrySHA256'] = entry_hash
        try:
            result['sharedEngineMappingWitness'] = shared_mapping(text, qemu, expected_hash, result['identity']['pid'])
        except (RuntimeError, ValueError, KeyError) as error:
            result.update(status='FAIL', mappingError=str(error))
    (output / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    if result['status'] != 'PASS':
        raise RuntimeError('Trial failed: ' + str(output))
    return result


def validate_matrix(rows):
    if any(row['status'] != 'PASS' or not row['processRetired'] for row in rows.values()):
        raise ValueError('A real engine trial failed or remains live')
    identities = [tuple(sorted(row['identity'].items())) for row in rows.values()]
    if len(identities) != len(set(identities)):
        raise ValueError('Trials did not use fresh processes')
    if len({row['engineSHA256'] for row in rows.values()}) != 1:
        raise ValueError('Different engine identities')
    if rows['off']['cacheCounters'] or rows['off']['cacheEnabledWitness']:
        raise ValueError('Default-off unexpectedly enabled')
    for name in ('cold', 'reuse', 'different_cpu_mode', 'changed_guest_bytes', 'self_modifying_code'):
        if not rows[name]['cacheEnabledWitness'] or not rows[name]['cacheCounters']:
            raise ValueError('Missing actual cache witness: ' + name)
    if rows['cold']['cacheCounters']['stored'] < 2:
        raise ValueError('Cold process must store arithmetic/memory and target-loop IR')
    if rows['reuse']['cacheCounters']['hits'] < 2:
        raise ValueError('No fresh-process hits for both arithmetic/memory and target-loop IR')
    # Each process executes two identical seeds; dynamic state must not enter key.
    if rows['different_cpu_mode']['cacheCounters']['hits']:
        raise ValueError('Changed mode unexpectedly reused prior-context IR')
    if not rows['different_cpu_mode']['cacheCounters']['stored']:
        raise ValueError('Changed mode did not translate/store')
    if not rows['changed_guest_bytes']['cacheCounters']['stored']:
        raise ValueError('Changed bytes did not translate/store')
    if not rows['self_modifying_code'].get('selfModify') or rows['self_modifying_code']['cacheCounters']['hits'] < 2:
        raise ValueError('Missing actual self-modification/invalidation cache replay')
    return {'status': 'PASS', 'scope': 'REAL_GENERATED_CODE_CORRECTNESS_ONLY',
            'iosAcceptance': False, 'performanceQualified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    engine = parser.add_mutually_exclusive_group(required=True)
    engine.add_argument('--qemu', type=Path)
    engine.add_argument('--shared-library', type=Path)
    parser.add_argument('--library-entry', type=Path, default=Path(__file__).with_name('portable-ir-cache-library-entry.py'))
    parser.add_argument('--library-entry-sha256')
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    selected = args.shared_library or args.qemu
    library_entry = args.library_entry.resolve() if args.shared_library else None
    if args.shared_library and not re.fullmatch('[a-f0-9]{64}', args.library_entry_sha256 or ''):
        raise ValueError('Pinned library entry SHA256 required')
    if not args.execute:
        print(json.dumps({'execution': False, 'scope': 'plan-only', 'qemu': str(selected),
                          'artifactKind': 'shared-library-qemu_ios_main' if library_entry else 'executable',
                          'libraryEntry': str(library_entry) if library_entry else None,
                          'engineSHA256': args.sha256, 'cases': ['off', 'cold', 'reuse', 'different_cpu_mode', 'changed_guest_bytes', 'self_modifying_code']}))
        return
    qemu, output = selected.resolve(), args.output.resolve()
    if not re.fullmatch('[a-f0-9]{64}', args.sha256) or digest(qemu) != args.sha256:
        raise ValueError('Exact engine SHA256 required')
    output.mkdir(mode=0o700, exist_ok=False)
    cache = output / 'cache'; cache.mkdir(mode=0o700)
    rows = {}
    variants = [('off', False, 0x13, False), ('cold', True, 0x13, False),
                ('reuse', True, 0x13, False), ('different_cpu_mode', True, 0x10, False),
                ('changed_guest_bytes', True, 0x13, True),
                ('self_modifying_code', True, 0x13, False)]
    for name, enabled, mode, subtract in variants:
        if name == 'cold' and any(cache.iterdir()):
            raise ValueError('Cold cache directory is not initially empty')
        rows[name] = trial(qemu, args.sha256, output / name, cache,
                           enabled=enabled, mode=mode, subtract=subtract,
                           self_modify=name == 'self_modifying_code',
                           library_entry=library_entry, entry_hash=args.library_entry_sha256)
        (output / 'PROGRESS.json').write_text(json.dumps(rows, indent=2) + '\n')
    result = validate_matrix(rows)
    result['trials'] = rows
    (output / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
