"""Check exact artifact schema bindings and unrelated APK payload preservation."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('verify_output', ROOT / 'verify-diagnostic-output.py')
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)
REPACK_TEST = importlib.util.spec_from_file_location('repack_tests', Path(__file__).with_name('test_repack_diagnostic_engine.py'))
FIXTURE = importlib.util.module_from_spec(REPACK_TEST)
REPACK_TEST.loader.exec_module(FIXTURE)


class PayloadTests(FIXTURE.RepackTests):
    # Inherit the nine packaging contracts, then exercise final signed-payload
    # verification independently of the receipt generator.
    def test_verifier_accepts_exact_payload_and_checks_every_hash(self):
        receipt = self.run_repack()
        result = VERIFY.verify_payloads(self.base, self.output, receipt)
        self.assertEqual(result['signedApkSha256'], VERIFY.sha(self.output))
        self.assertEqual(result['preservedPayloads'], len(receipt['unchangedPayloads']))

    def rewrite(self, changes, omitted=()):
        replacement = self.root / 'tampered.apk'
        with zipfile.ZipFile(self.output) as before, zipfile.ZipFile(replacement, 'x') as after:
            for info in before.infolist():
                if info.filename not in omitted:
                    after.writestr(info, changes.get(info.filename, before.read(info)))
        return replacement

    def test_verifier_rejects_unrelated_managed_or_jni_change(self):
        receipt = self.run_repack()
        for name in ['classes.dex', 'lib/arm64-v8a/liblighttouch_jni.so']:
            altered = self.rewrite({name: b'changed'})
            with self.assertRaisesRegex(ValueError, 'Unrelated APK payload changed'):
                VERIFY.verify_payloads(self.base, altered, receipt)
            altered.unlink()

    def test_verifier_rejects_receipt_omitting_unrelated_file(self):
        receipt = self.run_repack()
        receipt['unchangedPayloads'].pop('classes.dex')
        altered = self.rewrite({}, ['classes.dex'])
        with self.assertRaisesRegex(ValueError, 'omitted unrelated'):
            VERIFY.verify_payloads(self.base, altered, receipt)

    def test_verifier_rejects_changed_new_engine(self):
        receipt = self.run_repack()
        altered = self.rewrite({'lib/arm64-v8a/libqemu-arm.so': b'wrong'})
        with self.assertRaisesRegex(ValueError, 'Replacement APK payload differs'):
            VERIFY.verify_payloads(self.base, altered, receipt)

    def test_verifier_rejects_extra_entry(self):
        receipt = self.run_repack()
        altered = self.rewrite({})
        with zipfile.ZipFile(altered, 'a') as archive:
            archive.writestr('unexpected', b'x')
        with self.assertRaisesRegex(ValueError, 'closure differs'):
            VERIFY.verify_payloads(self.base, altered, receipt)

    def test_verifier_rejects_receipt_broadening_replace_scope(self):
        receipt = self.run_repack()
        receipt['replacementPayloads']['classes.dex'] = receipt['unchangedPayloads'].pop('classes.dex')
        with self.assertRaisesRegex(ValueError, 'Unexpected replacement'):
            VERIFY.verify_payloads(self.base, self.output, receipt)


class NativeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.run = self.root / 'run'
        self.run.mkdir()
        self.frozen = self.run / 'frozen'
        self.frozen.mkdir()
        self.notices = self.root / 'notices'
        self.notices.mkdir()
        self.bundle = self.root / 'sourcekit'
        self.bundle.mkdir()
        self.source = self.root / 'source'
        self.source.mkdir()
        self.rev = '1' * 40
        self.engine = self.frozen / 'libqemu-arm.so'
        raw = bytearray(64)
        raw[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HH', raw, 16, 3, 183)
        self.engine.write_bytes(raw)
        self.cpp = self.root / 'cpp.so'
        self.cpp.write_bytes(b'cpp')
        (self.frozen / 'libc++_shared.so').write_bytes(b'cpp')
        sources = {}
        for name in ['include/hw/arm/ipod_touch_lcd.h', 'hw/arm/ipod_touch_lcd.c', 'contrib/ios-app/qemu-ios-ui.c']:
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
            sources[name] = VERIFY.sha(path)
        self.manifest = {'schema': 2, 'api_major': 2, 'portable_version': 1, 'source_revision': self.rev,
                         'source_status': '', 'source_dirty': False, 'library_sha256': VERIFY.sha(self.engine),
                         'runtime_files': {'libc++_shared.so': VERIFY.sha(self.cpp)}, 'source_inputs': sources}
        self.write(self.frozen / 'portable-manifest.json', self.manifest)
        (self.notices / 'NOTICE.engine.txt').write_bytes(b'notice')
        self.notice = {'schema': 1, 'source_revision': self.rev, 'library_sha256': VERIFY.sha(self.engine),
                       'components': [{'name': 'Android NDK runtime/toolchain notices', 'runtime_files': self.manifest['runtime_files']}],
                       'files': [{'path': 'NOTICE.engine.txt', 'sha256': VERIFY.sha(self.notices / 'NOTICE.engine.txt')}]}
        self.write(self.notices / 'manifest.json', self.notice)
        (self.bundle / 'source.c').write_bytes(b'public source')
        self.bundle_data = {'schema': 1, 'abi': 'arm64-v8a', 'source_revision': self.rev, 'generator_revision': self.rev,
                            'engine_sha256': VERIFY.sha(self.engine), 'files': {'source.c': VERIFY.sha(self.bundle / 'source.c')},
                            'source_modes': {'source.c': (self.bundle / 'source.c').stat().st_mode & 0o777}}
        self.write(self.bundle / 'bundle.json', self.bundle_data)
        self.plan = {'android_api': 29, 'source_root': str(self.source), 'source_revision': self.rev,
                     'run_output': str(self.run), 'files': [{'path': str(self.source / n), 'sha256': h} for n,h in sources.items()],
                     'targets': [{'abi': 'arm64-v8a', 'cpp_input': str(self.cpp), 'notices': str(self.notices),
                                  'sourcekit': str(self.bundle), 'readelf': 'readelf',
                                  'expected_exports': ['qemu_ios_main'], 'expected_needed': ['libc.so']}]}
        self.plan_path = self.root / 'plan.json'
        self.write(self.plan_path, self.plan)
        self.row = {'abi': 'arm64-v8a', 'frozen': str(self.frozen), 'native_source_revision': self.rev,
                    'packaging_tool_revision': self.rev, 'library_sha256': VERIFY.sha(self.engine),
                    'manifest_sha256': VERIFY.sha(self.frozen / 'portable-manifest.json'),
                    'notice_manifest_sha256': VERIFY.sha(self.notices / 'manifest.json'),
                    'sourcekit_manifest_sha256': VERIFY.sha(self.bundle / 'bundle.json')}
        self.write(self.run / 'artifacts.json', [self.row])
        self.terminal = {'approved_plan_sha256': VERIFY.sha(self.plan_path), 'completed_abis': ['arm64-v8a'],
                         'commands': [{'exit': 0, 'cleanup_session_retired': True}]}
        self.write(self.run / 'terminal.json', self.terminal)
        for path in self.frozen.iterdir():
            path.chmod(0o444)

    @staticmethod
    def write(path, data):
        path.write_text(json.dumps(data))

    @staticmethod
    def readelf(argv, **kwargs):
        if '-dW' in argv:
            return '  (NEEDED) Shared library: [libc.so]\n  (SONAME) Library soname: [libqemu-arm.so]\n'
        if '-lW' in argv:
            return '  LOAD 0 0 0 0 0 R 0x4000\n'
        return ' 1: 00000000 0 FUNC GLOBAL DEFAULT 1 qemu_ios_main\n'

    def verify(self):
        with patch.object(VERIFY.subprocess, 'check_output', side_effect=self.readelf):
            return VERIFY.verify_engine(self.plan_path, VERIFY.sha(self.plan_path))

    def test_actual_output_schema_and_full_member_inventory(self):
        result = self.verify()
        self.assertTrue(result['pass'])
        self.assertFalse(result['freshRelinkPerformed'] or result['targetExecutionVerified'])
        self.assertEqual(result['elf']['loadAlignment'], [16384])

    def test_wrong_elf_machine_rejected_before_readelf(self):
        raw = bytearray(self.engine.read_bytes())
        struct.pack_into('<H', raw, 18, 62)
        self.engine.chmod(0o644)
        self.engine.write_bytes(raw)
        with self.assertRaisesRegex(ValueError, 'Expected AArch64'):
            VERIFY.readelf_check(self.engine, 'readelf', [], [])

    def test_wrong_export_or_needed_closure_rejected(self):
        for exports, needed in [([], ['libc.so']), (['qemu_ios_main'], ['libother.so'])]:
            with patch.object(VERIFY.subprocess, 'check_output', side_effect=self.readelf):
                with self.assertRaisesRegex(ValueError, 'exports differ|needed closure differs'):
                    VERIFY.readelf_check(self.engine, 'readelf', exports, needed)

    def test_unexpected_notice_member_rejected(self):
        (self.notices / 'extra').write_bytes(b'extra')
        with self.assertRaisesRegex(ValueError, 'Notice closure'):
            self.verify()

    def test_changed_sourcekit_member_rejected(self):
        (self.bundle / 'source.c').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'member hash differs'):
            self.verify()

    def test_failed_or_incomplete_terminal_refused(self):
        self.terminal['commands'][0]['exit'] = 1
        self.write(self.run / 'terminal.json', self.terminal)
        with self.assertRaisesRegex(ValueError, 'Native command failed'):
            self.verify()

    def test_manifest_api_mismatch_refused(self):
        path = self.frozen / 'portable-manifest.json'
        path.chmod(0o644)
        self.manifest['portable_version'] = 2
        self.write(path, self.manifest)
        path.chmod(0o444)
        with self.assertRaisesRegex(ValueError, 'API manifest differs'):
            self.verify()


if __name__ == '__main__':
    unittest.main()
