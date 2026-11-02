#!/usr/bin/env python3
"""Exercise source-only cache updates against a completed real engine record."""
import argparse
import copy
import importlib.util
import json
import pathlib
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument('build', type=pathlib.Path)
a = p.parse_args()
build = a.build.resolve()
src = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(src / 'contrib/portable'))
spec = importlib.util.spec_from_file_location('engine_cache', src / 'contrib/portable/engine-cache.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
previous = json.loads((build / 'engine-inputs.json').read_text())
manifest = json.loads((build / 'portable-manifest.json').read_text())
current = copy.deepcopy(previous)
current['source_revision'] = 'test-clean-next-revision'
if previous['recipe_sha256'] == guard.LEGACY_RECIPE and previous['schema'] == 1:
    current.update(schema=2, recipe_sha256='reviewed-new-guard-recipe',
                   source_update_guard_sha256='guard-version', environment={},
                   tools={'compiler_version': manifest['tools']['compiler'],
                          'ninja_version': manifest['tools']['ninja'], 'compiler_sha256': 'recorded-tool'})
else:
    assert previous['schema'] == 2

with tempfile.TemporaryDirectory(prefix='source-update-') as temp:
    out = pathlib.Path(temp)
    for name in ['libqemu-arm.so', 'portable-manifest.json', 'meson-info', 'pyvenv']:
        (out / name).symlink_to(build / name)
    stamp = out / 'engine-inputs.json'
    for key, value in [('abi', 'wrong-abi'), ('argv', previous['argv'] + ['--extra-cflags=-DCHANGED']),
                       ('dependency_manifest_sha256', 'changed-dependency'),
                       ('ndk_properties_sha256', 'changed-ndk')]:
        stamp.write_text(json.dumps(previous))
        before = stamp.read_bytes()
        candidate = copy.deepcopy(current)
        candidate[key] = value
        try:
            guard.check_engine_cache(out, candidate, [], True)
        except RuntimeError:
            pass
        else:
            raise AssertionError('Accepted changed configuration ' + key)
        assert stamp.read_bytes() == before
        assert not (out / 'source-update-history.json').exists()
    stamp.write_text(json.dumps(previous))
    guard.check_engine_cache(out, current, [], True)
    assert json.loads(stamp.read_text()) == current
    history = json.loads((out / 'source-update-history.json').read_text())
    assert history[0]['previous_compiled_revision'] == manifest['source_revision']
    assert history[0]['next_revision'] == current['source_revision']
    for key in ['recipe_sha256', 'tools', 'environment']:
        before = stamp.read_bytes()
        candidate = copy.deepcopy(current)
        candidate[key] = {'changed': True} if key != 'recipe_sha256' else 'unknown-recipe'
        try:
            guard.check_engine_cache(out, candidate, [], True)
        except RuntimeError:
            pass
        else:
            raise AssertionError('Accepted changed guarded input ' + key)
        assert stamp.read_bytes() == before
    candidate = copy.deepcopy(current)
    candidate['source_revision'] = 'second-source-update'
    guard.check_engine_cache(out, candidate, [], True)
    assert json.loads(stamp.read_text())['source_revision'] == 'second-source-update'
print('PASS: real engine record permits clean source-only updates; rejects ABI/macros/dependencies/NDK/tools/environment/unknown recipe changes before mutation')
