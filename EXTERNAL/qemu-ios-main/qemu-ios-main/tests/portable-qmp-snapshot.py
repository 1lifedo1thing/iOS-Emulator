#!/usr/bin/env python3
"""Actual engine protocol check for the bounded client and long private paths."""
import argparse
import json
import os
import pathlib
import socket
import struct
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
p.add_argument('runner')
p.add_argument('library')
p.add_argument('client')
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='qmp-snapshot-') as temp:
    root = pathlib.Path(temp) / ('long-private-parent-' * 8)
    root.mkdir()
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    short = f'/proc/self/fd/{fd}/qmp.sock'
    sock = root / 'qmp.sock'
    guest = root / 'legal-arm-loop.bin'
    guest.write_bytes(struct.pack('<I', 0xeafffffe))
    with (root / 'engine.log').open('w') as log:
        process = subprocess.Popen([a.runner, a.library, 'qemu', '-M', 'virt',
            '-cpu', 'cortex-a15', '-m', '16M', '-nodefaults', '-display', 'none',
            '-serial', 'none', '-monitor', 'none', '-device',
            f'loader,file={guest},addr=0x40200000,cpu-num=0', '-qmp',
            f'unix:{short},server=on,wait=off'], pass_fds=(fd,), stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while not sock.exists():
                if process.poll() is not None:
                    raise RuntimeError((root / 'engine.log').read_text())
                if time.monotonic() > deadline:
                    raise TimeoutError('engine startup')
                time.sleep(.01)
            result = subprocess.run([a.client, str(sock)], capture_output=True,
                                    text=True, timeout=15, check=True)
            lines = [json.loads(line) for line in result.stdout.splitlines()]
            assert len(lines) == 8
            by_id = {line.get('id'): line for line in lines}
            assert by_id['lt-status']['return']['running']
            assert len(by_id['lt-cpus']['return']) == 1
            assert 'R15=40200000' in by_id['lt-registers']['return']
            assert 'CPU #0' in by_id['lt-cpu-state']['return']
            # This legal virt guest has no XNU mapping: the fixed RAM reads
            # return a bounded inaccessible-memory report, never MMIO data.
            assert 'return' in by_id['lt-instructions']
            assert 'return' in by_id['lt-caller']
            with socket.socket(socket.AF_UNIX) as connection:
                connection.connect(short)
                stream = connection.makefile('rwb', buffering=0)
                stream.readline()
                stream.write(b'{"execute":"qmp_capabilities"}\n')
                stream.readline()
                stream.write(b'{"execute":"quit"}\n')
            process.wait(timeout=10)
            assert process.returncode == 0
            print('PASS: bounded actual ARM TCG status/cpu/register snapshot PC40200000, logical private socket path >108 bytes, fresh clean exit')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(fd)
