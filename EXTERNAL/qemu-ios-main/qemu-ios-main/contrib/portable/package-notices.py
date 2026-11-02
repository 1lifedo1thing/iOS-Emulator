#!/usr/bin/env python3
"""Package offline notices for an immutable engine's actual linked inputs."""
import argparse
import hashlib
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
NDK_NOTICES = {
    'NOTICE': 'faf0dc52cf6ce9ab09a04b0c074a378dd2565ee1ed9bc1179d83793bcdba012e',
    'toolchains/llvm/prebuilt/linux-x86_64/NOTICE': 'f96f763beb66a7ba7a667647fc64c0226ace875e590c831fdd9579ec1c1d91e1',
    'toolchains/llvm/prebuilt/linux-x86_64/sysroot/NOTICE': '2905ac5c59628352af990c748a8f4c3675a04f36eca7914612e3068661d2c6a6',
}
ARCHIVES = {
    'ffmpeg': {'libavcodec.a', 'libavutil.a'},
    'glib': {'libglib-2.0.a', 'libgobject-2.0.a', 'libgio-2.0.a'},
    'pcre2': {'libpcre2-8.a'}, 'pixman': {'libpixman-1.a'},
    'proxy-libintl': {'libintl.a'}, 'libffi': {'libffi.a'},
    'libgcrypt': {'libgcrypt.a'}, 'libgpg-error': {'libgpg-error.a'},
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git_bytes(revision, name, root=ROOT):
    return subprocess.check_output(['git', 'show', revision + ':' + name], cwd=root)


def leading_notices(data):
    """Keep initial C/C++ copyright/license comments, not implementation code."""
    text = data.decode('utf-8', errors='replace')
    match = re.match(r'\A\s*((?:(?:/\*.*?\*/|//[^\n]*(?:\n|$))\s*)+)', text, re.S)
    if not match:
        return ''
    comments = match[1].strip()
    if re.search(r'copyright|license|SPDX|permission|public domain', comments, re.I):
        return comments
    return ''


def notice_archive_inputs(inputs):
    """Verify archive pins; record truly empty inputs without inventing code."""
    linked, empty = {}, {}
    for name, expected in inputs.items():
        path = Path(name)
        if path.suffix != '.a':
            continue
        data = path.read_bytes()
        if sha(data) != expected:
            raise ValueError('linked archive changed: ' + path.name)
        if path.name in linked and linked[path.name] != expected:
            raise ValueError('ambiguous linked archive: ' + path.name)
        linked[path.name] = expected
        # A regular ar magic with no following bytes cannot contain an object,
        # index or external member. Do not exempt thin or nonempty archives.
        if data == b'!<arch>\n':
            entry = empty.setdefault(path.name, {'sha256': expected, 'member_count': 0,
                                                'input_paths': []})
            entry['input_paths'].append(str(path))
    covered = {'libqemuutil.a', 'libltm-pvrtc.a', 'libfdt.a', 'libatomic.a', 'libslirp.a'}
    covered.update(lib for names in ARCHIVES.values() for lib in names)
    unmapped = linked.keys() - covered - empty.keys()
    if unmapped:
        raise ValueError('unmapped selected archives require notice review: ' + ', '.join(sorted(unmapped)))
    return linked, empty


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--deps', type=Path, required=True, help='native dependency build directory')
    parser.add_argument('--crypto-deps', type=Path, required=True)
    parser.add_argument('--ndk', type=Path, help='NDK used for this Bionic engine')
    parser.add_argument('--source-reference', required=True, help='public corresponding-source HTTPS URL')
    args = parser.parse_args()
    generator_revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    generator_name = Path(__file__).relative_to(ROOT).as_posix()
    generator_data = Path(__file__).read_bytes()
    if git_bytes(generator_revision, generator_name) != generator_data:
        parser.error('commit notice generator before recording its revision')
    url = urlsplit(args.source_reference)
    if url.scheme != 'https' or not url.netloc or url.username or url.password:
        parser.error('source-reference must be HTTPS without embedded credentials')
    build = args.build.resolve()
    engine = json.loads((build / 'portable-manifest.json').read_text())
    revision = engine['source_revision']
    if not re.fullmatch('[0-9a-f]{40}', revision) or engine['source_dirty'] or engine['source_status']:
        parser.error('engine must record a clean immutable source revision')
    if sha((build / 'libqemu-arm.so').read_bytes()) != engine['library_sha256']:
        parser.error('engine library differs from recorded manifest')
    inventory = json.loads((build / 'selected-license-inventory.json').read_text())
    if (inventory['source_revision'], inventory['library_sha256']) != (revision, engine['library_sha256']):
        parser.error('selected-source inventory differs from engine; regenerate its audit')
    try:
        linked, empty_archives = notice_archive_inputs(engine['link_inputs'])
    except ValueError as error:
        parser.error(str(error))
    files = {}
    components = []

    def tracked(name):
        data = git_bytes(revision, name)
        if sha(data) != engine['source_inputs'].get(name):
            parser.error('tracked source differs from recorded commit: ' + name)
        return data

    # Project-specific grants are retained from each selected source below;
    # this branch has no aggregate licenses/S5LBox-MIT.txt document.
    for name in ['COPYING', 'COPYING.LIB', 'LICENSE',
                 'hw/arm/powervr/LICENSE.md', 'hw/arm/powervr/README.md']:
        files['qemu/' + name] = tracked(name)
    snippets = []
    selected = {**inventory.get('archive_source_candidates', {}),
                **inventory.get('archive_include_candidates', {}),
                **inventory['selected_sources'], **inventory['selected_includes']}
    for name in sorted(selected):
        data = tracked(name)
        if sha(data) != selected[name]['sha256']:
            parser.error('selected inventory source checksum mismatch: ' + name)
        notice = leading_notices(data)
        if notice:
            snippets.append(name + '\n' + '=' * len(name) + '\n' + notice + '\n')
    # This archive's unmodified reference decoder is not represented as a direct .o.
    for name in ['hw/arm/powervr/PVRTDecompress.cpp', 'hw/arm/powervr/PVRTDecompress.h']:
        snippets.append(name + '\n' + leading_notices(tracked(name)) + '\n')
    files['qemu/SELECTED-SOURCE-NOTICES.txt'] = ('\n'.join(snippets)).encode()
    components.append({'name': 'QEMU/LightTouch', 'source_revision': revision,
                       'source_reference': args.source_reference,
                       'selected_source_count': len(inventory['selected_sources']),
                       'selected_include_count': len(inventory['selected_includes'])})

    for pinfile, directory in [('contrib/portable/dependencies.json', args.deps),
                               ('contrib/portable/crypto-dependencies.json', args.crypto_deps)]:
        for package in json.loads(tracked(pinfile))['packages']:
            name = package['name']
            used = sorted(ARCHIVES.get(name, set()) & linked.keys())
            if not used:
                continue
            archive = directory / package['archive']
            if sha(archive.read_bytes()) != package['sha256']:
                parser.error('dependency archive checksum mismatch: ' + name)
            count = 0
            with tarfile.open(archive) as tar:
                for member in tar.getmembers():
                    parts = Path(member.name).parts
                    if not member.isfile() or len(parts) < 2:
                        continue
                    relative = Path(*parts[1:])
                    # Preserve original release legal documents and author credits.
                    top_notice = len(parts) == 2 and re.match(r'COPYING|LICENSE|AUTHORS|NOTICE', relative.name, re.I)
                    if not (top_notice or parts[1] == 'LICENSES'):
                        continue
                    stream = tar.extractfile(member)
                    files['dependencies/' + name + '/' + relative.as_posix()] = stream.read()
                    count += 1
            if not count:
                parser.error('no upstream notices in verified archive: ' + name)
            components.append({'name': name, 'source_reference': package['url'],
                               'archive_sha256': package['sha256'],
                               'linked_archives': {lib: linked[lib] for lib in used}})

    if 'libfdt.a' in linked:
        wrap = tracked('subprojects/dtc.wrap').decode()
        dtc_revision = re.search(r'^revision\s*=\s*([0-9a-f]{40})$', wrap, re.M)[1]
        dtc_url = re.search(r'^url\s*=\s*(\S+)$', wrap, re.M)[1]
        dtc = ROOT / 'subprojects/dtc'
        if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=dtc, text=True).strip() != dtc_revision:
            parser.error('libfdt source checkout differs from pinned revision')
        if subprocess.check_output(['git', 'diff', '--name-only', 'HEAD'], cwd=dtc, text=True):
            parser.error('libfdt tracked source checkout is dirty')
        for name in ['BSD-2-Clause', 'GPL', 'README.license']:
            files['dependencies/libfdt/' + name] = git_bytes(dtc_revision, name, dtc)
        dtc_notices = []
        for name in subprocess.check_output(['git', 'ls-tree', '-r', '--name-only', dtc_revision, 'libfdt'], cwd=dtc, text=True).splitlines():
            notice = leading_notices(git_bytes(dtc_revision, name, dtc))
            if notice:
                dtc_notices.append(name + '\n' + notice + '\n')
        files['dependencies/libfdt/SOURCE-NOTICES.txt'] = ('\n'.join(dtc_notices)).encode()
        components.append({'name': 'libfdt', 'source_reference': dtc_url,
                           'source_revision': dtc_revision, 'linked_archive_sha256': linked['libfdt.a']})

    if 'libslirp.a' in linked:
        wrap = tracked('subprojects/slirp.wrap').decode()
        slirp_revision = re.search(r'^revision\s*=\s*([0-9a-f]{40})$', wrap, re.M)[1]
        slirp_url = re.search(r'^url\s*=\s*(\S+)$', wrap, re.M)[1]
        slirp = ROOT / 'subprojects/slirp'
        if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=slirp, text=True).strip() != slirp_revision:
            parser.error('libslirp source checkout differs from pinned revision')
        # The wrap's diff_files, in the order meson applies them.
        patch_names = [n.strip() for n in re.search(r'^diff_files\s*=\s*(.+)$', wrap, re.M)[1].split(',')]
        patches = [tracked('subprojects/packagefiles/' + n) for n in patch_names]
        patch = b''.join(patches)
        # Judge the entire tracked dependency tree against the pinned source plus
        # the recorded integration edit; do not hide extra local dependency edits.
        with tempfile.TemporaryDirectory(prefix='slirp-notice-check-') as temporary:
            archive = subprocess.check_output(['git', 'archive', slirp_revision], cwd=slirp)
            subprocess.run(['tar', '-xf', '-', '-C', temporary], input=archive, check=True)
            for one in patches:
                subprocess.run(['git', 'apply', '-'], cwd=temporary, input=one, check=True)
            entries = subprocess.check_output(['git', 'ls-tree', '-r', slirp_revision], cwd=slirp, text=True).splitlines()
            for entry in entries:
                metadata, name = entry.split('\t', 1)
                if metadata.startswith('160000 '):
                    continue  # Fuzzing corpus submodules are not libslirp inputs.
                expected, actual = Path(temporary) / name, slirp / name
                same = (actual.is_symlink() and os.readlink(expected) == os.readlink(actual)) if metadata.startswith('120000 ') else (
                    not actual.is_symlink() and expected.read_bytes() == actual.read_bytes())
                if not same:
                    parser.error('libslirp source differs from pinned integration: ' + name)
        for name in ['COPYRIGHT', 'LICENSE']:
            files['dependencies/libslirp/' + name] = git_bytes(slirp_revision, name, slirp)
        components.append({'name': 'libslirp', 'source_reference': slirp_url,
                           'source_revision': slirp_revision, 'integration_patch_sha256': sha(patch),
                           'linked_archive_sha256': linked['libslirp.a']})

    wrap = tracked('subprojects/keycodemapdb.wrap').decode()
    key_revision = re.search(r'^revision\s*=\s*([0-9a-f]{40})$', wrap, re.M)[1]
    key_url = re.search(r'^url\s*=\s*(\S+)$', wrap, re.M)[1]
    key_tree = ROOT / 'subprojects/keycodemapdb'
    if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=key_tree, text=True).strip() != key_revision:
        parser.error('generated keymap source differs from pinned revision')
    subprocess.run(['git', 'diff', '--quiet', 'HEAD'], cwd=key_tree, check=True)
    for name in ['LICENSE.BSD', 'LICENSE.GPL2', 'README']:
        files['dependencies/keycodemapdb/' + name] = git_bytes(key_revision, name, key_tree)
    components.append({'name': 'keycodemapdb generated keymaps', 'source_reference': key_url,
                       'source_revision': key_revision,
                       'grant': 'package and generated output dual GPL2-or-later/BSD3; upstream README'})

    if engine.get('runtime_files') or 'libatomic.a' in linked:
        if not args.ndk:
            parser.error('Bionic engine notices require its --ndk')
        properties = (args.ndk / 'source.properties').read_text()
        version = re.search(r'^Pkg.Revision\s*=\s*(\S+)$', properties, re.M)[1]
        if version != '28.2.13676358':
            parser.error('NDK release differs from portable pin')
        for name in engine['link_inputs']:
            if Path(name).name == 'libatomic.a' and not Path(name).resolve().is_relative_to(args.ndk.resolve()):
                parser.error('NDK differs from linked atomic runtime input')
        for name, expected in engine.get('runtime_files', {}).items():
            if sha((build / name).read_bytes()) != expected:
                parser.error('staged NDK runtime differs from engine manifest: ' + name)
        for name, expected in NDK_NOTICES.items():
            data = (args.ndk / name).read_bytes()
            if sha(data) != expected:
                parser.error('NDK notice differs from pinned release: ' + name)
            files['ndk/' + name.replace('/', '-') + '.txt'] = data
        components.append({'name': 'Android NDK runtime/toolchain notices', 'version': version,
                           'source_reference': 'https://android.googlesource.com/toolchain/llvm-project/',
                           'scope': 'release notices include components beyond the selected runtime',
                           'runtime_files': engine.get('runtime_files', {})})

    files['NOTICE.engine.txt'] = (
        'Offline notices for the actual linked LightTouch portable engine.\n'
        'Library SHA256: ' + engine['library_sha256'] + '\n'
        'These notices do not determine distribution eligibility or clear the complete APK.\n'
        'Corresponding source, patches, reproducible build and applicable static LGPL\n'
        'relinking material must accompany any distribution under their license terms.\n'
        'Source-reference availability must be verified by the publisher.\n'
        'Guest firmware, ROMs, NAND and private device state are not included.\n').encode()
    manifest = {'schema': 1, 'scope': 'selected native engine notices; distribution review unresolved',
                'source_reference': args.source_reference, 'source_revision': revision,
                'generator_revision': generator_revision,
                'generator_script_sha256': sha(generator_data),
                'source_reference_availability': 'publisher must verify before distribution',
                'library_sha256': engine['library_sha256'], 'components': components,
                'empty_archive_inputs': empty_archives,
                'empty_archive_scope': 'verified regular ar magic only; zero members, no object payload',
                'files': [{'path': name, 'sha256': sha(data)} for name, data in sorted(files.items())]}
    files['manifest.json'] = (json.dumps(manifest, indent=2, ensure_ascii=True) + '\n').encode()
    destination = args.destination.absolute()
    if destination.exists():
        actual = {str(p.relative_to(destination)): p.read_bytes() for p in destination.rglob('*') if p.is_file() and not p.is_symlink()}
        if actual != files or any(p.is_symlink() for p in destination.rglob('*')):
            parser.error('destination differs; use a fresh generated-assets directory')
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix='.engine-notices-', dir=destination.parent))
        try:
            for name, data in sorted(files.items()):
                path = stage / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                path.chmod(0o644)
            os.rename(stage, destination)
        finally:
            if stage.exists():
                shutil.rmtree(stage)
    print(json.dumps({'schema': 1, 'manifest_sha256': sha(files['manifest.json']),
                      'files': len(files), 'status': 'OK'}, sort_keys=True))


if __name__ == '__main__':
    main()
