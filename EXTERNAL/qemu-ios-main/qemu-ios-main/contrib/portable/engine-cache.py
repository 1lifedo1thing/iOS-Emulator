#!/usr/bin/env python3
"""Explicit source-only Ninja updates with invariant configuration checks."""
import json
import pathlib
import subprocess
from cache import check_cache, digest

# Reviewed pre-update recipe. Its exact argv/dependency/NDK/cache fields must
# match; the existing compiled manifest supplies compiler/Ninja baselines.
LEGACY_RECIPE = '069ac3eddbed8773cbedddc1ae4747064ed4a83d0d979f7fa2018e49601332cf'

def check_engine_cache(output, identity, markers, source_update=False):
    output = pathlib.Path(output)
    stamp = output / 'engine-inputs.json'
    if not source_update or not stamp.exists():
        return check_cache(output, identity, markers, 'engine')
    previous = json.loads(stamp.read_text())
    allowed = {'source_revision'}
    legacy = previous.get('recipe_sha256') == LEGACY_RECIPE and previous.get('schema') == 1
    manifest_path = output / 'portable-manifest.json'
    if not manifest_path.exists():
        raise RuntimeError('Source update requires a completed recorded engine build')
    manifest = json.loads(manifest_path.read_text())
    if manifest['source_dirty'] or digest(output / 'libqemu-arm.so') != manifest['library_sha256']:
        raise RuntimeError('Source update requires an intact clean engine artifact')
    if legacy:
        allowed |= {'schema', 'recipe_sha256', 'source_update_guard_sha256', 'tools', 'environment'}
        if any(identity.get('environment', {}).values()):
            raise RuntimeError('Legacy source update cannot infer changed compiler environment')
        if (manifest['tools']['compiler'] != identity['tools']['compiler_version'] or
                manifest['tools']['ninja'] != identity['tools']['ninja_version']):
            raise RuntimeError('Compiler/Ninja changed since the recorded build')
    changed = {key for key in previous.keys() | identity.keys()
               if previous.get(key) != identity.get(key)}
    if changed - allowed:
        raise RuntimeError('Source update changes configuration/tool/dependency inputs: ' +
                           ', '.join(sorted(changed - allowed)) + '; use fresh output')
    meson_info = json.loads((output / 'meson-info/meson-info.json').read_text())
    meson_version = subprocess.check_output([str(output / 'pyvenv/bin/meson'), '--version'], text=True).strip()
    baseline = manifest['tools'].get('meson', meson_info['meson_version']['full'])
    if meson_version != baseline:
        raise RuntimeError('Meson changed since the recorded build')
    history_path = output / 'source-update-history.json'
    history = json.loads(history_path.read_text()) if history_path.exists() else []
    history.append({'previous_requested_revision': previous['source_revision'],
                    'previous_compiled_revision': manifest['source_revision'],
                    'next_revision': identity['source_revision'],
                    'previous_manifest_sha256': digest(manifest_path),
                    'previous_library_sha256': manifest['library_sha256'],
                    'legacy_guard_migration': legacy,
                    'configuration_unchanged': True})
    # Validation finishes before any recorded inputs change. A failed configure
    # or compile remains distinguishable from the completed library manifest.
    history_path.write_text(json.dumps(history, indent=2) + '\n')
    stamp.write_text(json.dumps(identity, sort_keys=True, indent=2) + '\n')
    return stamp
