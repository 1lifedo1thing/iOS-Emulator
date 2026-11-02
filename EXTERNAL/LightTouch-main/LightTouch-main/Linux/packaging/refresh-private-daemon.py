#!/usr/bin/env python3
"""Build/test only the private QEMU daemon before collecting a fresh runtime."""
import argparse
from pathlib import Path
import subprocess


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--from', dest='previous', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--evidence', required=True)
    parser.add_argument('collector', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.collector or args.collector[0] != '--' or len(args.collector) < 2:
        parser.error('Expected -- followed by collection command')
    here = Path(__file__).resolve().parent
    subprocess.run(['python3', str(here / 'build-mobile-deps.py'), args.output,
                    '--daemon-only-from', args.previous], check=True)
    subprocess.run(['python3', str(here / 'test-private-daemon.py'),
                    '--daemon', str(Path(args.output) / 'prefix/bin/usbmuxd-qemu'),
                    '--library-path', str(Path(args.output) / 'prefix/lib'),
                    '--evidence', args.evidence], check=True)
    subprocess.run(args.collector[1:], check=True)
