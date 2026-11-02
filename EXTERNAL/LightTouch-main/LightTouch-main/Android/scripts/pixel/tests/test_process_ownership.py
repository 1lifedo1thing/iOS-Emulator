# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise tracked supervisor ownership guards without launching Android."""
import json
import multiprocessing
from pathlib import Path
import signal
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import persistent_avd
import pixel_supervisor

base = pixel_supervisor.base


class ProcessOwnership(unittest.TestCase):
    def args(self, root):
        return SimpleNamespace(output=str(root / 'attempt'), adb='/synthetic/adb',
            emulator='/synthetic/emulator', avd='synthetic', user='synthetic',
            cores=2, memory_mb=4096, expected_page_bytes=16384, fresh_library=True)

    def test_owned_disposal_excludes_unrelated_tree_and_reused_pid(self):
        identities = {100:(1,1000), 101:(100,1010), 102:(100,7777),
                      9002:(9001,90020)}
        entries = [Path('/proc') / str(pid) for pid in identities]
        with mock.patch.object(base, 'process_identity', side_effect=identities.get), \
             mock.patch.object(base.Path, 'iterdir', return_value=entries), \
             mock.patch.object(base.os, 'kill') as kill:
            owned = {102:1020}
            base.retain_descendants(100, owned)
            base.signal_owned(owned, signal.SIGKILL)
        self.assertNotIn(9002, owned)
        self.assertEqual({call.args[0] for call in kill.call_args_list}, {100,101})

    def test_reused_wrapper_refuses_semantic_stop_and_emulator_kill(self):
        with tempfile.TemporaryDirectory() as temporary:
            attempt = base.Attempt(self.args(Path(temporary)))
            attempt.avd = mock.Mock(pid=9002)
            attempt.avd.poll.return_value = 0
            attempt.avd_start_ticks = 111
            attempt.owned_avd_identities = {9002:111, 101:1010}
            attempt.adb = mock.Mock()
            identities = {9002:(9001,90020), 101:(1,1010), 102:(9002,1020)}
            with mock.patch.object(base, 'process_identity', side_effect=identities.get), \
                 mock.patch.object(base.Path, 'iterdir', return_value=[Path('/proc') / str(p) for p in identities]), \
                 mock.patch.object(base.os, 'kill') as kill:
                attempt.stop()
                attempt.cleanup()
            attempt.adb.assert_not_called()
            self.assertEqual([call.args[0] for call in kill.call_args_list], [101])
            receipt = json.loads((attempt.output / 'stop-admission.json').read_text())
            self.assertFalse(receipt['originalWrapperIdentityMatched'])
            self.assertFalse(receipt['guestShutdownConfirmed'])

    def test_watchdog_original_ticks_prevent_adopting_reused_wrapper_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            complete = mock.Mock()
            complete.wait.return_value = False
            identities = {9002:(9001,90020), 102:(9002,1020)}
            output = Path(temporary) / 'deadline.json'
            with mock.patch.object(base, 'process_identity', side_effect=identities.get), \
                 mock.patch.object(base.Path, 'iterdir', return_value=[Path('/proc') / str(p) for p in identities]), \
                 mock.patch.object(base.os, 'kill') as kill, \
                 mock.patch.object(base.time, 'monotonic', return_value=100):
                base.watchdog(9002, 100.1, complete, output, 111)
                kill.assert_not_called()
            receipt = json.loads(output.read_text())
            self.assertEqual(receipt['ownedProcessIdentities'], {'9002':111})
            self.assertFalse(receipt['guestShutdownConfirmed'])

    def test_attached_stop_uses_verified_ticks_without_adopting_android(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            attempt = base.Attempt(self.args(root))
            (attempt.output / 'clock-context.json').write_text('{}')
            owner = {'wrapperPid':999, 'wrapperStartTicks':1234,
                     'bootId':'11111111-2222-3333-4444-555555555555'}
            with mock.patch.object(persistent_avd, 'load_owner', return_value=(owner,'a'*64)), \
                 mock.patch.object(persistent_avd, 'identity', return_value=1234), \
                 mock.patch.object(multiprocessing.get_context('fork'), 'Process') as watcher:
                watcher.return_value.pid = 777
                persistent_avd.attach_attempt(attempt, root / 'owner.json')
            self.assertEqual(attempt.avd_start_ticks, 1234)
            self.assertEqual(attempt.owned_avd_identities, {})
            xml = b'''<hierarchy><node package="org.lighttouch.android" enabled="true"
                clickable="true" bounds="[10,20][110,120]">
                <node text="Stop" enabled="true" clickable="false"/>
                </node></hierarchy>'''
            def adb(*argv, **kwargs):
                return 0, xml if argv[:2] == ('exec-out','cat') else b''
            attempt.adb = mock.Mock(side_effect=adb)
            with mock.patch.object(base, 'process_identity', return_value=(1,1234)):
                attempt.stop()
            self.assertTrue(any(call.args[:3] == ('shell','input','tap')
                                for call in attempt.adb.call_args_list))
            self.assertFalse(any(call.args[:2] == ('emu','kill')
                                 for call in attempt.adb.call_args_list))
            self.assertEqual(attempt.owned_avd_identities, {})


if __name__ == '__main__':
    unittest.main()
