"""Recognize the narrow Main UI state in which native capture is not ready.

This policy never grants guest input, proves a guest frame, or extends a caller's
deadline. A private device controller may retain a read-only Android screencap
while this returns ``starting``. Unknown, inconsistent, or failed UI is an error.
"""
import re
import xml.etree.ElementTree as ET


class CaptureReadinessError(ValueError):
    pass


def capture_readiness(xml, package='org.lighttouch.android'):
    try:
        root = ET.fromstring(xml)
    except ET.ParseError as error:
        raise CaptureReadinessError('Malformed Android UI XML') from error
    if root.tag != 'hierarchy':
        raise CaptureReadinessError('Android UI hierarchy required')
    nodes = list(root.iter('node'))

    def one(resource_id):
        matches = [n for n in nodes if n.get('resource-id') == resource_id]
        if len(matches) != 1 or matches[0].get('package') != package:
            raise CaptureReadinessError('Missing, duplicate, or foreign ' + resource_id)
        return matches[0]

    if any(n.get('resource-id') == 'session_error' for n in nodes):
        raise CaptureReadinessError('Session error must not be hidden by a boot wait')
    status = one('session_status')
    if status.get('enabled') != 'true' or status.get('text') not in ['starting', 'running']:
        raise CaptureReadinessError('Only starting or running sessions may be observed')
    starting = status.get('text') == 'starting'
    for resource_id, expected_text, enabled in [
        # The home screen's session dock.
        ('stop_guest', 'Stop', True), ('pause_guest', 'Pause', not starting),
        ('screenshot_button', 'Screenshot', not starting),
    ]:
        node = one(resource_id)
        if node.get('enabled') != str(enabled).lower() or node.get('clickable') != 'true':
            raise CaptureReadinessError('Unexpected control state: ' + resource_id)
        labels = [n for n in node.iter('node') if n.get('text') == expected_text]
        if len(labels) != 1 or labels[0].get('package') != package:
            raise CaptureReadinessError('Missing, duplicate, or foreign control label: ' + resource_id)
        match = re.fullmatch(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]', node.get('bounds', ''))
        if not match or int(match[3]) <= int(match[1]) or int(match[4]) <= int(match[2]):
            raise CaptureReadinessError('Invalid control bounds: ' + resource_id)
    displays = [n for n in nodes if n.get('content-desc') == 'Guest display']
    if len(displays) != 1 or displays[0].get('package') != package or displays[0].get('enabled') != 'true':
        raise CaptureReadinessError('Expected one enabled guest display')
    return 'starting' if starting else 'ready'
