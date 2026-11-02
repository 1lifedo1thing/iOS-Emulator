"""Check fixed engine counter decoding, exact uint64 deltas, and production snapshots."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest


class FramePipelineEvidenceTest(unittest.TestCase):
    def test_fixed_schema_exact_deltas_and_actual_getters(self):
        android = Path(__file__).resolve().parents[2]
        helper = android / "app/src/androidTest/java/org/lighttouch/android/FramePipelineEvidence.java"
        contract = Path(__file__).with_name("FramePipelineEvidenceContractTest.java")
        fixture_root = os.environ.get("LIGHTTOUCH_ENGINE_COUNTER_FIXTURES")
        fixtures = []
        if fixture_root:
            fixtures = [str(Path(fixture_root) / name) for name in ("scan-counters.json", "portable-counters.json")]
        with tempfile.TemporaryDirectory() as output:
            subprocess.run(["javac", "-J-Xmx96m", "-d", output, str(helper), str(contract)],
                           check=True, capture_output=True, text=True, timeout=20)
            result = subprocess.run(["java", "-Xmx64m", "-cp", output,
                                     "org.lighttouch.android.FramePipelineEvidenceContractTest", *fixtures],
                                    check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("FramePipelineEvidence contract PASS", result.stdout)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
