import importlib.util
import argparse
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("monitor", Path(__file__).resolve().parents[1] / "collect-monitor-phase.py")
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)


class MonitorPhaseTest(unittest.TestCase):
    def test_plan_only_cannot_construct_target_collector(self):
        prior = {"serial": "test-serial", "package": monitor.PACKAGE, "workerStartTicks": 555,
                 "spec": {"schema": 1, "workerPid": 71, "workerUid": 10179, "shutdownGraceMs": 35000,
                          "baseKind": "generated-stock", "recordId": "a" * 32,
                          "baseSha256": "b" * 64, "bootromSha256": "c" * 64,
                          "guestPackageSha256": "d" * 64,
                          "artifactPins": {name: "e" * 64 for name in ("mainApkSha256", "jniSha256", "engineSha256")}}}
        helper = monitor.load_collector()
        with tempfile.TemporaryDirectory() as temporary:
            binding = Path(temporary) / "before.json"
            binding.write_text(json.dumps(prior))
            args = argparse.Namespace(serial="test-serial", binding=binding, phase="a1-closed",
                                      out=Path(temporary) / "no-output", adb="never-execute", execute=False)
            with patch.object(monitor, "load_collector", return_value=helper), patch.object(helper, "Collector", side_effect=AssertionError("target operation")):
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    monitor.run(args)
            self.assertEqual(json.loads(output.getvalue())["status"], "PLAN_ONLY_NO_ADB")
            self.assertFalse(args.out.exists())

    def test_stat_comm_parentheses_and_start_identity(self):
        fields = ["S"] + ["0"] * 23
        for index, value in ((11, "400"), (12, "20"), (19, "555"), (21, "123")):
            fields[index] = value
        data = ("71 (engine (worker)) " + " ".join(fields)).encode()
        self.assertEqual(monitor.worker_stat(data, 71, 555), {"userTicks": 400, "systemTicks": 20, "rssPages": 123})
        for pid, start in ((72, 555), (71, 556)):
            with self.assertRaises(ValueError):
                monitor.worker_stat(data, pid, start)
        with self.assertRaises(ValueError):
            monitor.worker_stat(b"71 (truncated) S", 71, 555)

    def test_worker_uid_and_padded_argv(self):
        status = "Pid:\t71\nTgid:\t71\nUid:\t10179 10179 10179 10179\n"
        monitor.worker_identity(status, b"org.lighttouch.android:engine    \0", 71, 10179)
        for invalid in (status.replace("10179", "10180"), status.replace("Tgid:\t71", "Tgid:\t72")):
            with self.assertRaises(ValueError):
                monitor.worker_identity(invalid, b"org.lighttouch.android:engine\0", 71, 10179)
        with self.assertRaises(ValueError):
            monitor.worker_identity(status, b"another.app:engine\0", 71, 10179)

    def test_failed_or_nonpositive_pss_is_unavailable(self):
        self.assertEqual(monitor.positive_pss(b"Pss: 12345 kB\n", 0), 12345)
        for data, rc in ((b"Pss: 0 kB\n", 0), (b"Pss: 12345 kB\n", 1), (b"Permission denied", 1), (b"", 0)):
            self.assertIsNone(monitor.positive_pss(data, rc))

    def test_ui_requires_declared_panel_worker_and_session(self):
        prefix = '<hierarchy><node package="org.lighttouch.android" resource-id="toggle_performance" text="Performance"/><node package="org.lighttouch.android" resource-id="session_status" text="running"/>'
        closed = prefix + '</hierarchy>'
        self.assertFalse(monitor.panel_observation(closed, "a1-closed", 71)["panelOpen"])
        rows = ["Emulator CPU: 142% of one core · 1s ago", "Emulator memory: 400 MiB PSS · 2s ago",
                "Android RAM: 1 / 2 MiB available/total · 2s ago", "Android whole-device CPU: unavailable",
                "iOS guest CPU / RAM: unavailable", "Worker PID: 71 · sampled every 2s while open"]
        opened = prefix + ''.join('<node package="org.lighttouch.android" text="' + row + '"/>' for row in rows) + '</hierarchy>'
        observed = monitor.panel_observation(opened, "b-open", 71)
        self.assertEqual(observed["visibleMetricAgesSeconds"], [1, 2, 2])
        for xml, phase, pid in ((opened, "a2-closed", 71), (closed, "b-open", 71),
                                (opened, "b-open", 72), (opened, "paused-open", 71)):
            with self.assertRaises(ValueError):
                monitor.panel_observation(xml, phase, pid)

    def test_counter_bounds_preserve_multicore_and_zero(self):
        def sample(user, lower, upper):
            return {"stat": {"userTicks": user, "systemTicks": 20},
                    "clockBefore": {"lowerMs": lower, "upperMs": lower + 9},
                    "clockAfter": {"lowerMs": upper - 9, "upperMs": upper}}
        first, last = sample(10, 1000, 1020), sample(4010, 21000, 21020)
        result = monitor.counter_interval(first, last, 100)
        self.assertGreater(result["workerCpuPercentOfOneCoreBounds"][0], 100)
        self.assertEqual(monitor.counter_interval(first, sample(10, 21000, 21020), 100)["workerCpuPercentOfOneCoreBounds"], [0, 0])
        self.assertNotIn("workerCpuPercentOfOneCoreBounds", monitor.counter_interval(first, last, None))
        for invalid in (sample(9, 21000, 21020), sample(20, 1000, 1020)):
            with self.assertRaises(ValueError):
                monitor.counter_interval(first, invalid, 100)

    def test_new_worker_process_time_layout_is_explicit_and_legacy_remains_supported(self):
        def xml(cpu_rows):
            rows = cpu_rows + ["Emulator memory: 400 MiB PSS · 2s ago", "Android RAM: 1 / 2 MiB available/total · 2s ago",
                "Android whole-device CPU: unavailable", "iOS guest CPU / RAM: unavailable", "Worker PID: 71 · sampled every 2s while open"]
            return '<hierarchy><node package="org.lighttouch.android" resource-id="toggle_performance" text="Performance"/><node package="org.lighttouch.android" resource-id="session_status" text="running"/>' + ''.join(
                '<node package="org.lighttouch.android" text="' + row + '"/>' for row in rows) + '</hierarchy>'
        legacy = "Emulator CPU: 120% of one core · 1s ago"
        rate = "QEMU worker CPU: 120% of one core · 1s ago"
        total = "QEMU worker CPU time: 1234.5s total · 1s ago"
        self.assertEqual(monitor.panel_observation(xml([legacy]), "b-open", 71)["layout"], "legacy-25-process-cpu")
        self.assertEqual(monitor.panel_observation(xml([rate, total]), "b-open", 71)["layout"], "qemu-worker-process-cpu-and-time")
        for mixed in ([rate], [total], [legacy, total], [legacy, rate, total], [rate, total, total]):
            with self.assertRaises(ValueError):
                monitor.panel_observation(xml(mixed), "b-open", 71)


if __name__ == "__main__":
    unittest.main()
