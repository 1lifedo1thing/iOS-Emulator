#!/usr/bin/env python3
"""Create immutable corresponding-source and static LGPL relink material."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('engine_notices', Path(__file__).with_name('package-notices.py'))
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)
ASSET_SUFFIXES = {'.ipsw', '.itfw', '.itbase', '.ipa', '.dmg', '.rom', '.bin', '.img',
                  '.png', '.jpg', '.jpeg', '.gif', '.heic', '.mp3', '.m4a', '.aac',
                  '.mp4', '.mov', '.pdf', '.icns', '.ico', '.so', '.a', '.o', '.dylib'}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('build', type=Path)
    p.add_argument('destination', type=Path)
    p.add_argument('--deps', type=Path, required=True)
    p.add_argument('--crypto-deps', type=Path, required=True)
    p.add_argument('--notices', type=Path, required=True)
    p.add_argument('--ndk', type=Path, required=True)
    p.add_argument('--source-reference', required=True)
    a = p.parse_args()
    if subprocess.check_output(['git', 'status', '--porcelain=v1', '--untracked-files=all'], cwd=ROOT, text=True):
        p.error('Commit bundle tools before recording a reproducible generator revision')
    url = urlsplit(a.source_reference)
    if url.scheme != 'https' or not url.netloc or url.username or url.password:
        p.error('source-reference must be public HTTPS without embedded credentials')
    build, ndk = a.build.resolve(), a.ndk.resolve()
    engine = json.loads((build / 'portable-manifest.json').read_text())
    cache = json.loads((build / 'engine-inputs.json').read_text())
    revision = engine['source_revision']
    if engine['source_dirty'] or engine['source_status'] or sha(build / 'libqemu-arm.so') != engine['library_sha256']:
        p.error('engine must match its clean immutable manifest')
    if cache['abi'] not in ['arm64-v8a', 'x86_64']:
        p.error('this relink recipe currently supports pinned Bionic targets only')
    if sha(ndk / 'source.properties') != cache['ndk_properties_sha256']:
        p.error('NDK differs from engine input stamp')
    destination = a.destination.absolute()
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.engine-source-', dir=destination.parent))
    try:
        # Git archive reads the immutable source commit, never private assets.
        omitted = 0
        modes = {}
        process = subprocess.Popen(['git', 'archive', '--format=tar', revision], cwd=ROOT, stdout=subprocess.PIPE)
        try:
            with tarfile.open(fileobj=process.stdout, mode='r|') as archive:
                for member in archive:
                    name = Path(member.name)
                    if not member.isfile():
                        continue
                    if name.is_absolute() or '..' in name.parts:
                        raise RuntimeError('Unsafe immutable source archive path')
                    if (name.suffix.lower() in ASSET_SUFFIXES or name.parts[0] == 'manifests'
                            or 'screens' in name.parts or 'screenshots' in name.parts
                            or (name.parts[0] == 'docs' and name.as_posix() not in
                                ['docs/meson.build', 'docs/conf.py', 'docs/interop/firmware.json',
                                 'docs/interop/vhost-user.json'])):
                        omitted += 1
                        continue
                    data = archive.extractfile(member).read()
                    # QEMU's tracked, pinned pure-Python build-tool wheels are
                    # needed by offline configure and contain their notices.
                    build_wheel = name.parts[:2] == ('python', 'wheels') and name.suffix == '.whl'
                    if b'\0' in data and not build_wheel:
                        omitted += 1
                        continue
                    expected = engine['source_inputs'].get(name.as_posix())
                    if hashlib.sha256(data).hexdigest() != expected:
                        raise RuntimeError('Immutable source manifest mismatch: ' + name.as_posix())
                    target = stage / 'source/qemu' / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)
                    target.chmod(member.mode & 0o777)
                    modes['source/qemu/' + name.as_posix()] = member.mode & 0o777
        finally:
            process.stdout.close()
            if process.wait() != 0:
                raise RuntimeError('Immutable git archive failed')
        inventory = json.loads((build / 'selected-license-inventory.json').read_text())
        if (inventory['library_sha256'], inventory['source_revision']) != (engine['library_sha256'], revision):
            p.error('selected source audit is stale')
        for name in (set(inventory['selected_sources']) | set(inventory['selected_includes']) |
                     set(inventory.get('archive_source_candidates', {})) |
                     set(inventory.get('archive_include_candidates', {}))):
            if not (stage / 'source/qemu' / name).is_file():
                p.error('source exclusion omitted an actual selected source/header: ' + name)
        components = []
        archives = {Path(name).name: name for name in engine['link_inputs'] if name.endswith('.a')}
        replaceable = {}
        for pinfile, directory in [('contrib/portable/dependencies.json', a.deps),
                                   ('contrib/portable/crypto-dependencies.json', a.crypto_deps)]:
            pins = json.loads((stage / 'source/qemu' / pinfile).read_text())
            for package in pins['packages']:
                used = sorted(notices.ARCHIVES.get(package['name'], set()) & archives.keys())
                # Preserve every archive the recorded builder fetches, including
                # unlinked prerequisites used while building GLib or firmware's
                # crypto prefix. Selected binary inputs remain separately listed.
                archive = directory / package['archive']
                if sha(archive) != package['sha256']:
                    p.error('Release source archive differs from pin: ' + package['name'])
                target = stage / 'source/dependencies' / archive.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(archive, target)
                components.append({'name': package['name'], 'source_reference': package['url'],
                                   'role': 'linked' if used else 'build-prerequisite',
                                   'linked_archives': used,
                                   'source_archive': target.relative_to(stage).as_posix(),
                                   'source_archive_sha256': package['sha256']})
                if package['name'] in ['ffmpeg', 'glib', 'libgcrypt', 'libgpg-error', 'proxy-libintl']:
                    replaceable.update({name: 'relink/inputs/archives/' + name for name in used})
        # Include compiled libfdt and the preferred generated-keymap source.
        for name, component in [('dtc', 'libfdt'), ('keycodemapdb', 'keycodemapdb')]:
            wrap = (stage / 'source/qemu/subprojects' / (name + '.wrap')).read_text()
            subrevision = re.search(r'^revision\s*=\s*([0-9a-f]{40})$', wrap, re.M)[1]
            reference = re.search(r'^url\s*=\s*(\S+)$', wrap, re.M)[1]
            tree = ROOT / 'subprojects' / name
            if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=tree, text=True).strip() != subrevision:
                p.error(component + ' checkout differs from pin')
            subprocess.run(['git', 'diff', '--quiet', 'HEAD'], cwd=tree, check=True)
            target = stage / 'source/dependencies' / (name + '-source.tar')
            with target.open('wb') as stream:
                subprocess.run(['git', 'archive', '--format=tar', subrevision], cwd=tree, stdout=stream, check=True)
            components.append({'name': component, 'source_revision': subrevision,
                               'source_reference': reference,
                               'source_archive': target.relative_to(stage).as_posix(),
                               'source_archive_sha256': sha(target)})
        mappings = {}
        ndk_inputs = {}
        thin_members = {}
        for original, expected in engine['link_inputs'].items():
            path = Path(original)
            if sha(path) != expected:
                p.error('Actual link input differs from engine: ' + path.name)
            if path == build / 'libqemu-arm.so':
                continue  # Earlier manifests also record the final -o path.
            if path.suffix in ['.a', '.o']:
                relative = ('relink/inputs/archives/' + path.name if path.suffix == '.a' else
                            'relink/inputs/objects/' + path.relative_to(build).as_posix())
                target = stage / relative
                if target.exists() and sha(target) != expected:
                    p.error('Ambiguous archive name: ' + path.name)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
                mappings[original] = '@BUNDLE@/' + relative
                if path.suffix == '.a' and path.read_bytes()[:8] == b'!<thin>\n':
                    members = subprocess.check_output([
                        str(ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar'),
                        't', str(path)], text=True).splitlines()
                    for member in members:
                        child = Path(member)
                        if not child.is_absolute():
                            child = path.parent / child
                        child = child.resolve()
                        if not child.is_relative_to(path.parent.resolve()) or not child.is_relative_to(build):
                            p.error('Thin archive member escapes owned build directory')
                        member_target = target.parent / child.relative_to(path.parent.resolve())
                        member_target.parent.mkdir(parents=True, exist_ok=True)
                        recorded = engine.get('archive_member_inputs', {}).get(original)
                        if recorded is not None and recorded.get(str(child)) != sha(child):
                            p.error('Thin archive member differs from recorded engine manifest')
                        shutil.copyfile(child, member_target)
                        thin_members[member_target.relative_to(stage).as_posix()] = sha(child)
            elif path.is_relative_to(ndk):
                relative = path.relative_to(ndk).as_posix()
                mappings[original] = '@NDK@/' + relative
                ndk_inputs[relative] = expected
            else:
                p.error('Unmapped external link input: ' + path.name)
        shutil.copyfile(build / 'portable.exports', stage / 'relink/portable.exports')
        argv = []
        for index, arg in enumerate(engine['link_argv']):
            if index == 0:
                argv.append('@NDK@/' + Path(arg).relative_to(ndk).as_posix())
            elif arg.startswith('-L'):
                continue  # Every selected library is explicit below.
            elif arg in engine['resolved_libraries']:
                argv.append(mappings[engine['resolved_libraries'][arg]])
            elif arg.startswith('-Wl,--version-script='):
                argv.append('-Wl,--version-script=@BUNDLE@/relink/portable.exports')
            elif arg == str(build / 'libqemu-arm.so'):
                argv.append('@OUTPUT@')
            elif arg in mappings:
                argv.append(mappings[arg])
            elif (build / arg).as_posix() in mappings:
                argv.append(mappings[(build / arg).as_posix()])
            elif arg.startswith('/') or '/home/' in arg:
                p.error('Unmapped absolute link argument')
            else:
                argv.append(arg)
        runtime_files = {}
        triple = 'aarch64-linux-android' if cache['abi'] == 'arm64-v8a' else 'x86_64-linux-android'
        for name, expected in engine['runtime_files'].items():
            relative = 'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/' + triple + '/' + name
            if sha(ndk / relative) != expected:
                p.error('Runtime differs from engine: ' + name)
            runtime_files[name] = {'ndk_path': relative, 'sha256': expected}
            ndk_inputs[relative] = expected
        notice_manifest = json.loads((a.notices / 'manifest.json').read_text())
        if (notice_manifest['library_sha256'], notice_manifest['source_revision']) != (engine['library_sha256'], revision):
            p.error('Notice bundle does not match engine')
        for item in notice_manifest['files'] + [{'path': 'manifest.json', 'sha256': sha(a.notices / 'manifest.json')}]:
            source = a.notices / item['path']
            if sha(source) != item['sha256']:
                p.error('Notice checksum mismatch')
            target = stage / 'notices' / item['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        for name in ['relink-bundle.py', 'prepare-bundle-source.py']:
            shutil.copyfile(Path(__file__).with_name(name), stage / name)
        shutil.copyfile(Path(__file__).with_name('SOURCE-BUNDLE.md'), stage / 'README.md')
        metadata = {'schema': 1, 'abi': cache['abi'], 'source_revision': revision,
                    'source_reference': a.source_reference, 'engine_sha256': engine['library_sha256'],
                    'generator_revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                    'components': components, 'omitted_nonbuild_asset_count': omitted,
                    'scope': 'engine corresponding source and relink material; application review and publication completeness are separate',
                    'object_metadata': 'Original linked objects retain compiler debug/build-path metadata; no guest inputs are read.',
                    'source_modes': modes,
                    'thin_archive_members': thin_members,
                    'thin_member_provenance': 'Actual member bytes hashed at packaging; original relink receipt establishes correspondence for older engine manifests that record only archive headers.',
                    'relink': {'compiler': Path(engine['link_argv'][0]).relative_to(ndk).as_posix(),
                               'compiler_sha256': sha(Path(engine['link_argv'][0])),
                               'ndk_properties_sha256': cache['ndk_properties_sha256'],
                               'argv': argv, 'ndk_inputs': ndk_inputs,
                               'replaceable_archives': replaceable, 'runtime_files': runtime_files}}
        metadata['files'] = {path.relative_to(stage).as_posix(): sha(path)
                             for path in sorted(stage.rglob('*')) if path.is_file()}
        (stage / 'bundle.json').write_text(json.dumps(metadata, indent=2) + '\n')
        if destination.exists():
            existing = {path.relative_to(destination).as_posix(): sha(path)
                        for path in destination.rglob('*') if path.is_file() and not path.is_symlink()}
            generated = {**metadata['files'], 'bundle.json': sha(stage / 'bundle.json')}
            if existing != generated or any(path.is_symlink() for path in destination.rglob('*')):
                p.error('Destination differs; choose a fresh immutable bundle directory')
        else:
            os.rename(stage, destination)
        print(json.dumps({'status': 'PACKAGED', 'manifest_sha256': sha(destination / 'bundle.json'),
                          'files': len(metadata['files']), 'engine_sha256': engine['library_sha256']}, sort_keys=True))
    finally:
        if stage.exists():
            shutil.rmtree(stage)


if __name__ == '__main__':
    main()
