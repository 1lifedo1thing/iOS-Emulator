import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("guest_profile", Path(__file__).resolve().parents[1] / "profile-existing-guest.py")
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


class ProfileReportTest(unittest.TestCase):
    def test_successful_empty_record_has_no_attribution(self):
        # A successful record/report exit alone was insufficient on the device.
        self.assertEqual(profile.report_counts("Event: task-clock:u\nSamples: 0\nEvent count: 0\n"),
                         {"samples": 0, "reported_sample_event_count": 0, "nonzero_sample_data": False})

    def test_both_samples_and_periods_must_be_nonzero(self):
        self.assertTrue(profile.report_counts("Samples: 17\nEvent count: 1,234\n")["nonzero_sample_data"])
        self.assertFalse(profile.report_counts("Samples: 17\nEvent count: 0\n")["nonzero_sample_data"])

    def test_missing_or_ambiguous_event_counts_are_not_a_profile(self):
        for text in ["", "Samples: 1\n", "Samples: 1\nEvent count: 9\nSamples: 2\nEvent count: 8\n"]:
            with self.assertRaises(ValueError):
                profile.report_counts(text)

    def test_thread_scope_uses_only_explicit_thread_selector(self):
        options = profile.record_options({31, 7}, 25, "/owned/perf.data", "cpu-cycles:u", threads=True)
        self.assertEqual(options[options.index("-t") + 1], "7,31")
        self.assertNotIn("-p", options)
        self.assertNotIn("-a", options)
        self.assertIn("--no-inherit", options)

    def test_thread_owner_and_tid_reuse_are_not_equivalent(self):
        status = "Name:\tCPU 0/TCG\nTgid:\t12862\nPid:\t12890\nUid:\t10179\t10179\t10179\t10179\n"
        # /proc stat field22=starttime; comm contains spaces and parentheses.
        fields = ["S"] + ["0"] * 18 + ["5409547"] + ["0"] * 3
        stat = "12890 (CPU 0/TCG (guest)) " + " ".join(fields)
        actual = profile.thread_identity(status, stat, "CPU 0/TCG\n", 12862, 12890, 10179)
        self.assertEqual(actual["comm"], "CPU 0/TCG")
        self.assertEqual(actual["start_ticks"], 5409547)
        reused = profile.thread_identity(status, stat.replace("5409547", "5409600"), "CPU 0/TCG\n", 12862, 12890, 10179)
        self.assertNotEqual(actual, reused)
        for invalid in [status.replace("12862", "12863"), status.replace("10179", "10180"), status.replace("12890", "12891")]:
            with self.assertRaises(ValueError):
                profile.thread_identity(invalid, stat, "CPU 0/TCG\n", 12862, 12890, 10179)


if __name__ == "__main__":
    unittest.main()
