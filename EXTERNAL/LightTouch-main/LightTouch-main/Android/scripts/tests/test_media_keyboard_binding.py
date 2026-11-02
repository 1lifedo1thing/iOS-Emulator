import importlib.util
import hashlib
from pathlib import Path
import tempfile
import unittest
from PIL import Image

spec = importlib.util.spec_from_file_location('binding', Path(__file__).resolve().parents[1]/'prepare-media-keyboard-binding.py')
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


class BindingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.image = Path(self.temp.name)/'guest.png'
        Image.new('RGBA', (80, 100), (17, 34, 51, 255)).save(self.image)
        self.args = dict(before={'spec': {'workerPid': 71, 'workerUid': 10179}, 'workerStartTicks': 123},
                         image_path=self.image, scratch='private-media-scratch', view=[400, 600], rotation=0,
                         letter=[.3, .7], backspace=[.9, .8], anchors=[[1, 50, 10, 60], [40, 70, 50, 80]],
                         scratch_approved=True, preserves_existing=True)

    def test_real_rgba_hash_matches_android_channel_order(self):
        result = b.prepare(**self.args)
        self.assertEqual(result['anchors'][0]['rgbaSha256'], hashlib.sha256(bytes([17, 34, 51, 255])*90).hexdigest())
        self.assertEqual(result['reviewedGuestCaptureSha256'], hashlib.sha256(self.image.read_bytes()).hexdigest())
        self.assertEqual(result['frame'], [80, 100])
        self.assertEqual(result['workerStartTicks'], 123)

    def test_changed_keyboard_anchor_is_detectable_but_caret_outside_anchor_is_ignored(self):
        first = b.prepare(**self.args)
        with Image.open(self.image) as image:
            image.putpixel((20, 20), (0, 0, 0, 0)); image.save(self.image)
        outside = b.prepare(**self.args)
        self.assertEqual(first['anchors'], outside['anchors'])
        self.assertNotEqual(first['reviewedGuestCaptureSha256'], outside['reviewedGuestCaptureSha256'])
        with Image.open(self.image) as image:
            image.putpixel((1, 50), (0, 0, 0, 255)); image.save(self.image)
        self.assertNotEqual(first['anchors'], b.prepare(**self.args)['anchors'])

    def test_missing_scratch_approval_and_existing_notes_preservation_are_rejected(self):
        for key, value in [('scratch_approved', False), ('preserves_existing', False), ('scratch', '')]:
            with self.subTest(key=key), self.assertRaises(ValueError): b.prepare(**{**self.args, key: value})

    def test_invalid_identity_and_geometry_rejected(self):
        for change in [{'before': {'spec': {'workerPid': 0, 'workerUid': 3}, 'workerStartTicks': 1}},
                       {'view': [400, 0]}, {'rotation': 4}, {'letter': [float('nan'), .7]},
                       {'letter': [1, .7]}, {'backspace': [.3, .7]}, {'anchors': [[0, 0, 90, 90], [1, 1, 2, 2]]}]:
            with self.subTest(change=change), self.assertRaises(ValueError): b.prepare(**{**self.args, **change})

    def test_anchor_count_and_pixel_budget_are_bounded(self):
        for anchors in [[], [[0, 0, 1, 1]], [[0, 0, 80, 100]]*9]:
            with self.subTest(anchors=anchors), self.assertRaises(ValueError): b.prepare(**{**self.args, 'anchors': anchors})
        Image.new('RGBA', (512, 512)).save(self.image)
        with self.assertRaisesRegex(ValueError, 'pixel budget'):
            b.prepare(**{**self.args, 'anchors': [[0, 0, 128, 256], [200, 200, 400, 400]]})


if __name__ == '__main__': unittest.main()
