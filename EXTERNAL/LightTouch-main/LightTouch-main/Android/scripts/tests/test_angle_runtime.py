"""Host synthetic packaging/consumer tests; no native compile or target claim."""
import hashlib
import io
import json
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

SCRIPTS = Path(__file__).resolve().parents[1]
STAGE = runpy.run_path(str(SCRIPTS / 'stage-angle-runtime.py'))
APK = runpy.run_path(str(SCRIPTS / 'check-apk.py'))
NAMES = STAGE['NAMES']
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()


def elf(machine):
    b = bytearray(120)
    b[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHI', b, 16, 3, machine, 1)
    struct.pack_into('<Q', b, 32, 64)
    struct.pack_into('<HHH', b, 52, 64, 56, 1)
    struct.pack_into('<IIQQQQQQ', b, 64, 1, 5, 0, 16384, 0, 120, 120, 16384)
    return bytes(b)


class AngleRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.dest = self.root / 'generated'
        self.dest.mkdir()
        (self.dest / 'previous').write_text('preserve on rejected input')
        self.dynamic = {}
        self.reader = mock.patch.object(STAGE['subprocess'], 'check_output', side_effect=self.readelf)
        self.reader.start()
        self.addCleanup(self.reader.stop)

    def readelf(self, command, **kwargs):
        path = Path(command[-1])
        return self.dynamic.get(str(path), f'(SONAME) [{path.name}]\n(NEEDED) [libc.so]\n')

    def bundle(self, abi, tag=None):
        root = self.root / (tag or abi)
        lib, notes = root / 'lib', root / 'notices'
        lib.mkdir(parents=True); notes.mkdir()
        objects = {}
        for i, name in enumerate(sorted(NAMES)):
            # Distinct identities per library, with valid bounded ELF64 headers.
            (lib / name).write_bytes(elf(STAGE['ABIS'][abi]) + bytes([i]))
            objects[name] = {'sha256': sha(lib / name), 'soname': name,
                             'needed': ['libc.so'], 'load_alignments': [16384]}
        closure = {'objects': objects}
        if abi != 'x86_64': closure['abi'] = abi
        manifest = root / 'closure.json'
        manifest.write_text(json.dumps(closure))
        (notes / 'LICENSE.txt').write_text('synthetic notice')
        notice = {'schema': 1, 'source_reference': 'https://example.org/angle',
                  'source_revision': 'a' * 40,
                  'libraries': {n: r['sha256'] for n, r in objects.items()},
                  'files': [{'path': 'LICENSE.txt', 'sha256': sha(notes / 'LICENSE.txt')}]}
        (notes / 'manifest.json').write_text(json.dumps(notice))
        return [abi, lib, manifest, notes, sha(manifest), sha(notes / 'manifest.json')]

    def stage(self, bundles):
        return STAGE['stage_bundles'](self.dest, bundles, Path('/mock/readelf'))

    def rejected(self, bundles):
        with self.assertRaises((ValueError, OSError)):
            self.stage(bundles)
        self.assertEqual((self.dest / 'previous').read_text(), 'preserve on rejected input')
        self.assertEqual(list(self.dest.iterdir()), [self.dest / 'previous'])

    def test_dual_abi_independent_notices_and_consumer(self):
        self.stage([self.bundle('x86_64'), self.bundle('arm64-v8a')])
        self.assertFalse((self.dest / 'previous').exists())
        self.assertTrue((self.dest / 'assets/licenses/angle/manifest.json').is_file())
        self.assertTrue((self.dest / 'assets/licenses/angle-arm64-v8a/manifest.json').is_file())
        self.assertEqual(self.consume(), {'x86_64', 'arm64-v8a'})

    def consume(self, change=None):
        entries = {}
        for p in self.dest.rglob('*'):
            if p.is_file():
                name = p.relative_to(self.dest).as_posix().replace('jniLibs/', 'lib/', 1)
                entries[name] = p.read_bytes()
        if change: change(entries)
        f = io.BytesIO()
        with zipfile.ZipFile(f, 'w') as z:
            for n, b in entries.items(): z.writestr(n, b)
        f.seek(0)
        with zipfile.ZipFile(f) as z: return APK['check_angle_runtime'](z)

    def test_legacy_x86_python_call_and_metadata_remain_accepted(self):
        b = self.bundle('x86_64')
        STAGE['stage'](self.dest, *b[1:4], Path('/mock/readelf'), *b[4:])
        def legacy(entries):
            name = 'assets/providers/angle-runtime.json'
            j = json.loads(entries[name])
            for key in ['notice_group', 'closure_manifest_sha256', 'notice_manifest_sha256']: j.pop(key)
            entries[name] = json.dumps(j).encode()
        self.assertEqual(self.consume(legacy), {'x86_64'})

    def test_arm_only_and_validate_only_never_mutates(self):
        bundle = self.bundle('arm64-v8a')
        STAGE['stage_bundles'](self.dest, [bundle], Path('/mock/readelf'), True)
        self.assertTrue((self.dest / 'previous').is_file())
        self.stage([bundle]); self.assertEqual(self.consume(), {'arm64-v8a'})

    def test_bad_second_bundle_cannot_clear_valid_output(self):
        x, arm = self.bundle('x86_64'), self.bundle('arm64-v8a')
        (arm[1] / sorted(NAMES)[0]).write_bytes(elf(62))
        self.rejected([x, arm])

    def test_duplicate_or_unknown_abi(self):
        b = self.bundle('x86_64')
        self.rejected([b, b])
        self.rejected([['armeabi-v7a', *b[1:]]])

    def test_missing_inputs_and_corrupt_notice(self):
        b = self.bundle('arm64-v8a')
        (b[3] / 'LICENSE.txt').unlink(); self.rejected([b])
        (b[3] / 'LICENSE.txt').write_text('changed'); self.rejected([b])
        (b[1] / sorted(NAMES)[0]).unlink(); self.rejected([b])

    def test_malformed_elf_table_and_alignment(self):
        b = self.bundle('arm64-v8a'); path = b[1] / sorted(NAMES)[0]
        for data in [b'\x7fELF', elf(183)[:80], elf(62)]:
            path.write_bytes(data); self.rejected([b])
        data = bytearray(elf(183)); struct.pack_into('<Q', data, 64 + 48, 4096)
        path.write_bytes(data); self.rejected([b])
        data = bytearray(elf(183)); struct.pack_into('<Q', data, 32, 2**63)
        path.write_bytes(data); self.rejected([b])

    def test_wrong_soname_or_dynamic_dependency(self):
        b = self.bundle('arm64-v8a'); path = b[1] / sorted(NAMES)[0]
        for dynamic in ['(SONAME) [libother.so]', f'(SONAME) [{path.name}]\n(NEEDED) [/tmp/dropin.so]']:
            self.dynamic[str(path)] = dynamic; self.rejected([b])

    def test_cross_abi_manifest_and_notice_declaration(self):
        b = self.bundle('arm64-v8a')
        closure = json.loads(b[2].read_text()); closure['abi'] = 'x86_64'
        b[2].write_text(json.dumps(closure)); b[4] = sha(b[2]); self.rejected([b])
        closure['abi'] = 'arm64-v8a'; b[2].write_text(json.dumps(closure)); b[4] = sha(b[2])
        n = b[3] / 'manifest.json'; j = json.loads(n.read_text()); j['abi'] = 'x86_64'
        n.write_text(json.dumps(j)); b[5] = sha(n); self.rejected([b])

    def test_symlink_and_destination_overlap(self):
        b = self.bundle('arm64-v8a'); license_ = b[3] / 'LICENSE.txt'
        target = self.root / 'outside'; target.write_bytes(license_.read_bytes()); license_.unlink(); license_.symlink_to(target)
        self.rejected([b])
        with self.assertRaises(ValueError): STAGE['stage_bundles'](b[1], [b], Path('/mock/readelf'))

    def test_cli_incomplete_inputs_fail_without_generated_mutation(self):
        # No mocked inspector/real ELF needed: command-line validation must fail first.
        r = subprocess.run([sys.executable, str(SCRIPTS / 'stage-angle-runtime.py'), str(self.dest),
                            '--abi', 'arm64-v8a', '--libraries', '/missing'], capture_output=True)
        self.assertNotEqual(r.returncode, 0); self.assertTrue((self.dest / 'previous').is_file())
        r = subprocess.run([sys.executable, str(SCRIPTS / 'stage-angle-runtime.py'), str(self.dest),
                            '--abi', 'arm64-v8a'], capture_output=True)
        self.assertNotEqual(r.returncode, 0); self.assertTrue((self.dest / 'previous').is_file())

    def test_consumer_rejects_partial_cross_abi_extra_metadata_and_notice(self):
        self.stage([self.bundle('x86_64'), self.bundle('arm64-v8a')])
        def partial(e): e.pop('lib/arm64-v8a/' + sorted(NAMES)[0])
        def cross(e): e['lib/arm64-v8a/' + sorted(NAMES)[0]] = e['lib/x86_64/' + sorted(NAMES)[0]]
        def extra(e): e['assets/providers/angle-runtime-armeabi-v7a.json'] = b'{}'
        def metadata(e):
            n = 'assets/providers/angle-runtime-arm64-v8a.json'; j = json.loads(e[n]); j['abi'] = 'x86_64'; e[n] = json.dumps(j).encode()
        def collision(e):
            n = 'assets/providers/angle-runtime-arm64-v8a.json'; j = json.loads(e[n]); j['notice_group'] = 'angle'; e[n] = json.dumps(j).encode()
        def notice_extra(e): e['assets/licenses/angle-arm64-v8a/extra.txt'] = b'bad'
        def notice_non_directory(e): e['assets/licenses/angle'] = b'bad'
        def provider_extra(e): e['lib/arm64-v8a/libEGL_lighttouch_angle_extra.so'] = b'bad'
        for mutation in [partial, cross, extra, metadata, collision, notice_extra, notice_non_directory, provider_extra]:
            with self.subTest(mutation=mutation.__name__), self.assertRaises(AssertionError): self.consume(mutation)

    def test_full_apk_checks_keep_ZIP_and_ELF_alignment_enforcement(self):
        self.stage([self.bundle('x86_64'), self.bundle('arm64-v8a')])
        entries = {}
        for p in self.dest.rglob('*'):
            if p.is_file():
                entries[p.relative_to(self.dest).as_posix().replace('jniLibs/', 'lib/', 1)] = p.read_bytes()
        def package(case):
            path = self.root / (case + '.apk')
            with zipfile.ZipFile(path, 'w') as z:
                first = True
                for name, original in entries.items():
                    b = original
                    info = zipfile.ZipInfo(name)
                    if name.startswith('lib/'):
                        if case == 'ELF4k' and first:
                            data = bytearray(b); struct.pack_into('<Q', data, 64 + 48, 4096); b = bytes(data)
                        info.compress_type = zipfile.ZIP_DEFLATED if case == 'compressed' and first else zipfile.ZIP_STORED
                        padding = (-(z.fp.tell() + 30 + len(name.encode()))) % 16384
                        if padding and not (case == 'ZIPunaligned' and first):
                            if padding < 4: padding += 16384
                            info.extra = struct.pack('<HH', 0xCAFE, padding - 4) + bytes(padding - 4)
                        first = False
                    z.writestr(info, b)
            return path
        APK['check_apk'](package('valid'))
        for case in ['ELF4k', 'compressed', 'ZIPunaligned']:
            with self.subTest(case=case), self.assertRaises(AssertionError): APK['check_apk'](package(case))

    def test_actual_JNI_closed_source_contract(self):
        # Source guard/loader contract only. Actual ABI compilation stays separately allocated.
        source = (SCRIPTS.parent / 'app/src/main/cpp/lighttouch_jni.cpp').read_text()
        body = source.split('JNI(debugSelectGraphicsProvider)', 1)[1].split('JNI(debugEnableTouchTrace)', 1)[0]
        self.assertIn('#ifndef LT_ANDROID_DEBUG', body)
        self.assertIn('#if !defined(__x86_64__) && !defined(__aarch64__)', body)
        self.assertIn('if(runtime || starting)', body)
        self.assertIn('if(choice!=0 && choice!=1)', body)
        self.assertEqual(set(__import__('re').findall(r'"(lib[^"/]+\.so)"', body)), NAMES)
        self.assertIn('dlopen(names[i],RTLD_NOW|RTLD_LOCAL)', body)
        self.assertLess(body.index('if(runtime || starting)'), body.index('graphics_provider_choice=unsigned(choice)'))
        self.assertNotIn('getenv(', body)
        self.assertIn('if(graphics_provider_fixed)', body)


if __name__ == '__main__': unittest.main()
