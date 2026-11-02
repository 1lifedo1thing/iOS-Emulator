import argparse
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('keyboard', Path(__file__).resolve().parents[1] / 'drive-media-keyboard.py')
k = importlib.util.module_from_spec(spec)
spec.loader.exec_module(k)


def binding():
    prior = {'package': k.PACKAGE, 'serial': 'serial', 'workerStartTicks': 123,
             'spec': {'workerPid': 71, 'workerUid': 10179, 'recordId': 'a'*32,
                      'artifactPins': {'mainApkSha256': 'b'*64}}}
    b = {'schema': 1, 'serial': 'serial', 'scratchNoteApproved': True, 'preservesExistingTestNote': True,
         'durationSeconds': 30, 'mediaFolder': 'media-evidence-1792015989831',
         'measurementStartDeviceBootTimeMs': 100000, 'mainApkSha256': 'b'*64, 'testApkSha256': 'c'*64,
         'rawGuestImageSha256': 'd'*64, 'androidImageSha256': 'e'*64, 'recordId': 'a'*32,
         'scratchNoteIdentity': 'new-media-scratch', 'androidDimensions': [100, 200],
         'guestViewportBounds': [5, 20, 95, 180], 'letterKeyRegion': [10, 100, 20, 110],
         'backspaceKeyRegion': [80, 100, 90, 110], 'stableKeyboardRegions': [[10, 120, 30, 125], [50, 120, 70, 125]],
         'surfaceGeneration': 8, 'inputCancellationRequests': 3}
    return b, prior


def sample(at=101000):
    return {'label': 'running', 'hostElapsedMs': at, 'surfaceValid': True, 'viewFocused': True,
            'status': {'pid': 71, 'uid': 10179, 'state': 'running', 'error': '', 'observations': {'surfaceGeneration': 8}},
            'androidInput': {'cancellationRequests': 3, 'rejectedBatches': 0, 'androidContactIds': []}}


def clock(at):
    return {'lowerMs': at, 'upperMs': at+9}


class KeyboardFenceTest(unittest.TestCase):
    def test_actual_own_tap_read_fence_defers_historical_held_sample(self):
        # Poco03: the read ended at92166603.385411ms while the completed
        # Android tap's immediate uptime query bracket was92166670..679ms.
        b, prior = binding(); b['measurementStartDeviceBootTimeMs'] = 92166000
        fence = k.Fence(b, prior)
        fence.owned_tap_completed('letterKeyRegion', clock(92166670))
        gate = k.PostTapSampleFence(fence.last_owned_tap, clock(92166700), fence.cutoff)
        old = sample(92166603)
        old.update(readBeginElapsedNs=92166601863640, readEndElapsedNs=92166603385411)
        old['androidInput']['androidContactIds'] = [0]
        self.assertFalse(gate.ready(old, clock(92166700)))
        fresh = sample(92167603)
        fresh.update(readBeginElapsedNs=92167601863640, readEndElapsedNs=92167603385411)
        self.assertTrue(gate.ready(fresh, clock(92167700)))
        fence.sample(fresh, clock(92167700))
        fresh['androidInput']['androidContactIds'] = [0]
        self.assertTrue(gate.ready(fresh, clock(92167700)))
        with self.assertRaisesRegex(ValueError, 'held external input'):
            fence.sample(fresh, clock(92167700))

    def test_owned_sample_wait_deadline_never_renews_or_extends_cutoff(self):
        owned = {'key': 'letterKeyRegion', 'commandReturnedDeviceBootTimeUpperMs': 101009}
        gate = k.PostTapSampleFence(owned, clock(101100), 127000)
        stale = sample(); stale.update(readBeginElapsedNs=101000000000, readEndElapsedNs=101001000000)
        self.assertFalse(gate.ready(stale, clock(102000)))
        fresh = sample(103200); fresh.update(readBeginElapsedNs=103200000000, readEndElapsedNs=103201000000)
        with self.assertRaisesRegex(ValueError, 'expired'):
            gate.ready(fresh, clock(103101))
        self.assertEqual(gate.deadline_ms, 103109)
        near_end = k.PostTapSampleFence(owned, clock(126400), 127000)
        self.assertEqual(near_end.deadline_ms, 126500)
        with self.assertRaisesRegex(ValueError, 'cutoff'):
            near_end.ready(fresh, clock(126500))
        with self.assertRaisesRegex(ValueError, 'phase'):
            gate.ready({**stale, 'label': 'measurement-end'}, clock(101200))
        with self.assertRaisesRegex(ValueError, 'timestamps'):
            gate.ready(sample(), clock(101200))
        with self.assertRaisesRegex(ValueError, 'future'):
            gate.ready(fresh, clock(101200))

    def test_actual_sdk36_top_display_focus_and_inactive_null(self):
        # Actual Poco SDK36 excerpts: `window windows` had no focus field;
        # full output carries an inactive display7 and the active display0.
        actual = (b'  Display: mDisplayId=7\n  mCurrentFocus=null\n'
                  b'  Display: mDisplayId=0 (organized)\n'
                  b'  mCurrentFocus=Window{774ee2f u0 org.lighttouch.android/org.lighttouch.android.MainActivity}\n'
                  b'  mTopFocusedDisplayId=0\n')
        self.assertEqual(k.focused_own_window(actual)['topFocusedDisplayId'], 0)
        for bad in (b'', actual.replace(b'mTopFocusedDisplayId=0', b'mTopFocusedDisplayId=7'),
                    actual.replace(b'org.lighttouch.android/org.lighttouch.android.MainActivity',
                                   b'com.android.launcher3/com.android.launcher3.uioverrides.QuickstepLauncher'),
                    actual + b'  mTopFocusedDisplayId=0\n',
                    actual + b'  mCurrentFocus=null\n', b'x' * 131073):
            with self.assertRaises(ValueError): k.focused_own_window(bad)

    def test_initial_identity_and_record_metadata_keep_distinct_receipts(self):
        # Exercise the real metadata reader with exclusive-write receipts,
        # including the selected-record read shared by both collection paths.
        helper = k.load_monitor().load_collector()
        lock = b'actual-small-base-lock'
        record_id = 'a' * 32
        pin = k.sha(lock)
        expected = {'recordId': record_id, 'baseSha256': pin,
                    'bootromSha256': 'b' * 64, 'guestPackageSha256': 'c' * 64}
        record = {'id': record_id, 'base_sha256': pin,
                  'bootrom_sha256': 'b' * 64, 'guest_package_sha256': 'c' * 64}
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            class Collector:
                def file(self, label, path):
                    values = {'files/library/selected': record_id.encode(),
                              'files/library/records/' + record_id + '/record.json': json.dumps(record).encode(),
                              'files/library/records/' + record_id + '/base/device.lock.json': lock,
                              'files/library/records/' + record_id + '/state/.base-identity': pin.encode()}
                    value = values[path]
                    with (root / (label + '.json')).open('x') as receipt:
                        json.dump({'path': path, 'sha256': k.sha(value)}, receipt)
                    return value
            c = Collector()
            def identity(prefix):
                self.assertEqual(c.file(prefix + '-selected', 'files/library/selected'), record_id.encode())
            k.collect_initial_evidence(identity, helper, c, expected)
            selected = list(root.glob('*-selected.json'))
            self.assertEqual(len(selected), 2)
            self.assertEqual({json.loads(p.read_text())['path'] for p in selected}, {'files/library/selected'})

    def test_actual_remote_shell_guards_precede_input(self):
        # Execute the production shell string with only its two Android
        # endpoints replaced by local fixtures; no ADB/phone is reachable.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            endpoint = root/'run-as'
            endpoint.write_text('#!/bin/sh\ncase "$2" in cat) cat "$STAT_FIXTURE";; tail) cat "$PHASE_FIXTURE";; *) exit 99;; esac\n')
            endpoint.chmod(0o700)
            sink = root/'input'
            sink.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$INPUT_LOG"\n')
            sink.chmod(0o700)
            fields = ['S'] + ['0']*23; fields[19] = '555'
            (root/'stat').write_text('71 (engine (thread)) ' + ' '.join(fields))
            (root/'phase').write_text('{"label":"running"}\n')
            env = {**os.environ, 'PATH': str(root) + ':/usr/bin:/bin', 'STAT_FIXTURE': str(root/'stat'),
                   'PHASE_FIXTURE': str(root/'phase'), 'INPUT_LOG': str(root/'events')}
            def execute(start=555, cutoff=10**15):
                return subprocess.run(['/bin/sh', '-c', k.tap_script(71, start, cutoff, 10, 20, 'media-evidence-1792015989831')],
                                      env=env, capture_output=True, timeout=2).returncode
            self.assertEqual(execute(start=556), 21)
            self.assertFalse((root/'events').exists())
            self.assertEqual(execute(cutoff=500), 22)
            self.assertFalse((root/'events').exists())
            (root/'phase').write_text('{"label":"measurement-end"}\n')
            self.assertEqual(execute(), 24)
            self.assertFalse((root/'events').exists())
            (root/'phase').write_text('{"label":"running"}\n')
            self.assertEqual(execute(), 0)
            self.assertEqual((root/'events').read_text(), 'touchscreen tap 10 20\n')

    def test_quiet_cutoff_and_no_late_pair(self):
        b, p = binding(); f = k.Fence(b, p)
        self.assertEqual(f.cutoff, 127000)
        f.start_pair(clock(121990)); f.finish_pair(clock(125900))
        self.assertEqual(f.pairs, 1)
        with self.assertRaises(ValueError): k.Fence(b, p).start_pair(clock(121991))
        with self.assertRaises(ValueError): f.finish_pair(clock(126995))

    def test_device_clock_only_cadence_and_no_catchup(self):
        b, p = binding(); f = k.Fence(b, p)
        f.start_pair(clock(101000)); f.finish_pair(clock(101500))
        for at in (102999, 106501):
            with self.assertRaises(ValueError): f.start_pair(clock(at))
        f.start_pair(clock(106500)); self.assertEqual(f.last_pair_ms, 106500)

    def test_phase_and_identity_surface_ownership_changes_abort(self):
        b, p = binding()
        edits = [('label', 'measurement-end'), ('surfaceValid', False), ('viewFocused', False)]
        for key, value in edits:
            s = sample(); s[key] = value
            with self.assertRaises(ValueError): k.Fence(b, p).sample(s, clock(101100))
        for key, value in [('pid', 72), ('uid', 10180), ('state', 'paused'), ('error', 'actual failure')]:
            s = sample(); s['status'][key] = value
            with self.assertRaises(ValueError): k.Fence(b, p).sample(s, clock(101100))
        s = sample(); s['status']['observations']['surfaceGeneration'] = 9
        with self.assertRaises(ValueError): k.Fence(b, p).sample(s, clock(101100))
        for key, value in [('cancellationRequests', 4), ('rejectedBatches', 1), ('androidContactIds', [7])]:
            s = sample(); s['androidInput'][key] = value
            with self.assertRaises(ValueError): k.Fence(b, p).sample(s, clock(101100))

    def test_stale_future_old_and_regressing_trace_rejected(self):
        b, p = binding(); f = k.Fence(b, p)
        f.sample(sample(), clock(101100))
        for at, now in [(99999, 100000), (100999, 101100), (104000, 101100), (101000, 104000)]:
            with self.assertRaises(ValueError): f.sample(sample(at), clock(now))

    def test_proc_uptime_print_precision_is_a_bracket(self):
        b, p = binding()
        k.Fence(b, p).sample(sample(101005), clock(101000))
        with self.assertRaises(ValueError):
            k.Fence(b, p).sample(sample(101010), clock(101000))

    def test_bounded_trace_tail_requires_complete_last_record(self):
        data = b'partial old record\n' + json.dumps(sample()).encode() + b'\n'
        self.assertEqual(k.latest_sample(data)['status']['pid'], 71)
        for invalid in (data[:-1], b'', b'[]\n', b'{broken}\n', b'x'*65537+b'\n'):
            with self.assertRaises((ValueError, json.JSONDecodeError)): k.latest_sample(invalid)

    def test_only_fresh_scratch_binding_with_separate_keys(self):
        b, p = binding(); k.binding_check(b, p)
        for key, value in [('scratchNoteApproved', False), ('preservesExistingTestNote', False),
                           ('durationSeconds', 1800), ('mainApkSha256', 'f'*64), ('recordId', 'f'*32),
                           ('mediaFolder', 'latest'), ('letterKeyRegion', [80, 100, 90, 110]),
                           ('guestViewportBounds', [0, 0, 5, 10]), ('surfaceGeneration', 0)]:
            changed = copy.deepcopy(b); changed[key] = value
            with self.assertRaises(ValueError): k.binding_check(changed, p)

    def test_pair_count_bound_cannot_be_extended_by_observer(self):
        b, p = binding(); f = k.Fence(b, p)
        f.pairs = 15
        with self.assertRaises(ValueError): f.start_pair(clock(101000))
        b['durationSeconds'] = 1200
        self.assertEqual(k.Fence(b, p).maximum_pairs, 600)
        self.assertEqual(k.Fence(b, p).cutoff, 1297000)

    def test_actual_png_roi_detects_layout_and_pixel_change(self):
        from PIL import Image
        b, _ = binding()
        def png(image):
            out = io.BytesIO(); image.save(out, format='PNG'); return out.getvalue()
        image = Image.new('RGB', (100, 200), (20, 30, 40))
        before = k.image_regions(png(image), b)
        image.putpixel((10, 120), (1, 2, 3))
        self.assertNotEqual(before, k.image_regions(png(image), b))
        with self.assertRaises(ValueError): k.image_regions(png(Image.new('RGB', (101, 200))), b)

    def test_plan_only_never_constructs_target_collector(self):
        b, p = binding(); android, raw = b'approved-android', b'approved-raw'
        b['androidImageSha256'] = k.sha(android); b['rawGuestImageSha256'] = k.sha(raw)
        helper = type('Helper', (), {'validate_spec': lambda self, x: None, 'Collector': lambda *args, **kwargs: self.fail('target access')})()
        monitor = type('Monitor', (), {'load_collector': lambda self: helper})()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, data in [('keyboard.json', json.dumps(b).encode()), ('before.json', json.dumps(p).encode()), ('android.png', android), ('raw.png', raw)]:
                (root/name).write_bytes(data)
            args = argparse.Namespace(keyboard=root/'keyboard.json', binding=root/'before.json', android_image=root/'android.png', raw_guest_image=root/'raw.png', serial='serial', execute=False, out=root/'never-created')
            with patch.object(k, 'load_monitor', return_value=monitor), patch.object(k, 'image_regions', return_value=['pin1', 'pin2']), contextlib.redirect_stdout(io.StringIO()) as output:
                k.run(args)
            self.assertEqual(json.loads(output.getvalue())['status'], 'PLAN_ONLY_NO_ADB')
            self.assertFalse(args.out.exists())


if __name__ == '__main__':
    unittest.main()
