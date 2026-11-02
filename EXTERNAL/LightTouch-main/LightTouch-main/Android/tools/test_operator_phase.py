import unittest
from operator_phase import phase_cutoff_ns
from observed_lock import WakeReadiness


class OperatorPhaseTest(unittest.TestCase):
    origin = 50_000_000_000

    def test_keyboard_unlock_remains_admissible_after_initial_home_window(self):
        began = self.origin + 220_000_000_000
        wake = WakeReadiness(began, phase_cutoff_ns(self.origin, 'keyboard'),
                             12, {'pid': 41}, [0, 0, 320, 480], 0)
        self.assertEqual(began + 15_000_000_000, wake.deadline_ns)
        with self.assertRaises(ValueError):
            WakeReadiness(began, phase_cutoff_ns(self.origin, 'awake'),
                          12, {'pid': 41}, [0, 0, 320, 480], 0)

    def test_keyboard_reserves_original_media_cleanup_time(self):
        self.assertEqual(self.origin + 585_000_000_000,
                         phase_cutoff_ns(self.origin, 'keyboard'))

    def test_initial_home_keeps_original_180_seconds(self):
        self.assertEqual(self.origin + 180_000_000_000,
                         phase_cutoff_ns(self.origin, 'awake'))

    def test_repeated_scope_lookup_cannot_renew_a_deadline(self):
        cutoff = phase_cutoff_ns(self.origin, 'keyboard')
        for _ in range(4):
            self.assertEqual(cutoff, phase_cutoff_ns(self.origin, 'keyboard'))
        with self.assertRaises(ValueError):
            WakeReadiness(cutoff, cutoff, 12, {}, [0, 0, 320, 480], 0)

    def test_other_phases_cannot_issue_lock_navigation(self):
        for phase in ('visual-wake', 'media', 'stopped', '', None):
            with self.subTest(phase=phase), self.assertRaises(ValueError):
                phase_cutoff_ns(self.origin, phase)

    def test_original_monotonic_origin_must_be_exact_positive_integer(self):
        for value in (0, -1, True, 1.2, '50'):
            with self.subTest(origin=value), self.assertRaises(ValueError):
                phase_cutoff_ns(value, 'keyboard')


if __name__ == '__main__':
    unittest.main()
