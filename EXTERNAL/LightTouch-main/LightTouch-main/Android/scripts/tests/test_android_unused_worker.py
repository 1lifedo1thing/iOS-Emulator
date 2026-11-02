import copy
import importlib.util
import pathlib
import unittest

SPEC = importlib.util.spec_from_file_location('unused_worker', pathlib.Path(__file__).resolve().parents[1] / 'android_unused_worker.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def status():
    return dict(pid=321, uid=10201, state='stopped', error='', sessionStarted=False,
                nativePollSnapshotValid=False, nativeStateDirty=False, nativeStorageFailed=False,
                touchTraceConfigured=False, graphicsProviderChoice='angle-gl', graphicsProviderFixed=True,
                observations=dict(producedFrames=0))


class UnusedWorkerTests(unittest.TestCase):
    def test_accepted_arm_before_native_runtime_does_not_require_configured_trace(self):
        receipt = dict(touchTraceArmAdmissionAccepted=True, touchTraceArmError='',
                       touchTraceConfiguredAfterArm=False, traceDeliveryVerified=False)
        self.assertTrue(MODULE.pre_native_trace_arm_admitted(receipt))
        for key, value in [('touchTraceArmAdmissionAccepted', False), ('touchTraceArmError', 'rejected'),
                           ('touchTraceConfiguredAfterArm', True), ('traceDeliveryVerified', True)]:
            with self.subTest(key=key):
                self.assertFalse(MODULE.pre_native_trace_arm_admitted(dict(receipt, **{key: value})))

    def test_missing_or_malformed_admission_never_passes(self):
        for receipt in [None, [], {}, dict(touchTraceArmAdmissionAccepted=True),
                        dict(touchTraceArmAdmissionAccepted=1, touchTraceArmError='',
                             touchTraceConfiguredAfterArm=False, traceDeliveryVerified=False)]:
            self.assertFalse(MODULE.pre_native_trace_arm_admitted(receipt))

    def test_unused_clean_status_has_no_runtime_trace_configuration(self):
        self.assertTrue(MODULE.unused_worker_matches(status(), pid=321, uid=10201, graphics_provider='angle-gl'))

    def test_dirty_native_started_or_configured_status_is_rejected(self):
        for key in ['sessionStarted', 'nativePollSnapshotValid', 'nativeStateDirty', 'nativeStorageFailed', 'touchTraceConfigured']:
            for value in [True, 0, None]:
                with self.subTest(key=key, value=value):
                    self.assertFalse(MODULE.unused_worker_matches(dict(status(), **{key: value}), pid=321, uid=10201))

    def test_provider_must_be_fixed_when_requested(self):
        for change in [dict(graphicsProviderChoice='platform'), dict(graphicsProviderFixed=False), dict(graphicsProviderFixed=1)]:
            self.assertFalse(MODULE.unused_worker_matches(dict(status(), **change), pid=321, uid=10201, graphics_provider='angle-gl'))
        self.assertTrue(MODULE.unused_worker_matches(dict(status(), graphicsProviderFixed=False), pid=321, uid=10201))

    def test_identity_frames_and_errors_fail_closed(self):
        for change in [dict(pid=322), dict(uid=10202), dict(pid=True), dict(state='running'), dict(error='fault'),
                       dict(observations={}), dict(observations=dict(producedFrames=1)), dict(observations=dict(producedFrames=False))]:
            self.assertFalse(MODULE.unused_worker_matches(dict(status(), **change), pid=321, uid=10201))

    def test_missing_keys_and_malformed_status_fail_closed(self):
        for key in status():
            missing = copy.deepcopy(status()); del missing[key]
            self.assertFalse(MODULE.unused_worker_matches(missing, pid=321, uid=10201, graphics_provider='angle-gl'))
        for malformed in [None, [], 3, {'observations': []}]:
            self.assertFalse(MODULE.unused_worker_matches(malformed, pid=321, uid=10201))

    def test_invalid_expected_identity_or_provider_is_rejected(self):
        for expected in [dict(pid=True, uid=10201), dict(pid=321, uid=0), dict(pid=321, uid=10201, graphics_provider=''),
                         dict(pid=321, uid=10201, graphics_provider=3)]:
            self.assertFalse(MODULE.unused_worker_matches(status(), **expected))


if __name__ == '__main__':
    unittest.main()
