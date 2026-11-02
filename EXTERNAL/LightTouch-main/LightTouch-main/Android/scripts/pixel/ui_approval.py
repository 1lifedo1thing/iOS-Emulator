# SPDX-License-Identifier: GPL-2.0-or-later
"""Local inspected-image approval: exact capture identity and original deadline."""
import hashlib
import json
from pathlib import Path
import re

ALLOWED = ('postinstall-ui.png', 'post-wait-ui.png', 'post-wait-current.png')


class UIApprovalUsageError(ValueError):
    """Pre-action local argument error; does not authorize changing target state."""


def normalized_capture(attempt, image):
    root = Path(attempt).resolve()
    if not isinstance(image, (str, Path)) or not str(image):
        raise UIApprovalUsageError('Actual capture filename/path required')
    path = Path(image)
    if not path.is_absolute():
        if len(path.parts) != 1:
            raise UIApprovalUsageError('One actual capture basename required')
        path = root / path
    if path.parent.resolve() != root or path.name not in ALLOWED or path.is_symlink():
        raise UIApprovalUsageError('Only the declared actual capture in this attempt is accepted')
    if not path.is_file():
        raise UIApprovalUsageError('Actual local capture does not exist')
    return path, path.name


def approve(controller, image, pin, png_dimensions):
    # Ownership/terminal and deadline failures are irreversible attempt failures.
    if (controller.out/'request-stop').exists() or (controller.out/'terminal.json').exists():
        raise RuntimeError('Attempt already stopping; no UI gate may be committed')
    if controller.elapsed() >= 240:
        raise RuntimeError('Original UI deadline cannot be extended')
    path, name = normalized_capture(controller.out, image)
    if not isinstance(pin, str) or not re.fullmatch('[0-9a-f]{64}', pin):
        raise UIApprovalUsageError('Exact inspected capture SHA256 required')
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != pin:
        raise UIApprovalUsageError('Inspected capture SHA mismatch')
    dimensions = png_dimensions(data)  # A corrupt actual capture is a target/evidence failure.
    try:
        receipt = json.loads((controller.out/(path.stem+'-receipt.json')).read_text())
    except (OSError, ValueError) as error:
        raise RuntimeError('Actual capture provenance unavailable') from error
    capture_elapsed = receipt.get('elapsedSeconds')
    if type(capture_elapsed) not in (int, float) or not 0 <= capture_elapsed < 240 or capture_elapsed > controller.elapsed():
        raise RuntimeError('Capture original-clock provenance invalid')
    if receipt.get('sha256') != pin or receipt.get('dimensions') != list(dimensions):
        raise RuntimeError('Actual capture receipt identity differs')
    if controller.elapsed() >= 240:
        raise RuntimeError('Approval work exceeded original UI deadline')
    controller.write('ui-gate.json', {'uiGatePassed': True, 'visuallyInspected': True,
        'unobstructedLightTouchUi': True, 'image': name, 'imageSha256': pin,
        'dimensions': dimensions, 'captureElapsedSeconds': capture_elapsed,
        'elapsedSeconds': controller.elapsed(), 'guestAcceptancePassed': False})


def local_usage_rejection(stage, error):
    return stage == 'approve-ui' and isinstance(error, UIApprovalUsageError)


def handle_controller_error(controller, stage, error):
    local = local_usage_rejection(stage, error)
    controller.write(('approval-usage-rejected-' if local else 'controller-failure-')+stage+'.json',
        {'error': str(error), 'elapsedSeconds': controller.elapsed(), 'guestAcceptancePassed': False,
         'preactionLocalUsageError': local, 'targetMutation': False if local else None})
    if not local:
        (controller.out/'request-stop').touch()
    return local
