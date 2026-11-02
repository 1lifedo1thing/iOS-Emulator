#!/usr/bin/env python3
"""Offline synthetic collector tests: no ADB invocation or device access."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOL = Path(__file__).resolve().parents[1] / "tools/collect-shutdown-evidence.py"
spec = importlib.util.spec_from_file_location("shutdown_collector", TOOL)
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


class CollectorContract(unittest.TestCase):
    def test_centisecond_uptime_brackets_android_milliseconds(self):
        result = collector.uptime_interval(b"123456.78 900000.50\n")
        self.assertEqual(result["lowerMs"], 123456780)
        self.assertEqual(result["upperMs"], 123456789)

    def test_high_precision_uptime_avoids_float_rounding(self):
        result = collector.uptime_interval(b"123456789.999999999 0.00\n")
        self.assertEqual(result["lowerMs"], 123456789999)
        self.assertEqual(result["upperMs"], 123456789999)

    def test_invalid_uptime_is_not_a_host_clock_fallback(self):
        for data in (b"-1.00 0.00", b"nan 0.00", b"123 0.00"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                collector.uptime_interval(data)

    def test_remote_exit_sentinal_preserves_exact_metadata_bytes(self):
        source = b'{"a":1}\n'
        data, exit_code = collector.remote_result(source + b"\nTOKEN=1\n", "TOKEN")
        self.assertEqual(data, source)
        self.assertEqual(exit_code, 1)
        with self.assertRaises(ValueError):
            collector.remote_result(source, "TOKEN")

    def test_local_adb_zero_does_not_mask_remote_error(self):
        with tempfile.TemporaryDirectory() as directory:
            c = collector.Collector(Path(directory), "synthetic", "example.app")
            def run(argv, **kwargs):
                marker = argv[-1].split("\\n__LT_READ_EXIT_", 1)[1].split("=%s", 1)[0]
                return subprocess.CompletedProcess(argv, 0, b"missing\n\n__LT_READ_EXIT_" + marker.encode() + b"=1\n", b"")
            with patch.object(collector.subprocess, "run", side_effect=run), self.assertRaisesRegex(ValueError, "exit=1"):
                c.read("read", "head -c 262145 files/missing")
            receipt = json.loads((Path(directory) / "read.json").read_text())
            self.assertEqual(receipt["adbExit"], 0)
            self.assertEqual(receipt["remoteExit"], 1)

    def test_reusing_output_does_not_write_failure_into_existing_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, "-B", str(TOOL), "before", "--serial", "synthetic",
                                     "--out", directory, "--expected", "missing.json"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_changed_boot_prevents_joining_receipt_clock(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            expected = {"schema": 1, "workerPid": 123, "workerUid": 10001, "shutdownGraceMs": 35000,
                        "baseKind": "generated-stock", "recordId": "a" * 32, "baseSha256": "b" * 64,
                        "bootromSha256": "c" * 64, "guestPackageSha256": "d" * 64,
                        "artifactPins": {"mainApkSha256": "e" * 64, "jniSha256": "f" * 64,
                                         "engineSha256": "0" * 64}}
            (p / "before.json").write_text(json.dumps({"spec": expected, "serial": "synthetic",
                                                       "package": "example.app", "bootId": "old"}))
            c = collector.Collector(p, "synthetic", "example.app")
            with patch.object(c, "boot", return_value="new"), self.assertRaisesRegex(ValueError, "boot changed"):
                collector.after(c, p)


if __name__ == "__main__":
    unittest.main()
