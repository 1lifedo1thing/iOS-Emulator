"""Exercise bounded timeline parsing, history continuity, and actual blocked-I/O expiry."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class FrameTimelineEvidenceTest(unittest.TestCase):
    def test_bounded_readonly_timeline_contract(self):
        android = Path(__file__).resolve().parents[2]
        helper = android / "app/src/androidTest/java/org/lighttouch/android/FrameTimelineEvidence.java"
        contract = Path(__file__).with_name("FrameTimelineEvidenceContractTest.java")
        with tempfile.TemporaryDirectory() as output:
            subprocess.run(["javac", "-J-Xmx96m", "-d", output, str(helper), str(contract)],
                           check=True, capture_output=True, text=True, timeout=20)
            result = subprocess.run(["java", "-Xmx64m", "-cp", output,
                                     "org.lighttouch.android.FrameTimelineEvidenceContractTest"],
                                    check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("FrameTimelineEvidence contract PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
