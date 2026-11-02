"""Execute the instrumentation exception-preservation helper on the host JVM."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class FailureEvidenceTest(unittest.TestCase):
    def test_original_failure_survives_all_diagnostic_outcomes(self):
        android = Path(__file__).resolve().parents[2]
        helper = android / "app/src/androidTest/java/org/lighttouch/android/FailureEvidence.java"
        contract = Path(__file__).with_name("FailureEvidenceContractTest.java")
        with tempfile.TemporaryDirectory() as output:
            subprocess.run(["javac", "-J-Xmx96m", "-d", output, str(helper), str(contract)],
                           check=True, capture_output=True, text=True, timeout=20)
            result = subprocess.run(["java", "-Xmx64m", "-cp", output,
                                     "org.lighttouch.android.FailureEvidenceContractTest"],
                                    check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("FailureEvidence contract PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
