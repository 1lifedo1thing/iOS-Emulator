#!/usr/bin/env python3
"""Read-only diagnostic engine/APK verification; never compiles or runs a guest."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import zipfile


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load(path):
    return json.loads(Path(path).read_text())


def members(root, records):
    root = Path(root)
    actual = set()
    for path in root.rglob('*'):
        require(not path.is_symlink(), 'Symlink in verified directory')
        if path.is_file():
            actual.add(path.relative_to(root).as_posix())
    for name, expected in records.items():
        path = Path(name)
        require(not path.is_absolute() and '..' not in path.parts, 'Unsafe verified member')
        require(sha(root / path) == expected, 'Verified member hash differs: ' + name)
    return actual


def readelf_check(library, readelf, expected_exports, expected_needed):
    raw = Path(library).read_bytes()
    require(len(raw) >= 64 and raw[:7] == b'\x7fELF\x02\x01\x01' and
            struct.unpack_from('<HH', raw, 16) == (3, 183), 'Expected AArch64 ELF64 shared engine')
    reports = {}
    for name, flags in [('dynamic', ['-dW']), ('segments', ['-lW']), ('symbols', ['--dyn-syms', '-W'])]:
        reports[name] = subprocess.check_output([str(readelf), *flags, str(library)], text=True, timeout=15)
    exports = set()
    for line in reports['symbols'].splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[4] in ('GLOBAL', 'WEAK') and fields[6] != 'UND':
            exports.add(fields[7].split('@', 1)[0])
    require(exports == set(expected_exports), 'Engine exports differ')
    needed = re.findall(r'\(NEEDED\).*\[([^]]+)\]', reports['dynamic'])
    require(set(needed) == set(expected_needed) and len(needed) == len(set(needed)), 'Engine needed closure differs')
    require(re.findall(r'\(SONAME\).*\[([^]]+)\]', reports['dynamic']) == ['libqemu-arm.so'], 'Engine SONAME differs')
    alignment = [int(line.split()[-1], 16) for line in reports['segments'].splitlines()
                 if line.strip().startswith('LOAD ')]
    require(alignment and all(value == 16384 for value in alignment), 'Engine PT_LOAD alignment differs')
    return {'exports': len(exports), 'needed': needed, 'loadAlignment': alignment,
            'reportSha256': {name: hashlib.sha256(text.encode()).hexdigest() for name, text in reports.items()}}


def verify_engine(plan_path, expected_plan_sha):
    plan_path = Path(plan_path)
    require(sha(plan_path) == expected_plan_sha, 'Native plan hash differs')
    plan = load(plan_path)
    require(plan['android_api'] == 29 and len(plan['targets']) == 1 and plan['targets'][0]['abi'] == 'arm64-v8a',
            'Expected single ARM64/API29 diagnostic target')
    target = plan['targets'][0]
    run = Path(plan['run_output'])
    terminal = load(run / 'terminal.json')
    require(terminal['approved_plan_sha256'] == expected_plan_sha and terminal['completed_abis'] == ['arm64-v8a'],
            'Native terminal incomplete or differently bound')
    require(terminal['commands'] and all(row.get('exit') == 0 and row.get('cleanup_session_retired', True)
                                        for row in terminal['commands']), 'Native command failed or did not retire')
    artifacts = load(run / 'artifacts.json')
    require(len(artifacts) == 1 and artifacts[0]['abi'] == 'arm64-v8a', 'Unexpected native artifact cohort')
    row = artifacts[0]
    root = Path(row['frozen'])
    for name in ('libqemu-arm.so', 'libc++_shared.so', 'portable-manifest.json'):
        require((root / name).is_file() and not (root / name).is_symlink() and
                (root / name).stat().st_mode & 0o222 == 0, 'Native frozen input must be immutable')
    manifest = load(root / 'portable-manifest.json')
    require(manifest['schema'] == 2 and manifest['api_major'] == 2 and manifest['portable_version'] == 1,
            'Engine API manifest differs')
    require(manifest['source_revision'] == row['native_source_revision'] == row['packaging_tool_revision'] == plan['source_revision'] and
            not manifest['source_dirty'] and manifest['source_status'] == '', 'Engine source identity differs')
    require(sha(root / 'libqemu-arm.so') == manifest['library_sha256'] == row['library_sha256'], 'Frozen engine hash differs')
    require(sha(root / 'portable-manifest.json') == row['manifest_sha256'], 'Frozen manifest hash differs')
    require(manifest['runtime_files']['libc++_shared.so'] == sha(root / 'libc++_shared.so') == sha(target['cpp_input']),
            'Engine C++ runtime identity differs')
    # Cross-check diagnostic production inputs against pinned clean source, not
    # merely presence of property names in an arbitrary library string table.
    source = Path(plan['source_root'])
    pins = {item['path']: item['sha256'] for item in plan['files']}
    for name in ('include/hw/arm/ipod_touch_lcd.h', 'hw/arm/ipod_touch_lcd.c', 'contrib/ios-app/qemu-ios-ui.c'):
        require(manifest['source_inputs'][name] == sha(source / name) == pins[str(source / name)], 'Diagnostic source input differs: ' + name)
    elf = readelf_check(root / 'libqemu-arm.so', target['readelf'], target['expected_exports'], target['expected_needed'])
    notice_root = Path(target['notices'])
    notice = load(notice_root / 'manifest.json')
    require(sha(notice_root / 'manifest.json') == row['notice_manifest_sha256'] and notice['schema'] == 1 and
            notice['source_revision'] == plan['source_revision'] and notice['library_sha256'] == row['library_sha256'],
            'Notice source/engine binding differs')
    notice_files = {item['path']: item['sha256'] for item in notice['files']}
    require(len(notice_files) == len(notice['files']), 'Duplicate notice member')
    require(members(notice_root, notice_files) == {'manifest.json'} | set(notice_files), 'Notice closure differs')
    runtime = [item for item in notice['components'] if item['name'] == 'Android NDK runtime/toolchain notices']
    require(len(runtime) == 1 and runtime[0]['runtime_files']['libc++_shared.so'] == sha(root / 'libc++_shared.so'),
            'Notice runtime binding differs')
    bundle_root = Path(target['sourcekit'])
    bundle = load(bundle_root / 'bundle.json')
    require(sha(bundle_root / 'bundle.json') == row['sourcekit_manifest_sha256'] and bundle['schema'] == 1 and
            bundle['abi'] == 'arm64-v8a' and bundle['source_revision'] == bundle['generator_revision'] == plan['source_revision'] and
            bundle['engine_sha256'] == row['library_sha256'], 'Sourcekit binding differs')
    require(members(bundle_root, bundle['files']) == {'bundle.json'} | set(bundle['files']), 'Sourcekit closure differs')
    for name, mode in bundle['source_modes'].items():
        require(name in bundle['files'] and (bundle_root / name).stat().st_mode & 0o777 == mode,
                'Sourcekit mode differs: ' + name)
    return {'pass': True, 'kind': 'NATIVE_ARTIFACT_VERIFIED_NOT_GUEST_ACCEPTANCE',
            'sourceRevision': plan['source_revision'], 'librarySha256': row['library_sha256'], 'elf': elf,
            'noticeFiles': len(notice_files), 'sourcekitFiles': len(bundle['files']),
            'freshRelinkPerformed': False, 'targetExecutionVerified': False}


def verify_payloads(original, signed, receipt):
    require(sha(original) == receipt['originalApkSha256'], 'Original APK hash differs')
    unchanged, replaced = receipt['unchangedPayloads'], receipt['replacementPayloads']
    require(not set(unchanged) & set(replaced), 'Receipt payload groups overlap')
    require(replaced.get('lib/arm64-v8a/libqemu-arm.so') == receipt['engineSha256'], 'Receipt engine binding differs')
    prefix = 'assets/licenses/engine-arm64-v8a/'
    require(all(name == 'lib/arm64-v8a/libqemu-arm.so' or name.startswith(prefix) for name in replaced),
            'Unexpected replacement in engine-only receipt')
    with zipfile.ZipFile(original) as before, zipfile.ZipFile(signed) as after:
        require(len(before.namelist()) == len(set(before.namelist())) and len(after.namelist()) == len(set(after.namelist())),
                'Duplicate APK payload')
        require(set(after.namelist()) == set(unchanged) | set(replaced), 'Signed APK closure differs')
        # V2 signatures are outside ZIP payloads. No new JAR signature entries
        # may silently broaden this exact payload closure.
        for name, expected in unchanged.items():
            require(hashlib.sha256(before.read(name)).hexdigest() == expected ==
                    hashlib.sha256(after.read(name)).hexdigest(), 'Unrelated APK payload changed: ' + name)
            require(before.getinfo(name).compress_type == after.getinfo(name).compress_type,
                    'Unrelated APK compression changed: ' + name)
        for name, expected in replaced.items():
            require(hashlib.sha256(after.read(name)).hexdigest() == expected, 'Replacement APK payload differs: ' + name)
        allowed_removed = {name for name in before.namelist() if name.startswith(prefix) or
                           name.upper() == 'META-INF/MANIFEST.MF' or
                           re.fullmatch(r'META-INF/[^/]+\.(?:SF|RSA|DSA|EC)', name, re.I)}
        require(set(before.namelist()) - set(unchanged) - {'lib/arm64-v8a/libqemu-arm.so'} == allowed_removed,
                'Receipt omitted unrelated original APK payload')
    return {'preservedPayloads': len(unchanged), 'replacedPayloads': len(replaced), 'signedApkSha256': sha(signed)}


def verify_apk(original, signed, receipt_path, apksigner, zipalign, expected_certificate, expected_engine_sha):
    receipt = load(receipt_path)
    require(receipt['schema'] == 1 and receipt['kind'] == 'UNSIGNED_ENGINE_ONLY_DIAGNOSTIC_NOT_TARGET_ACCEPTANCE',
            'Wrong diagnostic receipt kind')
    require(receipt['engineSha256'] == expected_engine_sha, 'Receipt differs from independently verified native engine')
    result = verify_payloads(original, signed, receipt)
    certificate = subprocess.check_output([str(apksigner), 'verify', '--verbose', '--print-certs', str(signed)], text=True, timeout=20)
    certs = re.findall(r'Signer #\d+ certificate SHA-256 digest: ([0-9a-f]+)', certificate)
    require(certs == [expected_certificate] and 'Verified using v2 scheme (APK Signature Scheme v2): true' in certificate,
            'Signing certificate or V2 signature differs')
    subprocess.run([str(zipalign), '-c', '-P', '16', '4', str(signed)], check=True, capture_output=True, timeout=20)
    spec = importlib.util.spec_from_file_location('apk_check', Path(__file__).with_name('check-apk.py'))
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    checker.check_apk(signed)
    result.update(passVerified=True, kind='SIGNED_DIAGNOSTIC_PAYLOAD_VERIFIED_NOT_GUEST_ACCEPTANCE',
                  certificateSha256=expected_certificate, targetExecutionVerified=False)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    native = sub.add_parser('engine')
    native.add_argument('--plan', type=Path, required=True)
    native.add_argument('--plan-sha256', required=True)
    apk = sub.add_parser('apk')
    for field in ('original', 'signed', 'receipt', 'apksigner', 'zipalign'):
        apk.add_argument('--' + field, type=Path, required=True)
    apk.add_argument('--certificate-sha256', required=True)
    apk.add_argument('--engine-sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), 'Verification output already exists')
    result = verify_engine(args.plan, args.plan_sha256) if args.mode == 'engine' else verify_apk(
            args.original, args.signed, args.receipt, args.apksigner, args.zipalign, args.certificate_sha256, args.engine_sha256)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps(result))
