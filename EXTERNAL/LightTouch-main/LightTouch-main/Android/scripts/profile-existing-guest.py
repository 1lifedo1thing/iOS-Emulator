#!/usr/bin/env python3
"""Plan/attach a bounded own-app CPU profile; never launch or stop the guest."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid

PACKAGE = "org.lighttouch.android"
NDK_VERSION = "28.2.13676358"
PINS = {
    "arm64": "7eb29ecba8ae792397bd8c1b0fcd874fed0dc4c44d0cf9cb284e948f80cfe7e5",
    "x86_64": "570bf3bcebb261e98e20d523aa454d63a9dfb7c6580951dbef1297bf68f49c06",
    "host": "5c05c264b627de770cff9c02b6773931d3d6911a478b54f30428721d26fa8aea",
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def record_options(ids, percent, output, event="task-clock:u", threads=False):
    # Keep existing cpu-percent exactly; use tiny rings and bounded stack/data.
    return ["record", "--app", PACKAGE, "-t" if threads else "-p", ",".join(map(str, sorted(ids)))] + (["--no-inherit"] if threads else []) + [
            "-f", "99", "-e", event, "--duration", "10",
            "--clockid", "monotonic", "--call-graph", "dwarf,8192", "-m", "1",
            "--user-buffer-size", "16M", "--size-limit", "32M",
            "--cpu-percent", str(percent), "--exit-with-parent", "--no-dump-kernel-symbols", "-o", output]


def thread_identity(status, stat, comm, engine_pid, tid, uid):
    fields = {line.split(":", 1)[0]: line.split(":", 1)[1].split()
              for line in status.splitlines() if ":" in line}
    if (int(fields["Uid"][0]) != uid or int(fields["Tgid"][0]) != engine_pid
            or int(fields["Pid"][0]) != tid):
        raise ValueError("Selected TID is not owned by the verified engine/UID")
    if int(stat.split("(", 1)[0]) != tid:
        raise ValueError("Thread stat identity changed")
    values = stat.rsplit(")", 1)[1].split()
    return {"tid": tid, "tgid": engine_pid, "uid": uid,
            "start_ticks": int(values[19]), "comm": comm.rstrip("\n")}


def report_counts(text):
    samples = [int(value.replace(",", "")) for value in re.findall(r"^Samples:\s*([\d,]+)\s*$", text, re.MULTILINE)]
    events = [int(value.replace(",", "")) for value in re.findall(r"^Event count:\s*([\d,]+)\s*$", text, re.MULTILINE)]
    if len(samples) != 1 or len(events) != 1:
        raise ValueError("Expected exactly one sampling event/count in host report")
    return {"samples": samples[0], "reported_sample_event_count": events[0],
            "nonzero_sample_data": samples[0] > 0 and events[0] > 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--engine-pid", type=int, required=True)
    parser.add_argument("--arch", choices=["arm64", "x86_64"], default="arm64")
    parser.add_argument("--scope", choices=["engine", "engine-threads", "app"], default="engine")
    # Some Android kernels report zero samples for task-clock:u despite CPU
    # counter growth. Allow the explicit unfiltered event used by successful
    # target recordings, retaining the same permission and zero-sample checks.
    parser.add_argument("--event", choices=["task-clock:u", "task-clock", "cpu-cycles:u"], default="task-clock:u")
    parser.add_argument("--out", type=Path, required=True, help="New private host evidence directory")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--execute", action="store_true", help="Run reviewed plan against the named existing worker")
    args = parser.parse_args()
    if args.engine_pid <= 0:
        parser.error("engine PID must be positive")
    tool = args.ndk / "simpleperf/bin/android" / args.arch / "simpleperf"
    host = args.ndk / "simpleperf/bin/linux/x86_64/simpleperf"
    if sha(tool) != PINS[args.arch] or sha(host) != PINS["host"]:
        parser.error("Expected pinned NDK28.2 simpleperf tool bytes")
    run_id = "lt-perf-" + uuid.uuid4().hex
    remote = "/data/local/tmp/" + run_id
    receipt = {"schema": 1, "package": PACKAGE, "ndk": NDK_VERSION,
               "helper_source_sha256": sha(__file__),
               "ndk_dispatcher_sha256": PINS[args.arch], "host_report_sha256": PINS["host"],
               "scope": args.scope, "engine_pid": args.engine_pid, "remote_owned_directory": remote,
               "duration_seconds": 10, "frequency_hz": 99, "event": args.event,
               "sampling_clock": "device CLOCK_MONOTONIC", "commands": [],
               "launch_or_restart_requested": False, "device_qualification": False,
               "ndk_only_recorder_claimed": False,
               "recorder_note": "Android --app may delegate to the platform simpleperf_app_runner; retain version/logs, not an NDK-only recording claim."}
    if not args.execute:
        receipt["status"] = "PLAN_ONLY_NO_DEVICE_ACCESS"
        receipt["example_record_argv"] = [remote + "/simpleperf"] + record_options(
            [args.engine_pid], "EXISTING_CPU_PERCENT", remote + "/perf.data", args.event,
            threads=args.scope == "engine-threads")
        if args.scope == "engine-threads":
            receipt["example_selector_note"] = "Example TID is illustrative; execution enumerates and verifies every existing engine task before -t selection."
        print(json.dumps(receipt, indent=2))
        return 0
    args.out.mkdir(parents=True, exist_ok=False, mode=0o700)
    adb = [args.adb, "-s", args.serial]

    def save():
        (args.out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")

    def invoke(label, tail, timeout=5, required=True):
        command = adb + tail
        before = time.monotonic_ns()
        try:
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
            code, output = result.returncode, result.stdout
        except subprocess.TimeoutExpired as failure:
            code, output = 124, failure.stdout or b""
        (args.out / (label + ".log")).write_bytes(output)
        receipt["commands"].append({"label": label, "argv": command, "exit": code,
                                    "host_monotonic_begin_ns": before, "host_monotonic_end_ns": time.monotonic_ns()})
        save()
        if required and code:
            raise RuntimeError(label + " failed; no permissions/settings workaround attempted")
        return code, output

    def shell(label, command, **kwargs):
        return invoke(label, ["shell", shlex.join(command)], **kwargs)

    def own(label, command, **kwargs):
        return shell(label, ["run-as", PACKAGE] + command, **kwargs)

    def identity(pid, stage):
        _, status = own(f"{stage}-{pid}-status", ["cat", f"/proc/{pid}/status"])
        _, name = own(f"{stage}-{pid}-cmdline", ["cat", f"/proc/{pid}/cmdline"])
        _, stat = own(f"{stage}-{pid}-stat", ["cat", f"/proc/{pid}/stat"])
        user = int(next(line.split()[1] for line in status.decode().splitlines() if line.startswith("Uid:")))
        process = name.split(b"\0", 1)[0].decode()
        if user != uid or process not in [PACKAGE, PACKAGE + ":engine", PACKAGE + ":firmware"]:
            raise RuntimeError("PID does not belong to the selected package/UID")
        fields = stat.decode().rsplit(")", 1)[1].split()
        ticks = int(fields[19])
        receipt.setdefault("process_counter_samples", []).append({"stage": stage, "pid": pid,
            "user_ticks": int(fields[11]), "system_ticks": int(fields[12]), "thread_count": int(fields[17])})
        shell(f"{stage}-{pid}-threads", ["ps", "-T", "-p", str(pid), "-o", "PID,TID,NAME"], required=False)
        # ps NAME can be the process cmdline for every thread. Read actual comm.
        own(f"{stage}-{pid}-thread-comm", ["sh", "-c",
            f'for t in /proc/{pid}/task/*; do printf "%s " "${{t##*/}}"; cat "$t/comm"; done'], required=False)
        return {"pid": pid, "uid": user, "process": process, "start_ticks": ticks}

    def selected_threads(stage, tids=None):
        if tids is None:
            _, listing = own("selected-task-list", ["ls", "-1", f"/proc/{args.engine_pid}/task"])
            names = listing.decode().splitlines()
            if not names or any(not name.isdecimal() or int(name) <= 0 for name in names):
                raise RuntimeError("Invalid or empty engine task listing")
            tids = sorted({int(name) for name in names})
        result = []
        for tid in tids:
            path = f"/proc/{args.engine_pid}/task/{tid}"
            _, status = own(f"{stage}-tid-{tid}-status", ["cat", path + "/status"])
            _, stat = own(f"{stage}-tid-{tid}-stat", ["cat", path + "/stat"])
            _, comm = own(f"{stage}-tid-{tid}-comm", ["cat", path + "/comm"])
            result.append(thread_identity(status.decode(), stat.decode(), comm.decode(), args.engine_pid, tid, uid))
        return result

    try:
        _, shell_uid = shell("shell-uid", ["id", "-u"])
        if int(shell_uid) == 0:
            raise RuntimeError("Refuse privileged shell; helper never invokes adb root/unroot")
        _, own_uid = own("app-uid", ["id", "-u"])
        uid = int(own_uid)
        pids = {args.engine_pid}
        if args.scope == "app":
            for process in [PACKAGE, PACKAGE + ":engine", PACKAGE + ":firmware"]:
                _, output = shell("pidof-" + process.rsplit(".", 1)[-1].replace(":", "-"), ["pidof", process], required=False)
                pids.update(int(value) for value in output.split() if value.isdigit())
        before = [identity(pid, "before") for pid in sorted(pids)]
        if next(i for i in before if i["pid"] == args.engine_pid)["process"] != PACKAGE + ":engine":
            raise RuntimeError("Expected an already running dedicated engine worker")
        receipt["identities_before"] = before
        selected_ids = pids
        if args.scope == "engine-threads":
            receipt["selected_thread_identities_before"] = selected_threads("before")
            selected_ids = {i["tid"] for i in receipt["selected_thread_identities_before"]}
            receipt["thread_selection_note"] = "Existing verified tasks only; later-created threads are not included. Missing/reused selected tasks invalidate this recording."
        limits = {}
        for name in ["perf_event_paranoid", "perf_event_max_sample_rate", "perf_cpu_time_max_percent", "perf_event_mlock_kb"]:
            _, value = shell(name, ["cat", "/proc/sys/kernel/" + name])
            limits[name] = int(value)
        _, cpus = shell("configured-cpus", ["getconf", "_NPROCESSORS_CONF"])
        _, pages = shell("page-size", ["getconf", "PAGE_SIZE"])
        minimum_mlock = int(cpus) * 2 * max(4, int(pages) // 1024)
        receipt["existing_limits"] = limits
        # Simpleperf's internal default max is 4000 even with -f99. Avoid any
        # attempt to raise settings, and keep its cpu-percent equal to existing.
        if (limits["perf_event_paranoid"] > 1 or limits["perf_event_max_sample_rate"] < 4000
                or not 1 <= limits["perf_cpu_time_max_percent"] <= 100
                or limits["perf_event_mlock_kb"] < minimum_mlock):
            raise RuntimeError("Existing perf limits do not satisfy no-settings-change preflight")
        shell("create-owned-directory", ["mkdir", "-m", "755", remote])
        invoke("push-pinned-dispatcher", ["push", str(tool), remote + "/simpleperf"])
        shell("executable-permission", ["chmod", "755", remote + "/simpleperf"])
        shell("device-dispatcher-version", [remote + "/simpleperf", "--version"])
        shell("device-record-help", [remote + "/simpleperf", "record", "--help"])
        # Prefer the NDK dispatcher; --app is the supported non-root app route.
        # No app_profiler wrapper, launch command, Java compilation or property write.
        command = [remote + "/simpleperf"] + record_options(selected_ids, limits["perf_cpu_time_max_percent"], remote + "/perf.data", args.event,
                                                          threads=args.scope == "engine-threads")
        code, _ = shell("record", command, timeout=35, required=False)
        receipt["identities_after"] = [identity(pid, "after") for pid in sorted(pids)]
        receipt["worker_identity_preserved"] = receipt["identities_after"] == before
        if args.scope == "engine-threads":
            receipt["selected_thread_identities_after"] = selected_threads("after", sorted(selected_ids))
            receipt["selected_thread_identities_preserved"] = receipt["selected_thread_identities_after"] == receipt["selected_thread_identities_before"]
            if not receipt["selected_thread_identities_preserved"]:
                raise RuntimeError("Selected TID owner/start ticks/comm changed; profile remains unqualified")
        if code or not receipt["worker_identity_preserved"]:
            raise RuntimeError("Sampling failed or selected worker changed; profile remains unqualified")
        invoke("pull-owned-profile", ["pull", remote + "/perf.data", str(args.out / "perf.data")])
        receipt["perf_data_sha256"] = sha(args.out / "perf.data")
        receipt["host_report_command"] = [str(host), "report", "-i", str(args.out / "perf.data"),
                                           "--sort", "pid,tid,comm,dso,symbol", "-g", "-n"]
        report = subprocess.run(receipt["host_report_command"], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=30)
        (args.out / "report.txt").write_bytes(report.stdout)
        (args.out / "report.stderr").write_bytes(report.stderr)
        receipt["host_report_exit"] = report.returncode
        if report.returncode:
            raise RuntimeError("Host report failed; no CPU attribution")
        receipt["report_counts"] = report_counts(report.stdout.decode())
        if not receipt["report_counts"]["nonzero_sample_data"]:
            receipt["status"] = "INCONCLUSIVE_ZERO_SAMPLES_NO_CPU_ATTRIBUTION"
            receipt["cpu_attribution_available"] = False
            save()
            print(json.dumps(receipt, indent=2))
            return 2
        receipt["status"] = "NONZERO_SAMPLES_RECORDED_SYMBOL_AND_WORKLOAD_REVIEW_PENDING"
        receipt["cpu_attribution_available"] = True
        save()
        print(json.dumps(receipt, indent=2))
        return 0
    except (RuntimeError, ValueError, KeyError, IndexError, StopIteration, OSError, subprocess.TimeoutExpired) as error:
        receipt["status"] = "UNAVAILABLE_OR_INCOMPLETE_NO_SETTINGS_CHANGED"
        receipt["error"] = str(error)
        receipt["fallback"] = "Retain own process/thread snapshots and independent same-worker frame/audio/status deltas; no sampling or guest pass inferred."
        save()
        print(json.dumps(receipt, indent=2))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
