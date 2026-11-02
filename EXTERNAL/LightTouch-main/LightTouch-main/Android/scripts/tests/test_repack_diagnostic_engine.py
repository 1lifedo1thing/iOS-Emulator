"""Engine-only packaging must preserve every unrelated APK payload exactly."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile

SCRIPT = Path(__file__).resolve().parents[1] / 'repack-diagnostic-engine.py'
SPEC = importlib.util.spec_from_file_location('repack_engine', SCRIPT)
REPACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPACK)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class RepackTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.engine = self.root / 'engine'
        abi = self.engine / 'arm64-v8a'
        abi.mkdir(parents=True)
        (abi / 'libqemu-arm.so').write_bytes(b'new-engine-fixture')
        (abi / 'libc++_shared.so').write_bytes(b'same-cpp')
        (abi / 'portable-manifest.json').write_text(json.dumps({
            'source_revision': 'new-revision', 'source_dirty': False,
            'library_sha256': digest(b'new-engine-fixture')}))
        self.notices = self.root / 'notices'
        self.notices.mkdir()
        (self.notices / 'NOTICE.engine.txt').write_bytes(b'new engine notice')
        (self.notices / 'manifest.json').write_text(json.dumps({
            'schema': 1, 'library_sha256': digest(b'new-engine-fixture'),
            'components': [{'name': 'Android NDK runtime/toolchain notices',
                            'runtime_files': {'libc++_shared.so': digest(b'same-cpp')}}],
            'files': [{'path': 'NOTICE.engine.txt', 'sha256': digest(b'new engine notice')}]}))
        self.base = self.root / 'base.apk'
        self.entries = {'classes.dex': b'managed-code', 'AndroidManifest.xml': b'manifest',
                        'lib/arm64-v8a/libqemu-arm.so': b'old-engine',
                        'lib/arm64-v8a/libc++_shared.so': b'same-cpp',
                        'lib/arm64-v8a/liblighttouch_jni.so': b'unchanged-jni',
                        'lib/x86_64/libqemu-arm.so': b'unchanged-x86-engine',
                        'META-INF/DEBUG.RSA': b'old-signature',
                        'META-INF/MANIFEST.MF': b'old-signature-manifest',
                        'META-INF/real.kotlin_module': b'keep-kotlin-metadata',
                        'META-INF/keep.MF': b'keep-unrelated-manifest',
                        'assets/licenses/engine-arm64-v8a/obsolete.txt': b'old-notices',
                        'assets/licenses/engine-x86_64/manifest.json': b'keep-x86-notices'}
        self.write_base()
        self.output = self.root / 'unsigned.apk'

    def write_base(self):
        with zipfile.ZipFile(self.base, 'w') as archive:
            for name, payload in self.entries.items():
                archive.writestr(name, payload)

    def run_repack(self, **kwargs):
        args = dict(base=self.base, expected_base_sha=REPACK.sha(self.base),
                    engine_root=self.engine, notices=self.notices,
                    expected_engine_revision='new-revision', output=self.output)
        args.update(kwargs)
        return REPACK.repack(**args)

    def test_exact_unrelated_payload_preservation_and_notice_replacement(self):
        original = self.base.read_bytes()
        result = self.run_repack()
        self.assertEqual(self.base.read_bytes(), original)
        self.assertFalse(result['nativeManagedRecompiled'])
        self.assertTrue(result['zipalignRequired'] and result['signingRequired'])
        self.assertFalse(result['targetExecutionVerified'])
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.read('lib/arm64-v8a/libqemu-arm.so'), b'new-engine-fixture')
            self.assertEqual(archive.read('assets/licenses/engine-arm64-v8a/NOTICE.engine.txt'), b'new engine notice')
            for name, payload in self.entries.items():
                if name.startswith('assets/licenses/engine-arm64-v8a/') or REPACK.signature(name) or name == 'lib/arm64-v8a/libqemu-arm.so':
                    continue
                self.assertEqual(archive.read(name), payload)
            self.assertNotIn('META-INF/DEBUG.RSA', archive.namelist())
            self.assertNotIn('assets/licenses/engine-arm64-v8a/obsolete.txt', archive.namelist())

    def test_changed_original_hash_refuses_before_output(self):
        with self.assertRaisesRegex(ValueError, 'Original APK hash'):
            self.run_repack(expected_base_sha='0' * 64)
        self.assertFalse(self.output.exists())

    def test_wrong_source_or_dirty_engine_refused(self):
        for change in ({'source_dirty': True}, {'source_revision': 'other'}):
            path = self.engine / 'arm64-v8a/portable-manifest.json'
            row = json.loads(path.read_text())
            row.update(change)
            path.write_text(json.dumps(row))
            with self.assertRaisesRegex(ValueError, 'source identity'):
                self.run_repack()
        self.assertFalse(self.output.exists())

    def test_library_manifest_mismatch_refused(self):
        (self.engine / 'arm64-v8a/libqemu-arm.so').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'Engine manifest'):
            self.run_repack()
        self.assertFalse(self.output.exists())

    def test_cpp_runtime_cannot_silently_change(self):
        self.entries['lib/arm64-v8a/libc++_shared.so'] = b'old-different-cpp'
        self.write_base()
        with self.assertRaisesRegex(ValueError, 'C\\+\\+ runtime changed'):
            self.run_repack()
        self.assertFalse(self.output.exists())

    def test_exact_stripping_proof_preserves_original_cpp_payload(self):
        self.entries['lib/arm64-v8a/libc++_shared.so'] = b'packaged-stripped-cpp'
        self.write_base()
        backing = self.root / 'llvm-objcopy'; backing.write_bytes(b'pinned tool fixture')
        tool = self.root / 'llvm-strip'; tool.symlink_to(backing.name)
        def strip(argv, **kwargs):
            self.assertEqual(argv[0], str(tool))
            self.assertEqual(argv[1:3], ['--strip-unneeded', '-o'])
            Path(argv[3]).write_bytes(b'packaged-stripped-cpp')
        with patch.object(REPACK.subprocess, 'run', side_effect=strip):
            result = self.run_repack(strip_tool=tool)
        self.assertEqual(result['cppRuntimeProof']['kind'], 'EXACT_STRIP_UNNEEDED_REPRODUCTION')
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.read('lib/arm64-v8a/libc++_shared.so'), b'packaged-stripped-cpp')
        self.assertNotIn('lib/arm64-v8a/libc++_shared.so', result['replacementPayloads'])

    def test_different_runtime_after_stripping_still_refused(self):
        self.entries['lib/arm64-v8a/libc++_shared.so'] = b'other-runtime'
        self.write_base()
        tool = self.root / 'strip-tool'; tool.write_bytes(b'pinned tool fixture')
        def strip(argv, **kwargs): Path(argv[3]).write_bytes(b'wrong-stripped-runtime')
        with patch.object(REPACK.subprocess, 'run', side_effect=strip):
            with self.assertRaisesRegex(ValueError, 'after exact stripping'):
                self.run_repack(strip_tool=tool)
        self.assertFalse(self.output.exists())

    def test_notice_pin_failure_refused_before_output(self):
        (self.notices / 'NOTICE.engine.txt').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'Invalid or changed'):
            self.run_repack()
        self.assertFalse(self.output.exists())

    def test_duplicate_original_entry_refused(self):
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with zipfile.ZipFile(self.base, 'a') as archive:
                archive.writestr('classes.dex', b'ambiguous')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            self.run_repack()
        self.assertFalse(self.output.exists())

    def test_original_missing_engine_notices_refused(self):
        self.entries.pop('assets/licenses/engine-arm64-v8a/obsolete.txt')
        self.write_base()
        with self.assertRaisesRegex(ValueError, 'engine notices missing'):
            self.run_repack()
        self.assertFalse(self.output.exists())

    def test_existing_output_preserved(self):
        self.output.write_bytes(b'failed-attempt-preserved')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.run_repack()
        self.assertEqual(self.output.read_bytes(), b'failed-attempt-preserved')


if __name__ == '__main__':
    unittest.main()
