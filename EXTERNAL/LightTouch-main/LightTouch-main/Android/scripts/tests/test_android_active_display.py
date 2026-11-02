import importlib.util
import pathlib
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / 'android_active_display.py'
spec = importlib.util.spec_from_file_location('android_active_display', SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ActiveDisplayTests(unittest.TestCase):
    def setUp(self):
        self.line = ('Viewport INTERNAL: displayId=0, uniqueId=local:synthetic, port=0, '
                     'orientation=0, densityDpi=400 xDpi=400.000000, yDpi=400.000000, '
                     'logicalFrame=[0, 0, 1000, 2000], physicalFrame=[0, 0, 1000, 2000], '
                     'deviceSize=[1000, 2000], isActive=[1]')
        self.expected = dict(width=1000, height=2000, density_dpi=400)

    def matches(self, raw):
        return module.active_display_matches(raw, **self.expected)

    def test_matching_duplicate_reader_sections_are_accepted(self):
        self.assertTrue(self.matches(('  ' + self.line + '\n    ' + self.line).encode()))

    def test_auto_rotation_preference_is_not_an_orientation_report(self):
        self.assertFalse(self.matches('accelerometer_rotation=0\nuser_rotation=0'))
        self.assertTrue(self.matches('accelerometer_rotation=1\n' + self.line))

    def test_conflicting_active_viewports_are_rejected(self):
        self.assertFalse(self.matches(self.line + '\n' + self.line.replace('orientation=0,', 'orientation=1,')))

    def test_active_target_display_and_every_geometry_field_are_required(self):
        replacements = [('displayId=0,', 'displayId=1,'), ('isActive=[1]', 'isActive=[0]'),
                        ('orientation=0,', 'orientation=1,'), ('densityDpi=400 ', 'densityDpi=401 '),
                        ('logicalFrame=[0, 0, 1000, 2000]', 'logicalFrame=[0, 0, 2000, 1000]'),
                        ('physicalFrame=[0, 0, 1000, 2000]', 'physicalFrame=[0, 0, 999, 2000]'),
                        ('deviceSize=[1000, 2000]', 'deviceSize=[1000, 1999]')]
        for before, after in replacements:
            with self.subTest(field=before):
                self.assertFalse(self.matches(self.line.replace(before, after)))

    def test_missing_or_nontext_input_fails_closed(self):
        for raw in ['', b'\xff', None, {}, 12]:
            with self.subTest(raw=raw):
                self.assertFalse(self.matches(raw))

    def test_invalid_expected_configuration_is_rejected(self):
        for key, value in [('width', True), ('height', 0), ('density_dpi', -1),
                           ('orientation', 4), ('display_id', -1)]:
            config = dict(self.expected, **{key: value})
            self.assertFalse(module.active_display_matches(self.line, **config))

    def test_configurable_display_and_landscape_orientation(self):
        line = self.line.replace('displayId=0,', 'displayId=2,').replace('orientation=0,', 'orientation=1,')
        self.assertTrue(module.active_display_matches(line, display_id=2, orientation=1, **self.expected))


if __name__ == '__main__':
    unittest.main()
