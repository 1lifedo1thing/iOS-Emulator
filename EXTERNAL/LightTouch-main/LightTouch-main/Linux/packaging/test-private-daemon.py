#!/usr/bin/env python3
"""Exercise the actual private QEMU daemon without a guest or physical USB."""
import argparse
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--daemon', type=Path, required=True)
    parser.add_argument('--library-path', required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=False)
    results = []
    for original_mode in (0o700, 0o2775):
        with tempfile.TemporaryDirectory(prefix='lighttouch-private-daemon-') as scratch:
            root = Path(scratch)
            pairing = root / 'pairing'
            pairing.mkdir()
            pairing.chmod(original_mode)
            original = pairing.stat()
            mux, endpoint = root / 'mux.sock', root / 'qemu.sock'
            env = dict(os.environ, USBMUXD_QEMU_ADDR=str(endpoint), LD_LIBRARY_PATH=args.library_path)
            log = args.evidence / ('mode-%04o.log' % original_mode)
            with log.open('xb') as stream:
                child = subprocess.Popen([str(args.daemon), '-f', '-S', str(mux), '-C', str(pairing),
                                          '-P', str(root / 'daemon.pid')], env=env, stdin=subprocess.DEVNULL,
                                         stdout=stream, stderr=subprocess.STDOUT, close_fds=True)
                try:
                    deadline = time.monotonic() + 5
                    while not (mux.exists() and endpoint.exists()):
                        if child.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError('Actual daemon failed private listener admission; inspect ' + str(log))
                        time.sleep(.02)
                    during = pairing.stat()
                    assert stat.S_IMODE(during.st_mode) == 0o700
                    assert (during.st_dev, during.st_ino, during.st_uid) == (original.st_dev, original.st_ino, original.st_uid)
                    for socket in (mux, endpoint):
                        value = socket.lstat()
                        assert stat.S_ISSOCK(value.st_mode) and value.st_uid == os.geteuid()
                        assert not stat.S_IMODE(value.st_mode) & 0o077
                        assert stat.S_IMODE(value.st_mode) & 0o600 == 0o600
                    child.send_signal(signal.SIGTERM)
                    child.wait(timeout=5)
                    assert child.returncode == 0
                    after = pairing.stat()
                    assert stat.S_IMODE(after.st_mode) == 0o700
                    assert (after.st_dev, after.st_ino, after.st_uid) == (original.st_dev, original.st_ino, original.st_uid)
                    results.append(dict(original_mode=oct(original_mode), during_mode=oct(stat.S_IMODE(during.st_mode)),
                                        after_mode=oct(stat.S_IMODE(after.st_mode)), owner_uid=after.st_uid,
                                        exit_code=child.returncode, process_reaped=True))
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait(timeout=5)
    result = dict(passed=True, actual_daemon=str(args.daemon), cases=results,
                  guest_or_physical_usb_used=False)
    (args.evidence / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
