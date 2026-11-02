"""Bounded host JVM tests execute the actual Android-test transition policy."""
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
POLICY = HERE.parents[1] / 'app/src/androidTest/java/org/lighttouch/android/GuestPowerTransition.java'


class PowerTransitionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        subprocess.run(['javac', '-J-Xmx128m', '-d', cls.temp.name, str(POLICY),
                        str(HERE / 'power_transition.java')], check=True, timeout=20)

    def test_native_transition_sequences_and_deadlines(self):
        for scenario in ['shared-control-budget', 'actual-black-wake', 'delayed-wake', 'last-in-budget',
                         'deadline-no-renewal', 'lookup-consumes-budget', 'stale-frame',
                         'no-pixels', 'no-backlight', 'sleep-fade', 'frame-regression',
                         'clock-regression', 'identity-and-health', 'missing-pixels']:
            with self.subTest(scenario=scenario):
                result = subprocess.run(['java', '-Xmx64m', '-cp', self.temp.name,
                    'org.lighttouch.android.PowerTransitionHostTest', scenario],
                    check=True, text=True, capture_output=True, timeout=5)
                self.assertEqual(result.stdout.strip(), 'PASS ' + scenario)

    def test_shared_deadline_reaches_the_existing_action_guard(self):
        source = (POLICY.parent / 'GuestOrientationLifecycleTest.kt').read_text()
        self.assertIn('fun clickControl(text: String, sharedDeadline: Long = Long.MAX_VALUE)', source)
        self.assertIn('GuestPowerTransition.controlDeadline(SystemClock.elapsedRealtime(), sharedDeadline)', source)
        self.assertIn('clickControl("Power", gate.deadline)', source)
        self.assertIn('SystemClock.elapsedRealtime() < deadline && sameWindow &&', source)


if __name__ == '__main__':
    unittest.main()
