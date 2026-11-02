#!/usr/bin/env python3
"""Prepare an unsigned engine-only diagnostic APK; never sign/install/run it.

The original APK and its cached native/managed code remain immutable. The
caller must subsequently zipalign, sign with the original debug certificate,
and independently verify the signed payload and 16 KiB native alignment.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def require(ok, why):
    if not ok:
        raise ValueError(why)


def signature(name):
    return name.upper() == 'META-INF/MANIFEST.MF' or bool(
            re.fullmatch(r'META-INF/[^/]+\.(?:SF|RSA|DSA|EC)', name, re.I))


def cpp_runtime_proof(packaged, runtime, strip_tool=None):
    expected = hashlib.sha256(packaged).hexdigest()
    original = sha(runtime)
    if expected == original:
        return {'kind': 'EXACT_BYTES', 'runtimeSha256': original}
    require(strip_tool is not None, 'C++ runtime changed; explicit exact stripping proof required')
    # LLVM dispatches strip vs objcopy from argv[0]; retain the strip symlink
    # name while hashing the actual followed executable bytes.
    tool = Path(strip_tool).absolute()
    require(tool.is_file(), 'Stripping tool missing')
    tool_hash = sha(tool)
    with tempfile.TemporaryDirectory() as temporary:
        stripped = Path(temporary) / 'libc++_shared.so'
        subprocess.run([str(tool), '--strip-unneeded', '-o', str(stripped), str(runtime)],
                       check=True, capture_output=True, timeout=15)
        require(sha(stripped) == expected, 'C++ runtime changed after exact stripping reproduction')
    require(sha(tool) == tool_hash and sha(runtime) == original, 'Stripping inputs changed')
    return {'kind': 'EXACT_STRIP_UNNEEDED_REPRODUCTION', 'runtimeSha256': original,
            'packagedSha256': expected, 'stripTool': str(tool), 'stripToolSha256': tool_hash}


def repack(base, expected_base_sha, engine_root, notices, expected_engine_revision, output, strip_tool=None):
    base, engine_root, notices, output = map(Path, (base, engine_root, notices, output))
    require(not output.exists() and not output.is_symlink(), 'Output already exists')
    require(sha(base) == expected_base_sha, 'Original APK hash changed')
    abi = 'arm64-v8a'
    library = engine_root / abi / 'libqemu-arm.so'
    cpp = engine_root / abi / 'libc++_shared.so'
    manifest = json.loads((engine_root / abi / 'portable-manifest.json').read_text())
    require(manifest['source_revision'] == expected_engine_revision and not manifest['source_dirty'],
            'Diagnostic engine source identity changed')
    require(manifest['library_sha256'] == sha(library), 'Engine manifest hash mismatch')
    # Reuse the maintained complete notice verifier, including C++ runtime hash.
    spec = importlib.util.spec_from_file_location('engine_notices',
            Path(__file__).with_name('stage-engine-notices.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    prefix = 'assets/licenses/engine-arm64-v8a/'
    engine_entry = 'lib/arm64-v8a/libqemu-arm.so'
    preserved, replaced, removed = {}, {}, []
    with tempfile.TemporaryDirectory() as temporary:
        staged = Path(temporary) / 'licenses'
        module.stage(staged, engine_root, [(abi, notices)])
        replacements = {engine_entry: library}
        replacements.update({prefix + path.relative_to(staged / ('engine-' + abi)).as_posix(): path
                             for path in (staged / ('engine-' + abi)).rglob('*') if path.is_file()})
        with zipfile.ZipFile(base) as original:
            names = original.namelist()
            require(len(names) == len(set(names)), 'Duplicate original APK entry')
            require(engine_entry in names and 'lib/arm64-v8a/libc++_shared.so' in names,
                    'Original APK lacks expected ARM64 runtime')
            require(any(n.startswith(prefix) for n in names), 'Original engine notices missing')
            cpp_proof = cpp_runtime_proof(original.read('lib/arm64-v8a/libc++_shared.so'), cpp, strip_tool)
            with zipfile.ZipFile(output, 'x') as destination:
                for info in original.infolist():
                    require(not info.filename.startswith('/') and '..' not in info.filename.split('/'),
                            'Unsafe original APK entry')
                    if signature(info.filename):
                        removed.append(info.filename)
                        continue
                    if info.filename.startswith(prefix):
                        removed.append(info.filename)
                        continue
                    payload = library.read_bytes() if info.filename == engine_entry else original.read(info)
                    destination.writestr(copy.copy(info), payload)
                    digest = hashlib.sha256(payload).hexdigest()
                    (replaced if info.filename == engine_entry else preserved)[info.filename] = digest
                for name, source in sorted(replacements.items()):
                    if name == engine_entry:
                        continue
                    destination.writestr(name, source.read_bytes(), compress_type=zipfile.ZIP_DEFLATED)
                    replaced[name] = sha(source)
    require(sha(base) == expected_base_sha, 'Original APK changed while packaging')
    with zipfile.ZipFile(output) as staged:
        require(set(staged.namelist()) == set(preserved) | set(replaced), 'Unsigned APK closure changed')
        require(all(hashlib.sha256(staged.read(name)).hexdigest() == digest
                    for name, digest in {**preserved, **replaced}.items()), 'Unsigned APK payload mismatch')
    return {'schema': 1, 'kind': 'UNSIGNED_ENGINE_ONLY_DIAGNOSTIC_NOT_TARGET_ACCEPTANCE',
            'originalApkSha256': expected_base_sha, 'unsignedApkSha256': sha(output),
            'engineRevision': expected_engine_revision, 'engineSha256': sha(library),
            'cppRuntimeProof': cpp_proof,
            'unchangedPayloads': preserved, 'replacementPayloads': replaced,
            'removedEntries': removed, 'nativeManagedRecompiled': False,
            'zipalignRequired': True, 'signingRequired': True, 'targetExecutionVerified': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-apk', type=Path, required=True)
    parser.add_argument('--base-sha256', required=True)
    parser.add_argument('--engine-root', type=Path, required=True)
    parser.add_argument('--notices', type=Path, required=True)
    parser.add_argument('--engine-revision', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--strip-tool', type=Path,
                        help='Reproduce original stripped C++ runtime exactly; never replaces its APK payload')
    args = parser.parse_args()
    require(not args.receipt.exists(), 'Receipt already exists')
    receipt = repack(args.base_apk, args.base_sha256, args.engine_root, args.notices,
                     args.engine_revision, args.output, args.strip_tool)
    with args.receipt.open('x') as stream:
        json.dump(receipt, stream, indent=2)
        stream.write('\n')
    print(json.dumps({key: value for key, value in receipt.items()
                      if key not in ('unchangedPayloads', 'replacementPayloads', 'removedEntries')}))
