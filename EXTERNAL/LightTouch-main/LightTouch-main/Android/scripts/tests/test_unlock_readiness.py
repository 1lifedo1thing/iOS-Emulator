"""Small host JVM checks of the actual Android-test readiness admission class."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import re

HERE = Path(__file__).resolve().parent
POLICY = HERE.parents[1] / 'app/src/androidTest/java/org/lighttouch/android/GuestUnlockReadiness.java'


class UnlockReadinessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        subprocess.run(['javac', '-J-Xmx128m', '-d', cls.temp.name, str(POLICY),
                        str(HERE / 'unlock_readiness.java')], check=True, timeout=20)

    def test_actual_admission_and_continuation_contract(self):
        for scenario in ['poll-window', 'sparse', 'banner-reflow', 'sleeping-ui', 'unusable',
                         'identity', 'input-generation', 'surface-generation', 'width',
                         'height', 'invalid', 'deadline', 'clock']:
            with self.subTest(scenario=scenario):
                completed = subprocess.run(['java', '-Xmx64m', '-cp', self.temp.name,
                    'org.lighttouch.android.UnlockReadinessHostTest', scenario],
                    check=True, text=True, capture_output=True, timeout=5)
                self.assertEqual(completed.stdout.strip(), 'PASS ' + scenario)


class UnusedObserverContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (POLICY.parent / 'GuestSessionSetupTest.kt').read_text()
        cls.observer = cls.source.split('@Test fun observeExistingUnusedWorkerWithoutArming() {', 1)[1].split(
            '@Test fun armExistingUnusedWorkerBeforeOperatorStart()', 1)[0]
        cls.binding = cls.source.split('private fun withExistingWorker(', 1)[1].split(
            '@Test fun createUnusedForegroundServiceForLaterArming()', 1)[0]

    def test_observer_only_worker_calls_are_two_status_reads(self):
        self.assertEqual(re.findall(r'worker\.([A-Za-z0-9_]+)\(', self.observer), ['status', 'status'])
        for forbidden in ['ActivityScenario', 'startForegroundService', 'startActivity', 'debugEnableTouchTrace',
                          'debugArmQmp', 'debugSetGraphicsProvider', 'nativeStateMarkersModified', 'BIND_AUTO_CREATE']:
            self.assertNotIn(forbidden, self.observer)
        self.assertEqual(self.observer.count('withExistingWorker(context)'), 1)
        self.assertIn('connection, 0)', self.binding)
        self.assertNotIn('context.start', self.binding)

    def test_required_identity_arguments_and_process_lifetime_guards_remain(self):
        for argument in ['observeGuestSession', 'expectedMainApkSha256', 'expectedWorkerPid',
                         'expectedWorkerStartTicks', 'expectedBootId']:
            self.assertIn('getString("' + argument + '")', self.observer)
        for guard in ['fields[19]', 'listOf("Z", "X")', 'Process.myUid()', '/proc/sys/kernel/random/boot_id']:
            self.assertIn(guard, self.observer)
        self.assertGreaterEqual(self.observer.count('processIdentity()'), 6)
        self.assertEqual(self.observer.count('acceptanceSha256(File(context.applicationInfo.sourceDir))'), 2)

    def test_both_full_status_receipts_are_validated_without_arming(self):
        self.assertIn('receipt.put("before", encode(before))', self.observer)
        self.assertIn('receipt.put("after", encode(after))', self.observer)
        self.assertIn('processIdentity(); unused(before)', self.observer)
        self.assertIn('processIdentity(); unused(after)', self.observer)
        self.assertIn('.put("inputArmingCalls", 0).put("ordinaryForegroundCalls", 0)', self.observer)
        self.assertIn('.put("guestStartedByTest", false).put("workerRestartedByTest", false)', self.observer)
        self.assertIn('finally { File(evidence, "result.json").writeText(receipt.toString(2)) }', self.observer)

    def test_unused_status_requires_explicit_clean_fixed_angle_and_pre_native_trace(self):
        for key in ['sessionStarted', 'nativePollSnapshotValid', 'nativeStateDirty', 'nativeStorageFailed',
                    'graphicsProviderFixed', 'touchTraceConfigured', 'observations']:
            self.assertIn('"' + key + '"', self.observer)
        self.assertIn('state.keySet().containsAll(required)', self.observer)
        self.assertIn('observations.containsKey("producedFrames")', self.observer)
        self.assertIn('assertEquals(0L, observations.getLong("producedFrames"))', self.observer)
        self.assertIn('assertEquals("angle-gl", state.getString("graphicsProviderChoice"))', self.observer)
        self.assertIn('state.getBoolean("touchTraceConfigured"))', self.observer)
        self.assertNotIn('assertTrue(state.getBoolean("touchTraceConfigured"))', self.observer)


if __name__ == '__main__':
    unittest.main()
