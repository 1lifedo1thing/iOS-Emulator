#!/usr/bin/env python3
"""Run one reviewed finite test independently of its invoking terminal/session.

This grants no device ownership. The invoked controller must retain its own
independent device shutdown guard; terminating a host controller is not proof
that its device/guest stopped. No shell expansion, retries or deadline renewal.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.pending')
    with temporary.open('x') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    descriptor = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def boot_id():
    return Path('/proc/sys/kernel/random/boot_id').read_text().strip()


def validate(plan, now):
    require(plan.get('execution_authorized') is True, 'execution not authorized')
    command = plan.get('argv')
    require(isinstance(command, list) and command and all(isinstance(x, str) and '\0' not in x for x in command), 'argv')
    require(Path(command[0]).is_absolute(), 'absolute executable required')
    require(Path(plan.get('cwd', '')).is_absolute() and Path(plan['cwd']).is_dir(), 'cwd')
    require(type(plan.get('timeout_seconds')) in (int, float) and 0 < plan['timeout_seconds'] <= 1800, 'finite timeout <=1800s required')
    require(type(plan.get('admission_deadline_ns')) is int and 0 < plan['admission_deadline_ns'] - now <= 90_000_000_000, 'original finite admission deadline')
    pins = plan.get('sha256')
    require(isinstance(pins, dict) and pins and command[0] in pins, 'executable/source pins required')
    for path, expected in pins.items():
        require(Path(path).is_absolute() and digest(path) == expected, 'source pin ' + path)


def supervise(envelope, out):
    started = None
    child = None
    result = dict(success=False, deviceShutdownVerified=False, automaticRetry=False)
    try:
        require(envelope['boot_id'] == boot_id(), 'host reboot; original clocks invalid')
        validate(envelope['plan'], time.monotonic_ns())
        require(envelope['supervisor_sha256'] == digest(__file__), 'supervisor source changed')
        require(time.monotonic_ns() < envelope['hard_deadline_ns'], 'original hard deadline expired')
        save(out / 'supervisor.json', dict(pid=os.getpid(), session=os.getsid(0), boot_id=boot_id(),
            proc_stat=Path('/proc/self/stat').read_text(), hard_deadline_ns=envelope['hard_deadline_ns']))
        with (out / 'stdout').open('xb') as stdout, (out / 'stderr').open('xb') as stderr:
            started = time.monotonic_ns()
            child = subprocess.Popen(envelope['plan']['argv'], cwd=envelope['plan']['cwd'],
                stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                start_new_session=True, close_fds=True)
            save(out / 'controller.json', dict(pid=child.pid, session=os.getsid(child.pid),
                proc_stat=Path(f'/proc/{child.pid}/stat').read_text(), issued_ns=started,
                hard_deadline_ns=envelope['hard_deadline_ns'], argv=envelope['plan']['argv']))
            remaining = (envelope['hard_deadline_ns'] - time.monotonic_ns()) / 1e9
            require(remaining > 0, 'original hard deadline expired after launch')
            try:
                code = child.wait(timeout=remaining)
                result.update(success=code == 0, returncode=code, timed_out=False)
            except subprocess.TimeoutExpired:
                # This process group belongs solely to the child we have not reaped.
                # Independent shutdown guards must use their own detached session.
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=3)
                result.update(returncode=child.returncode, timed_out=True,
                    error='original host hard deadline reached; device state unknown')
            stdout.flush(); os.fsync(stdout.fileno())
            stderr.flush(); os.fsync(stderr.fileno())
    except BaseException as error:
        result['error'] = repr(error)
        if child is not None and child.poll() is None:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait(timeout=3)
    finally:
        result.update(issued_ns=started, completed_ns=time.monotonic_ns(),
            original_hard_deadline_ns=envelope['hard_deadline_ns'])
        save(out / 'result.json', result)
    return 0 if result['success'] else 1


def launch(plan_path, plan_sha, out):
    require(digest(plan_path) == plan_sha, 'plan pin')
    plan = json.loads(Path(plan_path).read_text())
    admission = time.monotonic_ns()
    validate(plan, admission)
    # O_EXCL directory admission: this exact output can never be restarted.
    # Operators must not create a new output to retry uncertain device actions.
    out.mkdir(mode=0o700, parents=False, exist_ok=False)
    envelope = dict(plan=plan, plan_sha256=plan_sha, boot_id=boot_id(),
        supervisor_sha256=digest(__file__), admitted_ns=admission,
        hard_deadline_ns=admission + int(plan['timeout_seconds'] * 1e9))
    save(out / 'envelope.json', envelope)
    with (out / 'supervisor.log').open('xb') as log:
        command = [sys.executable, str(Path(__file__).resolve()),
            '--child', str(out / 'envelope.json'), '--sha256', digest(out / 'envelope.json'),
            '--out', str(out)]
        # spawn rather than a Popen handle: the launching CLI intentionally does
        # not own/wait on this detached supervisor. Python-created descriptors
        # are CLOEXEC; only these explicit standard streams cross exec.
        pid = os.posix_spawn(sys.executable, command, os.environ, setsid=True,
            file_actions=[(os.POSIX_SPAWN_OPEN, 0, '/dev/null', os.O_RDONLY, 0),
                (os.POSIX_SPAWN_DUP2, log.fileno(), 1), (os.POSIX_SPAWN_DUP2, log.fileno(), 2)])
    save(out / 'launched.json', dict(supervisor_pid=pid,
        hard_deadline_ns=envelope['hard_deadline_ns'], admitted_ns=admission,
        started=False, acceptance=False, note='Launch only; inspect controller and result receipts'))
    return pid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', type=Path)
    parser.add_argument('--child', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--sha256')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if args.child:
        require(digest(args.child) == args.sha256, 'envelope pin')
        return supervise(json.loads(args.child.read_text()), args.out.resolve())
    if args.execute:
        require(args.out is not None and args.out.is_absolute(), 'absolute output required')
        print(json.dumps(dict(supervisor_pid=launch(args.execute, args.sha256, args.out))))
    else:
        print('REVIEW ONLY; explicit hash-pinned --execute plan required')
    return 0


if __name__ == '__main__':
    sys.exit(main())
