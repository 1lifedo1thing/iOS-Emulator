#!/usr/bin/env python3
"""Synthetic field/contradiction tests only; no guest or target acceptance."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools/verify-shutdown-evidence.py"
spec = importlib.util.spec_from_file_location("shutdown_evidence", TOOL)
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class ShutdownEvidence(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.lock = {"format": 1, "board": "n72ap", "build": "7E18", "boot_strategy": "iboot",
                     "guest_package": {"itpack": {"sha256": "c" * 64}}}
        self.write("device.lock.json", self.lock)
        self.expected = {"schema": 1, "clock": "android-elapsed-realtime-ms", "baseKind": "generated-stock",
                         "workerPid": 123, "workerUid": 10001, "recordId": "a" * 32,
                         "baseSha256": hashlib.sha256((self.root / "device.lock.json").read_bytes()).hexdigest(),
                         "bootromSha256": "b" * 64, "guestPackageSha256": "c" * 64,
                         "artifactPins": {"mainApkSha256": "d" * 64, "jniSha256": "e" * 64,
                                          "engineSha256": "f" * 64}, "shutdownGraceMs": 35000,
                         "acceptanceWindowStartMs": 900, "acceptanceWindowEndMs": 1100}
        self.record = {"schema": 1, "id": "a" * 32, "board": "n72ap", "build": "7E18",
                       "base_sha256": self.expected["baseSha256"], "bootrom_sha256": "b" * 64,
                       "guest_package_sha256": "c" * 64}
        self.receipt = {"reason": "native-terminal", "admissionStage": "native-terminal", "workerPid": 123,
                        "workerUid": 10001, "nativeTerminalState": "stopped", "nativePollSnapshotValid": True,
                        "guestShutdownConfirmed": True, "cleanShutdownConfirmed": True, "nativeStateDirty": False,
                        "nativeStorageFailed": False, "shutdownConfirmationInferred": False,
                        "nativeStateMarkersModified": False, "receiptPersistenceBestEffort": False,
                        "nativeEngineExit": 0, "error": "", "acceptedAtHostMs": 1000, "deadlineHostMs": 36000,
                        "observedAtHostMs": 3000}
        self.post = {"schema": 1, "clock": "android-elapsed-realtime-ms", "workerPid": 123, "workerUid": 10001,
                     "recordId": "a" * 32, "baseSha256": self.expected["baseSha256"],
                     "artifactPins": copy.deepcopy(self.expected["artifactPins"]), "observedAtHostMs": 3100,
                     "workerProbe": {"remoteExit": 0, "pids": [124]},
                     "stateDirectoryProbe": {"remoteExit": 0, "isDirectory": True},
                     "dirtyMarkerProbe": {"remoteExit": 0, "exists": False}}
        self.save()

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, name, obj):
        (self.root / name).write_text(json.dumps(obj) + "\n")

    def save(self):
        self.write("expected.json", self.expected)
        self.write("record.json", self.record)
        self.write("worker-shutdown-123.json", self.receipt)
        self.write("post-stop.json", self.post)

    def reject(self, text):
        self.save()
        with self.assertRaisesRegex(verifier.EvidenceError, text):
            verifier.check(self.root)

    def test_clean_fields_do_not_claim_scene_or_gate_acceptance(self):
        result = verifier.check(self.root)
        self.assertEqual(result["terminalAfterAcceptanceMs"], 2000)
        self.assertFalse(result["G6_G7GateClosed"])
        self.assertFalse(result["photoPersistenceChecked"])
        self.assertFalse(result["inputAuthenticityEstablished"])

    def test_marker_absence_process_exit_without_receipt_fails(self):
        (self.root / "worker-shutdown-123.json").unlink()
        with self.assertRaisesRegex(verifier.EvidenceError, "missing regular"):
            verifier.check(self.root)

    def test_each_native_clean_flag_is_required_and_strictly_typed(self):
        original = copy.deepcopy(self.receipt)
        for key in ("nativePollSnapshotValid", "guestShutdownConfirmed", "cleanShutdownConfirmed",
                    "nativeStateDirty", "nativeStorageFailed", "shutdownConfirmationInferred",
                    "nativeStateMarkersModified", "receiptPersistenceBestEffort"):
            for value in (None, int(original[key]), str(original[key]).lower(), not original[key]):
                with self.subTest(key=key, value=value):
                    self.receipt = copy.deepcopy(original)
                    self.receipt[key] = value
                    self.reject(key)

    def test_failed_or_nonzero_exit_and_boolean_exit_fail(self):
        for value in (1, True, "0", None):
            with self.subTest(value=value):
                self.receipt["nativeEngineExit"] = value
                self.reject("engine exit|nativeEngineExit")

    def test_stale_same_pid_receipt_outside_this_stop_window_fails(self):
        self.expected["acceptanceWindowStartMs"] = 4000
        self.expected["acceptanceWindowEndMs"] = 5000
        self.reject("outside this Stop")

    def test_wrong_identity_artifacts_and_lock_bytes_fail(self):
        self.record["id"] = "9" * 32
        self.reject("record identity")
        self.record["id"] = "a" * 32
        self.post["artifactPins"]["jniSha256"] = "9" * 64
        self.reject("artifact identity")
        self.post["artifactPins"] = copy.deepcopy(self.expected["artifactPins"])
        self.write("device.lock.json", {**self.lock, "extra": "changed bytes"})
        self.reject("lock bytes")

    def test_same_pid_deadline_even_empty_or_dangling_fails(self):
        p = self.root / "worker-shutdown-deadline-123.json"
        p.write_text("")
        with self.assertRaisesRegex(verifier.EvidenceError, "contradictory"):
            verifier.check(self.root)
        p.unlink()
        p.symlink_to(self.root / "missing")
        with self.assertRaisesRegex(verifier.EvidenceError, "contradictory"):
            verifier.check(self.root)

    def test_other_workers_deadline_and_fresh_idle_pid_are_separate(self):
        self.write("worker-shutdown-deadline-122.json", {"reason": "deadline-unconfirmed"})
        self.assertTrue(verifier.check(self.root)["status"].startswith("PASS"))

    def test_failed_remote_marker_probe_is_not_absence_proof(self):
        self.post["dirtyMarkerProbe"]["remoteExit"] = 1
        self.reject("failed remotely")
        self.post["dirtyMarkerProbe"]["remoteExit"] = 0
        self.post["dirtyMarkerProbe"]["exists"] = True
        self.reject("exists")

    def test_worker_present_and_probe_before_terminal_fail(self):
        self.post["workerProbe"]["pids"].append(123)
        self.reject("still present")
        self.post["workerProbe"]["pids"] = []
        self.post["observedAtHostMs"] = 2000
        self.reject("precede")

    def test_timestamps_use_checked_grace_and_terminal_before_deadline(self):
        self.receipt["deadlineHostMs"] = 35000
        self.reject("grace mismatch")
        self.receipt["deadlineHostMs"] = 36000
        self.receipt["observedAtHostMs"] = 36000
        self.reject("within shutdown")

    def test_duplicate_json_field_does_not_overwrite_failed_fact(self):
        p = self.root / "worker-shutdown-123.json"
        p.write_text(p.read_text().rstrip()[:-1] + ', "nativeStateDirty": true}\n')
        with self.assertRaisesRegex(verifier.EvidenceError, "duplicate JSON"):
            verifier.check(self.root)

    def test_cli_missing_or_truncated_input_is_json_failure_without_traceback(self):
        (self.root / "worker-shutdown-123.json").write_text("{")
        result = subprocess.run([sys.executable, "-B", str(TOOL), str(self.root)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stdout)["status"], "FAIL")
        self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
