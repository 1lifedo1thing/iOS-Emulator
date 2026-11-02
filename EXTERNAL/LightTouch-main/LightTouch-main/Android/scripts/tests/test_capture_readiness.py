import importlib.util
import pathlib
import unittest
import xml.etree.ElementTree as ET

path = pathlib.Path(__file__).resolve().parents[1] / 'android_capture_readiness.py'
spec = importlib.util.spec_from_file_location('capture_readiness_policy', path)
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


def ui(state='starting'):
    root = ET.Element('hierarchy')
    package = 'org.lighttouch.android'
    ET.SubElement(root, 'node', {'resource-id': 'session_status', 'package': package,
                               'enabled': 'true', 'text': state})
    for rid, text, enabled in [('stop_guest', 'Stop', True),
                              ('pause_guest', 'Pause', state == 'running'),
                              ('screenshot_button', 'Screenshot', state == 'running')]:
        parent = ET.SubElement(root, 'node', {'resource-id': rid, 'package': package,
            'enabled': str(enabled).lower(), 'clickable': 'true', 'bounds': '[10,20][110,120]'})
        # Compose children remain enabled even when their containing button is disabled.
        ET.SubElement(parent, 'node', {'text': text, 'package': package, 'enabled': 'true'})
    ET.SubElement(root, 'node', {'content-desc': 'Guest display', 'package': package, 'enabled': 'true'})
    return root


def classify(root):
    return policy.capture_readiness(ET.tostring(root))


class CaptureReadinessTests(unittest.TestCase):
    def test_starting_with_disabled_capture_waits_despite_enabled_child_text(self):
        self.assertEqual(classify(ui()), 'starting')

    def test_running_with_ready_controls_can_capture(self):
        self.assertEqual(classify(ui('running')), 'ready')

    def test_other_session_states_never_become_a_boot_wait(self):
        for state in ['stopped', 'failed', 'stopping', 'paused', 'starting ', '']:
            with self.subTest(state=state), self.assertRaises(policy.CaptureReadinessError):
                classify(ui(state))

    def test_session_error_fails_even_during_well_formed_starting(self):
        root = ui()
        ET.SubElement(root, 'node', {'resource-id': 'session_error', 'text': 'Native engine failed'})
        with self.assertRaises(policy.CaptureReadinessError):
            classify(root)

    def test_missing_duplicate_and_foreign_status_rejected(self):
        for change in ['missing', 'duplicate', 'foreign']:
            root = ui()
            status = root[0]
            if change == 'missing':
                root.remove(status)
            elif change == 'duplicate':
                root.append(ET.fromstring(ET.tostring(status)))
            else:
                status.set('package', 'another.application')
            with self.subTest(change=change), self.assertRaises(policy.CaptureReadinessError):
                classify(root)

    def test_every_inconsistent_control_rejected(self):
        for state in ['starting', 'running']:
            for rid in ['stop_guest', 'pause_guest', 'screenshot_button']:
                root = ui(state)
                node = root.find("node[@resource-id='%s']" % rid)
                node.set('enabled', 'false' if node.get('enabled') == 'true' else 'true')
                with self.subTest(state=state, resource_id=rid), self.assertRaises(policy.CaptureReadinessError):
                    classify(root)

    def test_bad_missing_duplicate_or_foreign_button_is_not_readiness(self):
        for change in ['missing', 'duplicate', 'foreign', 'label', 'bounds', 'clickable']:
            root = ui()
            node = root.find("node[@resource-id='screenshot_button']")
            if change == 'missing':
                root.remove(node)
            elif change == 'duplicate':
                root.append(ET.fromstring(ET.tostring(node)))
            elif change == 'foreign':
                node.set('package', 'another.application')
            elif change == 'label':
                node[0].set('text', 'Retry')
            elif change == 'bounds':
                node.set('bounds', '[10,20][10,120]')
            else:
                node.set('clickable', 'false')
            with self.subTest(change=change), self.assertRaises(policy.CaptureReadinessError):
                classify(root)

    def test_missing_duplicate_disabled_or_foreign_display_rejected(self):
        for change in ['missing', 'duplicate', 'disabled', 'foreign']:
            root = ui()
            node = root.find("node[@content-desc='Guest display']")
            if change == 'missing':
                root.remove(node)
            elif change == 'duplicate':
                root.append(ET.fromstring(ET.tostring(node)))
            else:
                node.set('enabled' if change == 'disabled' else 'package', 'false' if change == 'disabled' else 'another.application')
            with self.subTest(change=change), self.assertRaises(policy.CaptureReadinessError):
                classify(root)

    def test_malformed_or_unrelated_xml_never_waits(self):
        for xml in [b'<hierarchy>', b'<window/>', b'<hierarchy/>']:
            with self.subTest(xml=xml), self.assertRaises(policy.CaptureReadinessError):
                policy.capture_readiness(xml)


if __name__ == '__main__':
    unittest.main()
