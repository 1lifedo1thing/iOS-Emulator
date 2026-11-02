#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pixel adapter: bounded scoped readback before the unchanged owned cleanup."""
import argparse
import hashlib
import importlib.util
import json
import multiprocessing
import os
from pathlib import Path
import signal
import time
import sys
import memory_monitor
import bind_recovery
import capture_origin
import host_lifecycle
from diagnostic_control import Capture, validate_binding

spec = importlib.util.spec_from_file_location("base_retry", Path(__file__).resolve().parents[1] / "supervise_16k_retry.py")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def capture_child(adb, binding, output, original_signal_mask=None):
    # A stalled local file or ADB read cannot keep the parent from Stop/cleanup.
    os.setsid()
    if original_signal_mask is not None:
        signal.pthread_sigmask(signal.SIG_SETMASK, original_signal_mask)
    Capture(adb, binding, output).collect()


def bounded_capture(adb, binding, output, now=time.monotonic_ns, process_factory=None):
    begin = now()
    end = min(begin + 10_000_000_000, binding["originNs"] + 810_000_000_000)
    if end - begin < 300_000_000:
        return {"captureStarted": False, "expired": True, "guestAcceptancePassed": False}
    factory = process_factory or multiprocessing.get_context("fork").Process
    original_mask = signal.pthread_sigmask(signal.SIG_BLOCK, set())
    child = factory(target=capture_child, args=(adb, binding, output, original_mask))
    identity = None
    forced = False
    deadline = end - 200_000_000  # Reserve child disposal before the original gate.
    try:
        try:
            signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGHUP, signal.SIGINT})
            child.start()
            identity = base.process_identity(child.pid)
        finally:
            # Publish the owned child identity before any pending host terminal
            # signal can unwind; restoration itself remains inside outer try.
            signal.pthread_sigmask(signal.SIG_SETMASK, original_mask)
        child.join(timeout=max(0, (deadline - now()) / 1e9))
    finally:
        # TERM/HUP/KeyboardInterrupt during join must retire this separately
        # grouped child before the outer finally retires the AVD watchdog.
        if child.pid is not None:
            forced = child.is_alive()
            current = base.process_identity(child.pid) if forced else None
            if identity is None and forced:
                # Process.is_alive() polls this owned child's waitpid handle;
                # a live, unreaped child PID cannot have been reused. This
                # fallback covers an injected start/identity Python exception.
                identity = current
            if forced and current == identity and identity is not None:
                try:
                    # The child creates its own group; never signal the parent's.
                    if os.getpgid(child.pid) == child.pid:
                        os.killpg(child.pid, signal.SIGKILL)
                    else:
                        os.kill(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            child.join(timeout=max(0, (end - now()) / 1e9))
    return {"captureStarted": True, "captureChildPid": child.pid, "forcedCaptureDisposal": forced,
            "captureChildExit": child.exitcode, "captureChildStillAlive": child.is_alive(),
            "beginNs": begin, "limitNs": end, "endedNs": now(), "guestAcceptancePassed": False}


PIXEL_MEMORY_MB = None
ATTACH_OWNER_RECEIPT = None


def pixel_options(argv):
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--pixel-memory-mb", type=int, choices=(4096, 8192))
    args, remaining = parser.parse_known_args(argv)
    if args.pixel_memory_mb is not None and any(x == "--memory-mb" or x.startswith("--memory-mb=") for x in remaining):
        raise ValueError("Use only one memory allocation flag")
    return args.pixel_memory_mb, remaining


def attach_options(argv):
    parser=argparse.ArgumentParser(add_help=False)
    parser.add_argument('--attach-avd-owner')
    args,remaining=parser.parse_known_args(argv)
    return args.attach_avd_owner,remaining


class PixelAttempt(base.Attempt):
    def __init__(self, args):
        super().__init__(args)
        ticks = memory_monitor.process_ticks(os.getpid())
        if ticks is None: raise ValueError("Original supervisor process clock unavailable")
        # Include CLI validation/import time; no later constructor clock may extend gates.
        self.start = (ticks * 1_000_000_000 // os.sysconf("SC_CLK_TCK")) / 1e9
        self.deadline = self.start + base.HARD_LIMIT
        # Persist actual identity before creating any AVD/helper process. Tool
        # handles arrive later and are attached separately without clock edits.
        self.pixel_host_signals = []
        self.pixel_readback_done = False
        self.pixel_monitor = None
        self.pixel_monitor_stop = multiprocessing.get_context("fork").Event()
        self.pixel_monitor_identity = None
        self.pixel_guest_disable = multiprocessing.get_context("fork").Event()
        self.pixel_guest_idle = multiprocessing.get_context("fork").Event()
        if PIXEL_MEMORY_MB is not None:
            args.memory_mb = PIXEL_MEMORY_MB
        self.initialize_host_lifecycle(args)

    def initialize_host_lifecycle(self, args):
        capture_origin.capture(self.output, os.getpid(), None, args.avd)
        host_lifecycle.install(self.pixel_host_signals)

    def launch(self):
        if ATTACH_OWNER_RECEIPT:
            import persistent_avd
            persistent_avd.attach_attempt(self,ATTACH_OWNER_RECEIPT)
        else:
            super().launch()
        self.memory_phase("launch")
        ticks = memory_monitor.process_ticks(self.avd.pid)
        if ticks is None:
            raise RuntimeError("Owned AVD wrapper identity unavailable for monitor")
        # Binding uses the same original process origin as capture_origin.py.
        origin = memory_monitor.process_ticks(os.getpid()) * 1_000_000_000 // os.sysconf("SC_CLK_TCK")
        deadline = min(int(self.deadline * 1e9), origin + 900_000_000_000)
        self.pixel_monitor = multiprocessing.get_context("fork").Process(target=memory_monitor.monitor,
            args=(self.args.adb, str(self.output), self.avd.pid, ticks, origin, deadline, self.pixel_monitor_stop, self.pixel_guest_disable, self.pixel_guest_idle))
        self.pixel_monitor.start()
        self.pixel_monitor_identity = base.process_identity(self.pixel_monitor.pid)
        self.write("memory-monitor.json", {"pid": self.pixel_monitor.pid, "ownerPid": self.avd.pid,
            "ownerStartTicks": ticks, "originNs": origin, "deadlineNs": deadline,
            "requestedCadenceSeconds": 2, "guestAcceptancePassed": False})

    def memory_phase(self, phase):
        try:
            (self.output / "memory-phase.txt").write_text(phase)
            with (self.output / "memory-lifecycle.jsonl").open("a") as stream:
                stream.write(json.dumps({"phase": phase, "host": memory_monitor.host_sample()}) + "\n")
        except OSError:
            pass  # Observations cannot suppress semantic Stop or owned disposal.

    def retire_monitor(self):
        self.pixel_monitor_stop.set()
        if self.pixel_monitor is None:
            return
        self.pixel_monitor.join(timeout=min(1, max(0, self.deadline - time.monotonic())))
        if self.pixel_monitor.is_alive() and self.pixel_monitor_identity is not None and base.process_identity(self.pixel_monitor.pid) == self.pixel_monitor_identity:
            try:
                if os.getpgid(self.pixel_monitor.pid) == self.pixel_monitor.pid:
                    os.killpg(self.pixel_monitor.pid, signal.SIGKILL)
                else:
                    self.pixel_monitor.kill()
            except ProcessLookupError:
                pass
        self.pixel_monitor.join(timeout=min(.2, max(0, self.deadline - time.monotonic())))
        self.write("memory-monitor-terminal.json", {"exit": self.pixel_monitor.exitcode,
            "stillAlive": self.pixel_monitor.is_alive(), "guestAcceptancePassed": False})

    def pixel_readback(self):
        if self.pixel_readback_done or self.avd is None:
            return
        self.pixel_readback_done = True
        try:
            raw = (self.output / "clock-context.json").read_bytes()
            context = json.loads(raw)
            binding_path = self.output/"pixel-bound.json"
            if not binding_path.exists():
                candidates = [self.output/name for name in ("recovery-record-bound.json", "normal-record-bound.json") if (self.output/name).exists()]
                if len(candidates) != 1: raise ValueError("No unique scoped record binding for failure capture")
                binding_path = candidates[0]
            binding = validate_binding(json.loads(binding_path.read_text()))
            if binding["originToken"] != hashlib.sha256(raw).hexdigest() or binding["originNs"] != context["originNs"]:
                raise ValueError("Bound original clock differs")
            if context["supervisorStartTicks"] * 1_000_000_000 // context["clockTicksPerSecond"] != context["originNs"]:
                raise ValueError("Original process clock was backdated/reset")
            prior_path = self.output / ("recovery-record-bound.json" if binding.get("coldRecoveryUsed") is True else "normal-record-bound.json")
            if binding.get("coldRecoveryUsed") is True or prior_path.exists():
                from bind_recovery import match_ready
                prior = json.loads(prior_path.read_text())
                if prior["originNs"] != context["originNs"]:
                    raise ValueError("Recovery binding original origin differs")
                match_ready(prior, binding)
                self.record_id = binding["recordId"]
                self.worker_pid = str(binding["workerPid"])
                self.write("recovery-stop-binding.json", {"recordId": self.record_id, "workerPid": self.worker_pid,
                    "workerStartTicks": binding["workerStartTicks"], "originalWorkerBound": True,
                    "postStartBindingAvailable": binding_path.name == "pixel-bound.json",
                    "coldRecoveryUsed": binding["coldRecoveryUsed"],
                    "cleanInitialAdmissionClaimed": False, "guestShutdownConfirmed": False})
            outcome = bounded_capture(self.args.adb, binding, self.output / "pixel-precleanup")
            self.write("pixel-precleanup-process.json", outcome)
        except Exception as error:
            try:
                self.write("pixel-readback-unavailable.json", {"error": str(error), "guestAcceptancePassed": False,
                           "cleanupDependsOnCapture": False})
            except Exception:
                pass  # Receipt failure cannot suppress owned disposal.

    def recovery_stop_allowed(self):
        paths = [self.output/name for name in ("recovery-record-bound.json", "normal-record-bound.json") if (self.output/name).exists()]
        if not paths: return True
        try:
            if len(paths) != 1: raise ValueError("Conflicting record Start mode bindings")
            prior = validate_binding(json.loads(paths[0].read_text()))
            if type(prior.get("priorDirty")) is not bool or prior.get("coldRecoveryUsed") is not prior["priorDirty"] or prior.get("pinsVerified") is not True:
                raise ValueError("No actual recovery binding")
            raw = (self.output / "clock-context.json").read_bytes()
            if prior["originToken"] != hashlib.sha256(raw).hexdigest():
                raise ValueError("Recovery Stop original clock differs")
            self.record_id = prior["recordId"]
            self.worker_pid = str(prior["workerPid"])
            observations = {}
            for name in ("stat", "status", "cmdline"):
                code, data = self.adb("exec-out", "run-as", base.PACKAGE, "cat", f"/proc/{self.worker_pid}/{name}", timeout=1)
                if code: raise ValueError("Original worker unavailable before Stop")
                observations[name] = data.decode()
            if not memory_monitor.worker_identity(observations["stat"], observations["status"], observations["cmdline"], prior):
                raise ValueError("Original recovery worker identity changed before Stop")
            for path, expected in (("/proc/sys/kernel/random/boot_id", prior["bootId"]),
                                   ("files/library/selected", prior["recordId"])):
                code, data = self.adb("exec-out", "run-as", base.PACKAGE, "cat", path, timeout=1)
                if code or data.strip().decode() != expected:
                    raise ValueError("Recovery Stop boot/selection changed")
            code, pid = self.adb("shell", "pidof", base.PACKAGE+":engine", timeout=1)
            if code or pid.strip() != self.worker_pid.encode():
                raise ValueError("Unique original recovery worker unavailable")
        except Exception as error:
            # Stop was requested; never send it to a replacement PID.
            self.stop_attempted = True
            self.write("stop-admission.json", {"error": str(error), "guestShutdownConfirmed": False,
                "originalWorkerOnly": True, "replacementWorkerStopAdmitted": False})
            if self.record_id is not None:
                self.write("record-marker-stop.json", {"recordId": self.record_id,
                    "dirtyMarkerPresent": self.path_presence(f"files/library/records/{self.record_id}/state/.dirty"),
                    "guestShutdownConfirmed": False})
            return False
        return True

    def stop(self):
        self.memory_phase("stop-requested")
        try:
            self.pixel_readback()
        finally:
            if self.recovery_stop_allowed():
                super().stop()

    def cleanup(self):
        try:
            if getattr(self, "pixel_host_signals", []):
                try:
                    self.write("host-signals.json", {"signals": self.pixel_host_signals,
                        "senderKnown": False, "guestAcceptancePassed": False})
                finally:
                    if self.avd is not None and not self.stop_attempted:
                        self.stop()
            self.pixel_readback()
        finally:
            try:
                self.memory_phase("cleanup-before-owned-disposal")
                self.pixel_guest_disable.set()
                if self.pixel_monitor is not None and not self.pixel_guest_idle.wait(
                        timeout=min(.9, max(0, self.deadline - time.monotonic()))):
                    self.retire_monitor()  # Stalled observation cannot hold disposal.
                    self.write("memory-monitor-gap.json", {"continuousHostDuringCleanup": False,
                        "reason": "Guest observation did not acknowledge bounded cleanup barrier"})
            finally:
                try:
                    if getattr(self,'attached_owner',None) is not None:
                        import persistent_avd
                        persistent_avd.cleanup_attempt(self)
                    else:
                        super().cleanup()
                finally:
                    try:
                        self.memory_phase("cleanup-after-owned-disposal")
                    finally:
                        self.retire_monitor()


if __name__ == "__main__":
    ATTACH_OWNER_RECEIPT, sys.argv[1:] = attach_options(sys.argv[1:])
    PIXEL_MEMORY_MB, sys.argv[1:] = pixel_options(sys.argv[1:])
    base.Attempt = PixelAttempt
    raise SystemExit(base.main())
