#!/usr/bin/env python3
"""Offline binding for the opt-in instrumentation keyboard workload. No ADB."""
import argparse
import hashlib
import json
import math
from pathlib import Path


def prepare(before, image_path, scratch, view, rotation, letter, backspace, anchors,
            scratch_approved=False, preserves_existing=False):
    from PIL import Image
    if not scratch_approved or not preserves_existing or not 1 <= len(scratch) <= 120:
        raise ValueError('Inspect and approve a new scratch note; preserve existing notes')
    spec = before['spec']
    pid, uid, start = spec['workerPid'], spec['workerUid'], before['workerStartTicks']
    if any(type(v) is not int or v <= 0 for v in (pid, uid, start)):
        raise ValueError('Exact current worker PID/UID/start required')
    if len(view) != 2 or any(type(v) is not int or not 1 <= v <= 4096 for v in view) or rotation not in range(4):
        raise ValueError('Actual view geometry/rotation required')
    for point in (letter, backspace):
        if len(point) != 2 or any(not math.isfinite(v) or not 0 < v < 1 for v in point):
            raise ValueError('Keys use normalized native guest coordinates inside (0,1)')
    if letter == backspace or not 2 <= len(anchors) <= 8:
        raise ValueError('Distinct inspected keys and two to eight stable keyboard anchors required')
    with Image.open(image_path) as original:
        if any(not 1 <= v <= 4096 for v in original.size):
            raise ValueError('Guest image dimensions out of bounds')
        image = original.convert('RGBA')
        checked = []
        pixels = 0
        for box in anchors:
            if len(box) != 4 or any(type(v) is not int for v in box):
                raise ValueError('Anchor rectangle must have four integer edges')
            left, top, right, bottom = box
            if not 0 <= left < right <= image.width or not 0 <= top < bottom <= image.height:
                raise ValueError('Anchor outside actual guest image')
            pixels += (right-left)*(bottom-top)
            checked.append({'bounds': list(box), 'rgbaSha256': hashlib.sha256(image.crop(box).tobytes()).hexdigest()})
        if pixels > 65536:
            raise ValueError('Anchor pixel budget exceeded')
        return {'schema': 1, 'scratchNoteApproved': True, 'preservesExistingTestNote': True,
                'scratchNoteIdentity': scratch, 'reviewedGuestCaptureSha256': hashlib.sha256(image_path.read_bytes()).hexdigest(),
                'workerPid': pid, 'workerUid': uid, 'workerStartTicks': start,
                'view': list(view), 'frame': list(image.size), 'rotation': rotation,
                'letter': list(letter), 'backspace': list(backspace), 'anchors': checked}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', required=True, type=Path)
    parser.add_argument('--guest-image', required=True, type=Path)
    parser.add_argument('--scratch-identity', required=True)
    parser.add_argument('--view', nargs=2, type=int, required=True)
    parser.add_argument('--rotation', type=int, required=True)
    parser.add_argument('--letter', nargs=2, type=float, required=True)
    parser.add_argument('--backspace', nargs=2, type=float, required=True)
    parser.add_argument('--anchor', nargs=4, type=int, action='append', required=True)
    parser.add_argument('--scratch-approved', action='store_true')
    parser.add_argument('--preserves-existing-test-note', action='store_true')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if any((parent/'.git').exists() for parent in (args.out.resolve(), *args.out.resolve().parents)):
        parser.error('Keep private scene/worker bindings outside Git')
    binding = prepare(json.loads(args.before.read_text()), args.guest_image, args.scratch_identity,
                      args.view, args.rotation, args.letter, args.backspace, args.anchor,
                      args.scratch_approved, args.preserves_existing_test_note)
    with args.out.open('x') as output:
        json.dump(binding, output, separators=(',', ':'))
        output.write('\n')
    print('OFFLINE_BINDING_ONLY: actual guest scene/keyboard review is the operator claim; no target pass')


if __name__ == '__main__':
    main()
