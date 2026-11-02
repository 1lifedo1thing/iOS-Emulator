import copy
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from android_capture_review import CaptureReviewError, validate_reviewed_capture


class CaptureReviewTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = pathlib.Path(self.temporary.name)
        self.context = dict(recordId='fixture-record', workerPid=123, workerStartTicks=456,
                            bootId='test-boot', controllerSha256='a' * 64,
                            runOriginMachNs=1_000_000_000, guestStartMachNs=2_000_000_000,
                            guestInputEpoch=0, displayBounds='[10,20][650,1180]')
        self.manifest = dict(schemaVersion=1, index=12, context=copy.deepcopy(self.context),
                             capturedMachNs=10_000_000_000)
        for kind in ['native', 'surface']:
            raw = b'\x89PNG\r\n\x1a\nsynthetic-policy-fixture-' + kind.encode()
            name = '%s-12.png' % kind
            (self.directory / name).write_bytes(raw)
            self.manifest.update({kind: name, kind + 'Sha256': hashlib.sha256(raw).hexdigest(),
                                  kind + 'Bytes': len(raw)})
        self.gate = dict(action='wake', personallyViewedScene='lock-or-asleep',
                         captureManifest='capture-12.json')
        self.publish()

    def publish(self):
        raw = (json.dumps(self.manifest, sort_keys=True) + '\n').encode()
        (self.directory / 'capture-12.json').write_bytes(raw)
        self.gate['captureManifestSha256'] = hashlib.sha256(raw).hexdigest()

    def validate(self, now=20_000_000_000, deadline=260_000_000_000, **kwargs):
        return validate_reviewed_capture(self.directory, self.gate, self.context, now,
                                         deadline, 'wake', 'lock-or-asleep', **kwargs)

    def test_reviewed_capture_survives_advanced_or_absent_latest_pointer(self):
        self.assertEqual(self.validate(), self.manifest)
        (self.directory / 'latest-pair.json').write_text('{"index":13,"native":"newer.png"}')
        self.assertEqual(self.validate(), self.manifest)
        (self.directory / 'latest-pair.json').write_text('malformed unrelated mutable pointer')
        self.assertEqual(self.validate(), self.manifest)

    def test_expiry_future_prestart_and_original_deadline_rejected(self):
        self.validate(now=70_000_000_000)
        for now, deadline in [(70_000_000_001, 260_000_000_000), (9_999_999_999, 260_000_000_000),
                              (20_000_000_000, 20_000_000_000), (True, 260_000_000_000)]:
            with self.subTest(now=now, deadline=deadline):
                with self.assertRaises(CaptureReviewError): self.validate(now, deadline)
        self.manifest['capturedMachNs'] = self.context['guestStartMachNs'] - 1
        self.publish()
        with self.assertRaises(CaptureReviewError): self.validate()

    def test_wrong_manifest_hash_missing_or_rewritten_file_rejected(self):
        self.gate['captureManifestSha256'] = '0' * 64
        with self.assertRaises(CaptureReviewError): self.validate()
        self.publish()
        p = self.directory / 'capture-12.json'
        p.write_bytes(p.read_bytes() + b' ')
        with self.assertRaises(CaptureReviewError): self.validate()
        p.unlink()
        with self.assertRaises(CaptureReviewError): self.validate()

    def test_each_image_changed_missing_bad_hash_or_size_rejected(self):
        for kind in ['native', 'surface']:
            original = copy.deepcopy(self.manifest)
            self.manifest[kind + 'Sha256'] = 'b' * 64
            self.publish()
            with self.assertRaises(CaptureReviewError): self.validate()
            self.manifest = copy.deepcopy(original)
            self.manifest[kind + 'Bytes'] += 1
            self.publish()
            with self.assertRaises(CaptureReviewError): self.validate()
            self.manifest = original
            self.publish()
            p = self.directory / self.manifest[kind]
            raw = p.read_bytes()
            p.write_bytes(raw[:-1] + b'X')
            with self.assertRaises(CaptureReviewError): self.validate()
            p.unlink()
            with self.assertRaises(CaptureReviewError): self.validate()
            p.write_bytes(raw)

    def test_all_context_fields_reject_other_worker_boot_run_source_epoch_and_geometry(self):
        for key, value in self.context.items():
            original = self.manifest['context'][key]
            self.manifest['context'][key] = value + 1 if type(value) is int else value + 'X'
            self.publish()
            with self.subTest(key=key):
                with self.assertRaises(CaptureReviewError): self.validate()
            self.manifest['context'][key] = original
        for key in ['workerPid', 'workerStartTicks', 'guestInputEpoch', 'runOriginMachNs', 'guestStartMachNs']:
            self.manifest['context'][key] = True
            self.publish()
            with self.assertRaises(CaptureReviewError): self.validate()
            self.manifest['context'][key] = self.context[key]

    def test_gate_cannot_switch_action_scene_or_add_unreviewed_input(self):
        for key, value in [('action', 'unlock'), ('personallyViewedScene', 'home'),
                           ('normalizedTap', [0.1, 0.2]), ('unknown', True)]:
            saved = copy.deepcopy(self.gate)
            self.gate[key] = value
            with self.assertRaises(CaptureReviewError): self.validate()
            self.gate = saved

    def test_fixed_tap_must_match_existing_contract_exactly(self):
        self.gate['normalizedTap'] = [0.1, 0.2]
        self.validate(normalized_tap=(0.1, 0.2))
        for tap in [[0.2, 0.1], [True, 0.2], [float('nan'), 0.2], [0.1], '0.1,0.2']:
            self.gate['normalizedTap'] = tap
            with self.assertRaises(CaptureReviewError): self.validate(normalized_tap=(0.1, 0.2))

    def test_unsafe_manifest_and_image_paths_never_escape_capture_directory(self):
        original = copy.deepcopy(self.gate)
        for name in ['../capture-12.json', '/tmp/capture-12.json', 'sub/capture-12.json',
                     'capture-12.json/..', 'capture-12.json\x00', 'capture-1.json']:
            self.gate['captureManifest'] = name
            with self.assertRaises(CaptureReviewError): self.validate()
        self.gate = original
        for kind in ['native', 'surface']:
            for name in ['../outside.png', '/tmp/outside.png', 'sub/image.png', kind + '-13.png']:
                self.manifest[kind] = name
                self.publish()
                with self.assertRaises(CaptureReviewError): self.validate()
            self.manifest[kind] = kind + '-12.png'

    def test_manifest_images_and_root_symlinks_rejected(self):
        for name in ['capture-12.json', 'native-12.png', 'surface-12.png']:
            p = self.directory / name
            target = self.directory / ('original-' + name)
            p.rename(target)
            p.symlink_to(target)
            with self.assertRaises(CaptureReviewError): self.validate()
            p.unlink()
            target.rename(p)
        link = self.directory / 'root-link'
        link.symlink_to(self.directory, target_is_directory=True)
        with self.assertRaises(CaptureReviewError):
            validate_reviewed_capture(link, self.gate, self.context, 20_000_000_000,
                                      260_000_000_000, 'wake', 'lock-or-asleep')

    def test_schema_index_and_nonregular_files_rejected(self):
        for key, value in [('schemaVersion', True), ('schemaVersion', 2), ('index', True),
                           ('index', 13), ('capturedMachNs', True), ('extra', 'unknown')]:
            saved = copy.deepcopy(self.manifest)
            self.manifest[key] = value
            self.publish()
            with self.assertRaises(CaptureReviewError): self.validate()
            self.manifest = saved
        self.publish()
        p = self.directory / 'native-12.png'
        p.unlink()
        p.mkdir()
        with self.assertRaises(CaptureReviewError): self.validate()

    def test_malformed_or_oversized_manifest_rejected(self):
        p = self.directory / 'capture-12.json'
        for raw in [b'{', b'\xff', b'[]', b'{}' + b' ' * 16384]:
            p.write_bytes(raw)
            self.gate['captureManifestSha256'] = hashlib.sha256(raw).hexdigest()
            with self.assertRaises(CaptureReviewError): self.validate()


if __name__ == '__main__':
    unittest.main(verbosity=2)
