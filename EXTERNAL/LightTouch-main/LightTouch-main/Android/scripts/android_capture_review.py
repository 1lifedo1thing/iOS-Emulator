"""Validate a personally inspected immutable capture before a finite UI action.

The caller still checks the live worker, current UI and its original input
deadline. A newer capture does not revoke a reviewed capture; an intervening
guest action, changed geometry/context, or expired review does. No function in
this module reads a mutable latest-capture pointer or authorizes an action.
"""
import hashlib
import json
import math
import os
import pathlib
import re
import stat


class CaptureReviewError(ValueError):
    pass


CONTEXT_KEYS = {'recordId', 'workerPid', 'workerStartTicks', 'bootId',
                'controllerSha256', 'runOriginMachNs', 'guestStartMachNs',
                'guestInputEpoch', 'displayBounds'}
MANIFEST_KEYS = {'schemaVersion', 'index', 'context', 'capturedMachNs',
                 'native', 'nativeSha256', 'nativeBytes',
                 'surface', 'surfaceSha256', 'surfaceBytes'}


def _require(condition, message):
    if not condition:
        raise CaptureReviewError(message)


def _integer(value, minimum=0):
    return type(value) is int and value >= minimum


def _sha(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def _context(value):
    _require(type(value) is dict and set(value) == CONTEXT_KEYS, 'Invalid capture context keys')
    for key in ['recordId', 'bootId']:
        _require(type(value[key]) is str and bool(value[key]), 'Invalid ' + key)
    _require(_sha(value['controllerSha256']), 'Invalid controller source hash')
    for key in ['workerPid', 'workerStartTicks', 'runOriginMachNs', 'guestStartMachNs']:
        _require(_integer(value[key], 1), 'Invalid ' + key)
    _require(_integer(value['guestInputEpoch']), 'Invalid guest input epoch')
    _require(value['guestStartMachNs'] >= value['runOriginMachNs'], 'Guest start predates run')
    bounds = value['displayBounds']
    match = re.fullmatch(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]', bounds) if isinstance(bounds, str) else None
    _require(match is not None and int(match[3]) > int(match[1])
             and int(match[4]) > int(match[2]), 'Invalid display bounds')


def _read_regular(directory_fd, name, limit):
    """Names are already restricted; no symlink following or path traversal."""
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory_fd)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        _require(stat.S_ISREG(info.st_mode) and info.st_size <= limit,
                 'Expected bounded regular capture file')
        raw = stream.read(limit + 1)
        _require(len(raw) == info.st_size, 'Capture file changed during read')
        return raw


def validate_reviewed_capture(directory, gate, expected_context, now_ns,
                              input_deadline_ns, action, scene, normalized_tap=None):
    """Return the exact stored manifest after all immutable review checks pass.

    A gate contains action, personallyViewedScene, captureManifest and its hash,
    plus normalizedTap only for actions with an existing fixed tap contract.
    Context includes the current guest-input epoch, so a pre-action capture
    cannot be reused after an intervening wake, unlock or menu action.
    """
    try:
        keys = {'action', 'personallyViewedScene', 'captureManifest', 'captureManifestSha256'}
        if normalized_tap is not None:
            keys.add('normalizedTap')
        _require(type(gate) is dict and set(gate) == keys, 'Invalid review gate keys')
        _require(gate['action'] == action and gate['personallyViewedScene'] == scene,
                 'Wrong action or personally viewed scene')
        if normalized_tap is not None:
            tap = gate['normalizedTap']
            _require(type(tap) is list and len(tap) == 2
                     and all(type(v) in [int, float] and math.isfinite(v) and 0 <= v <= 1 for v in tap)
                     and tap == list(normalized_tap), 'Wrong fixed tap coordinates')
        _context(expected_context)
        _require(_integer(now_ns, 1) and _integer(input_deadline_ns, 1)
                 and now_ns < input_deadline_ns, 'Original input deadline expired')
        name = gate['captureManifest']
        match = re.fullmatch(r'capture-(\d{2})\.json', name) if isinstance(name, str) else None
        _require(match is not None and _sha(gate['captureManifestSha256']), 'Unsafe capture reference')
        root = pathlib.Path(directory)
        directory_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            raw = _read_regular(directory_fd, name, 16384)
            _require(hashlib.sha256(raw).hexdigest() == gate['captureManifestSha256'], 'Changed capture manifest')
            manifest = json.loads(raw)
            _require(type(manifest) is dict and set(manifest) == MANIFEST_KEYS, 'Invalid manifest keys')
            _require(type(manifest['schemaVersion']) is int and manifest['schemaVersion'] == 1,
                     'Unsupported capture schema')
            _require(_integer(manifest['index']) and manifest['index'] == int(match[1]), 'Wrong capture index')
            _context(manifest['context'])
            _require(manifest['context'] == expected_context, 'Changed worker, run, source, geometry or input epoch')
            captured = manifest['capturedMachNs']
            _require(_integer(captured, 1) and captured >= expected_context['guestStartMachNs']
                     and 0 <= now_ns - captured <= 60_000_000_000, 'Stale, future or pre-start capture')
            for kind in ['native', 'surface']:
                _require(manifest[kind] == '%s-%02d.png' % (kind, manifest['index']), 'Unsafe image path')
                _require(_integer(manifest[kind + 'Bytes'], 1) and _sha(manifest[kind + 'Sha256']),
                         'Invalid capture image metadata')
                image = _read_regular(directory_fd, manifest[kind], 32 * 1024 ** 2)
                _require(len(image) == manifest[kind + 'Bytes'] and image.startswith(b'\x89PNG\r\n\x1a\n')
                         and hashlib.sha256(image).hexdigest() == manifest[kind + 'Sha256'],
                         'Changed or invalid capture image')
            return manifest
        finally:
            os.close(directory_fd)
    except (OSError, TypeError, KeyError, json.JSONDecodeError, UnicodeDecodeError) as error:
        raise CaptureReviewError('Unreadable or malformed capture review') from error
