#!/usr/bin/env python3
"""Root-operated observation only: never launch, toggle, pause, or stop the app."""
import argparse
import hashlib
import importlib.util
import json
import re
import shlex
import statistics
import time
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path

PACKAGE = "org.lighttouch.android"
PHASES = ("a1-closed", "b-open", "a2-closed", "paused-open")
COLLECTOR_PATH = Path(__file__).resolve().parents[2] / "Portable/firmware/tools/collect-shutdown-evidence.py"


def require(ok, message):
    if not ok:
        raise ValueError(message)


def worker_stat(data, pid, start):
    head, separator, tail = data.rpartition(b")")
    fields = tail.split()
    require(separator and head.split(b" ", 1)[0] == str(pid).encode() and len(fields) > 21,
            "invalid worker stat/PID")
    require(int(fields[19]) == start, "worker start token changed")
    user, system, rss = (int(fields[i]) for i in (11, 12, 21))
    require(min(user, system, rss) >= 0, "invalid worker counters")
    return {"userTicks": user, "systemTicks": system, "rssPages": rss}


def worker_identity(status, cmdline, pid, uid):
    for field in ("Pid", "Tgid"):
        require(re.search(rf"(?m)^{field}:\s+{pid}\s*$", status), "worker " + field + " mismatch")
    actual = re.search(r"(?m)^Uid:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$", status)
    require(actual and all(int(value) == uid for value in actual.groups()), "worker UID mismatch")
    require(cmdline.split(b"\0", 1)[0].rstrip(b" ") == (PACKAGE + ":engine").encode(),
            "not production engine worker")


def positive_pss(data, exit_code):
    match = re.search(rb"(?m)^Pss:\s+(\d+)\s+kB\s*$", data) if exit_code == 0 else None
    return int(match[1]) if match and int(match[1]) > 0 else None


def panel_observation(xml, phase, pid):
    nodes = [n for n in ET.fromstring(xml).iter("node") if n.get("package") == PACKAGE]
    require(nodes, "own app is not visible; foreground it manually")
    tagged = {n.get("resource-id", "").split("/")[-1]: n for n in nodes if n.get("resource-id")}
    require("toggle_performance" in tagged, "Performance control not visible")
    texts = [n.get("text", "") for n in nodes]
    opened = "iOS guest CPU / RAM: unavailable" in texts
    require(opened == (phase in ("b-open", "paused-open")), "panel state differs from declared phase")
    state = tagged.get("session_status")
    require(state is not None and state.get("text") == ("paused" if phase == "paused-open" else "running"),
            "session state differs from declared phase")
    rows = [text for text in texts if text.startswith(("Emulator CPU:", "QEMU worker CPU:", "QEMU worker CPU time:", "Emulator memory:", "Android RAM:",
            "Android whole-device CPU:", "iOS guest CPU / RAM:", "Worker PID:"))]
    layout = "closed"
    if opened:
        prefixes = [row.split(":", 1)[0] + ":" for row in rows]
        common = {"Emulator memory:", "Android RAM:", "Android whole-device CPU:", "iOS guest CPU / RAM:", "Worker PID:"}
        if set(prefixes) == common | {"Emulator CPU:"} and len(rows) == 6:
            layout = "legacy-25-process-cpu"
        elif set(prefixes) == common | {"QEMU worker CPU:", "QEMU worker CPU time:"} and len(rows) == 7:
            layout = "qemu-worker-process-cpu-and-time"
        else:
            raise ValueError("mixed, incomplete or unknown monitor labels")
        require(any(re.match(rf"Worker PID: {pid}\b", row) for row in rows),
                "monitor worker identity/rows unavailable")
    return {"panelOpen": opened, "sessionState": state.get("text"), "rows": rows, "layout": layout,
            "visibleMetricAgesSeconds": [int(age) for row in rows for age in re.findall(r"· (\d+)s ago", row)],
            "atomicNativeSnapshot": False}


def counter_interval(first, last, hz):
    user = last["stat"]["userTicks"] - first["stat"]["userTicks"]
    system = last["stat"]["systemTicks"] - first["stat"]["systemTicks"]
    require(user >= 0 and system >= 0, "CPU counter regression")
    low = last["clockBefore"]["lowerMs"] - first["clockAfter"]["upperMs"]
    high = last["clockAfter"]["upperMs"] - first["clockBefore"]["lowerMs"]
    require(high > 0 and low > 0, "nonpositive/overlapping device clock interval")
    result = {"userTicksDelta": user, "systemTicksDelta": system,
              "deviceBootTimeIntervalMsBounds": [low, high], "clockTicksPerSecond": hz}
    if hz:
        cpu_ms = (user + system) * 1000 / hz
        result["workerCpuPercentOfOneCoreBounds"] = [100 * cpu_ms / high, 100 * cpu_ms / low]
    return result


def load_collector():
    spec = importlib.util.spec_from_file_location("shutdown_collector", COLLECTOR_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(args):
    prior = json.loads(args.binding.read_text())
    helper = load_collector()
    helper.validate_spec(prior["spec"])
    spec = prior["spec"]
    require(prior["serial"] == args.serial and prior["package"] == PACKAGE, "binding target mismatch")
    require(type(prior.get("workerStartTicks")) is int and prior["workerStartTicks"] > 0, "start token absent")
    plan = {"phase": args.phase, "serial": args.serial, "workerPid": spec["workerPid"],
            "settleSeconds": 5, "measurementSeconds": 20, "cadenceSeconds": 2,
            "collectionBudgetSeconds": 90, "controlsGuest": False, "requestsPerformanceSamples": False,
            "bindingSha256": hashlib.sha256(args.binding.read_bytes()).hexdigest(),
            "scriptSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "collectorSha256": hashlib.sha256(COLLECTOR_PATH.read_bytes()).hexdigest()}
    if not args.execute:
        print(json.dumps({"status": "PLAN_ONLY_NO_ADB", **plan}, indent=2))
        return
    out = args.out.resolve()
    require(not any((parent / ".git").exists() for parent in (out, *out.parents)), "private output must be outside Git")
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    c = helper.Collector(out, args.serial, PACKAGE, adb=args.adb, budget=90)
    helper.write_json(out / "plan.json", plan)
    pid, uid, start = spec["workerPid"], spec["workerUid"], prior["workerStartTicks"]
    try:
        shell_uid = c.read("shell-uid", "id -u", app=False)[0].strip()
        require(shell_uid.isdigit() and int(shell_uid) > 0, "nonroot shell identity required")
        require(c.read("app-uid", "id -u")[0].strip() == str(uid).encode(), "app UID mismatch")
        require(c.boot("boot-start") == prior["bootId"], "binding boot changed")
        worker_identity(c.file("identity-status", f"/proc/{pid}/status").decode("ascii"),
                        c.file("identity-cmdline", f"/proc/{pid}/cmdline"), pid, uid)
        paths = c.read("apk-path", "pm path " + PACKAGE, app=False)[0].decode().splitlines()
        require(len(paths) == 1 and paths[0].startswith("package:/") and paths[0].endswith("/base.apk"),
                "single installed base APK required")
        actual_hash = c.read("apk-sha256", "sha256sum " + shlex.quote(paths[0][8:]))[0].split()[0].decode()
        require(actual_hash == spec["artifactPins"]["mainApkSha256"], "installed main APK mismatch")
        helper.record_metadata(c, spec, "binding")
        hz_data, hz_exit = c.read("clock-ticks", "getconf CLK_TCK", app=False, allowed=(0, 1, 2, 127))
        hz = int(hz_data.strip()) if hz_exit == 0 and hz_data.strip().isdigit() and int(hz_data) > 0 else None

        def ui(label):
            remote = "/data/local/tmp/lt-monitor-" + uuid.uuid4().hex + ".xml"
            try:
                c.read(label + "-dump", "uiautomator dump " + shlex.quote(remote), app=False)
                data = c.file(label + "-xml", remote, app=False)
                return panel_observation(data, args.phase, pid)
            finally:
                c.read(label + "-remove-owned-xml", "rm -f " + shlex.quote(remote), app=False)

        time.sleep(5)
        views = [{"at": "before", **ui("ui-before")}]
        samples, origin = [], time.monotonic()
        for index in range(11):
            target = origin + index * 2
            time.sleep(max(0, target - time.monotonic()))
            late = max(0, time.monotonic() - target)
            require(late < 2, "cadence missed; retain partial evidence and repeat manually")
            label = f"sample-{index:02d}"
            before = c.clock(label + "-clock-before")
            stat = worker_stat(c.file(label + "-stat", f"/proc/{pid}/stat"), pid, start)
            pss_data, pss_exit = c.read(label + "-pss", "head -c 262145 " + f"/proc/{pid}/smaps_rollup",
                                      allowed=(0, 1))
            require(len(pss_data) <= 262144, "PSS metadata exceeds bound")
            worker_stat(c.file(label + "-stat-after", f"/proc/{pid}/stat"), pid, start)
            samples.append({"clockBefore": before, "clockAfter": c.clock(label + "-clock-after"),
                            "stat": stat, "pssKiB": positive_pss(pss_data, pss_exit), "cadenceLateSeconds": late})
            helper.write_json(out / (label + ".sample.json"), samples[-1])
        views.append({"at": "after", **ui("ui-after")})
        worker_identity(c.file("final-status", f"/proc/{pid}/status").decode("ascii"),
                        c.file("final-cmdline", f"/proc/{pid}/cmdline"), pid, uid)
        require(c.boot("boot-end") == prior["bootId"], "device rebooted")
        pss = [s["pssKiB"] for s in samples if s["pssKiB"] is not None]
        helper.write_json(out / "result.json", {"status": "CAPTURED_NOT_BENCHMARK_PASS", **plan,
            "counterInterval": counter_interval(samples[0], samples[-1], hz), "samples": samples,
            "ui": views, "availablePssSamples": len(pss), "medianPssKiB": statistics.median(pss) if pss else None,
            "observerCostIncluded": True, "guestCpuRamMeasured": False, "guestAcceptance": False})
    except Exception as error:
        helper.write_json(out / "failure.json", {"status": "FAILED_PARTIAL_EVIDENCE_RETAINED", "error": str(error), **plan})
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--binding", type=Path, required=True, help="current strict collector before.json")
    parser.add_argument("--phase", choices=PHASES, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--execute", action="store_true", help="operator explicitly authorizes read-only target collection")
    run(parser.parse_args())
