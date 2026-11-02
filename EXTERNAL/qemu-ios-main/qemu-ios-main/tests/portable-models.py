#!/usr/bin/env python3
"""Instantiate the actual n72 board with legal sparse inputs, never guest assets.

The ARM reset program only loops. This checks complete model initialization and
required device registration; it proves no iOS boot, input, audio or graphics.
"""
import argparse
import json
import pathlib
import socket
import struct
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
p.add_argument('runner')
p.add_argument('library')
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='lt-models-') as temp:
    root = pathlib.Path(temp)
    boot = root / 'legal-arm-loop.bin'
    boot.write_bytes(struct.pack('<I', 0xeafffffe))
    nor = root / 'erased-nor.bin'
    nor.write_bytes(b'\xff' * 0x100000)
    nand = root / 'empty-nand'
    nand.mkdir()
    qmp = root / 'qmp.sock'
    args = [a.runner, a.library, 'qemu', '-M',
            f'iPod-Touch,bootrom={boot},nor={nor},nand={nand}',
            '-display', 'none', '-serial', 'none', '-monitor', 'none',
            '-audio', 'driver=none', '-qmp', f'unix:{qmp},server=on,wait=off']
    with (root / 'engine.log').open('w') as log:
        process = subprocess.Popen(args, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 20
            while not qmp.exists():
                if process.poll() is not None:
                    raise RuntimeError('Board initialization exited before QMP')
                if time.monotonic() > deadline:
                    raise TimeoutError('Board initialization timeout')
                time.sleep(.05)
            client = socket.socket(socket.AF_UNIX)
            client.settimeout(10)
            client.connect(str(qmp))
            stream = client.makefile('rwb', buffering=0)
            assert 'QMP' in json.loads(stream.readline())

            def command(name, arguments=None):
                request = {'execute': name}
                if arguments is not None:
                    request['arguments'] = arguments
                stream.write((json.dumps(request) + '\n').encode())
                while True:
                    reply = json.loads(stream.readline())
                    if 'event' in reply:
                        continue
                    if 'error' in reply:
                        raise RuntimeError(reply)
                    return reply['return']

            command('qmp_capabilities')
            status = command('query-status')
            assert status['running'], status
            assert len(command('query-cpus-fast')) == 1
            names = {item['name'] for item in command('qom-list-types')}
            for model in ['unimplemented-device', 'ipodtouch.aes', 'ipodtouch.pke',
                          'ipodtouch.sha1', 'ipodtouch.lcd']:
                assert model in names, model
            command('quit')
            process.wait(timeout=10)
            assert process.returncode == 0, process.returncode
            print('PASS: actual n72 board initializes complete device graph with legal ARM loop, erased NOR and empty NAND; fresh worker clean exit')
        except BaseException:
            print('engine exit:', process.poll(), flush=True)
            print((root / 'engine.log').read_text(), flush=True)
            raise
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
