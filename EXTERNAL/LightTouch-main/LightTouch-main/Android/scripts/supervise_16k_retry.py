#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Allocated single-AVD supervision; no import, guest start or acceptance inference."""
import argparse
import hashlib
import json
import multiprocessing
import os
from pathlib import Path
import pwd
import re
import shlex
import signal
import subprocess
import time
import xml.etree.ElementTree as ET

BOOT_LIMIT = 240
STOP_AT = 840
HARD_LIMIT = 900
PACKAGE = "org.lighttouch.android"
# This tool is Linux-only; keep its independent watcher a fork even on Python
# versions whose default start method is forkserver.
PROCESS_CONTEXT = multiprocessing.get_context("fork")


def process_identity(pid):
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
        fields = stat[stat.rfind(")") + 2:].split()
        return int(fields[1]), int(fields[19])  # parent, start ticks
    except (OSError, ValueError, IndexError):
        return None


def retain_descendants(root, identities):
    parents = set()
    for pid, start in identities.items():
        identity = process_identity(pid)
        if identity and identity[1] == start:
            parents.add(pid)
    if root not in identities:
        identity = process_identity(root)
        if identity:
            identities[root] = identity[1]
            parents.add(root)
    for _ in range(8):
        added = set()
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            pid = int(entry.name)
            identity = process_identity(pid)
            if identity and identity[0] in parents:
                if pid in identities and identities[pid] != identity[1]:
                    continue
                identities.setdefault(pid, identity[1])
                added.add(pid)
        if added <= parents:
            break
        parents |= added


def signal_owned(identities, sig):
    # A reused PID is never an owned process. Children first, wrapper last.
    denied = []
    for pid, start in sorted(identities.items(), reverse=True):
        identity = process_identity(pid)
        if identity and identity[1] == start:
            try:
                os.kill(pid, sig)
            except ProcessLookupError:
                pass
            except PermissionError:
                # A sudo monitor may retain credentials. Never let its refusal
                # prevent disposal of the signalable owned emulator/Xvfb.
                denied.append(pid)
    return denied


def watchdog(root, deadline, complete, output, root_start_ticks):
    """Separate OS process: model turns/main-thread window or adb stalls cannot postpone it."""
    owned = {root:root_start_ticks}
    while not complete.wait(.25):
        retain_descendants(root, owned)
        remaining = deadline - time.monotonic()
        if remaining <= 2:
            signal_owned(owned, signal.SIGTERM)
        if remaining <= .5:
            denied = signal_owned(owned, signal.SIGKILL)
            # Disposal precedes best-effort file IO; receipt is not clean proof.
            Path(output).write_text(json.dumps({"forcedDeadlineDisposal": True, "remainingSeconds":remaining,
                "ownedProcessIdentities": owned, "signalPermissionDeniedPids":denied,
                "guestShutdownConfirmed": False}) + "\n")
            return


def semantic_stop_bounds(xml):
    candidates = []
    def walk(node, clickable=None):
        if node.get("enabled") == "false":
            clickable = None
        if node.get("clickable") == "true" and node.get("enabled") == "true":
            clickable = node
        if node.get("text") == "Stop" and clickable is not None and clickable.get("package") == PACKAGE:
            match = re.fullmatch(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", clickable.get("bounds", ""))
            if match:
                x1, y1, x2, y2 = map(int, match.groups())
                if x2 > x1 and y2 > y1:
                    candidates.append(((x1 + x2)//2, (y1 + y2)//2))
        for child in node:
            walk(child, clickable)
    walk(ET.fromstring(xml))
    if len(set(candidates)) != 1:
        raise ValueError("Exactly one enabled semantic Stop control required")
    return candidates[0]


class Attempt:
    def __init__(self, args):
        self.args = args
        self.expected_page_bytes = getattr(args,"expected_page_bytes",16384)
        if self.expected_page_bytes not in (4096,16384):
            raise ValueError("Expected page size must be exactly 4096 or 16384")
        self.actual_page_bytes = None
        self.output = Path(args.output)
        self.output.mkdir(parents=True, exist_ok=False)
        self.start = time.monotonic()
        self.deadline = self.start + HARD_LIMIT
        self.commands = self.output / "commands.jsonl"
        self.avd = None
        self.avd_start_ticks = None
        self.owned_avd_identities = {}
        self.logcat = None
        self.watcher = None
        self.complete = PROCESS_CONTEXT.Event()
        self.worker_pid = None
        self.stop_attempted = False
        self.record_id = getattr(args,"record_id",None)
        self.fresh_library = getattr(args,"fresh_library",False)
        self.import_command_sha256 = None
        self.service_server_pid = None
        self.bound_app_uid = None

    def write(self, name, value):
        (self.output/name).write_text(json.dumps(value, indent=2) + "\n")

    def adb(self, *argv, timeout=8, binary=False):
        command = [self.args.adb, "-s", "emulator-5554", *argv]
        begin = time.monotonic()
        try:
            result = subprocess.run(command, capture_output=True, timeout=min(timeout, max(.1, self.deadline-begin)))
            code, stdout, stderr = result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired as error:
            code, stdout, stderr = 124, error.stdout or b"", error.stderr or b""
        record = {"argv": command, "elapsedSeconds": begin-self.start, "seconds": time.monotonic()-begin,
                  "exit": code, "stderr": stderr.decode(errors="replace")}
        if binary:
            record.update(stdoutEncoding="binary-not-embedded",stdoutBytes=len(stdout),
                          stdoutSha256=hashlib.sha256(stdout).hexdigest())
        else:
            record["stdout"] = stdout.decode(errors="replace")
        with self.commands.open("a") as stream:
            stream.write(json.dumps(record) + "\n")
        return code, stdout

    def launch(self):
        # Refuse an existing target: the supervisor may only dispose its own AVD.
        result = subprocess.run([self.args.adb, "devices", "-l"], capture_output=True, timeout=5)
        if result.returncode != 0:
            raise RuntimeError("Cannot enumerate ADB devices; no launch or cleanup authorized")
        if any(line.split() and line.split()[0] == b"emulator-5554" for line in result.stdout.splitlines()):
            raise RuntimeError("emulator-5554 already exists; no launch or cleanup authorized")
        memory_mb = getattr(self.args,"memory_mb",2048)
        cores = getattr(self.args,"cores",2)
        command = ["sudo", "-n", "-u", self.args.user, "xvfb-run", "-a", "-s", "-screen 0 1280x1024x24",
                   self.args.emulator, "-avd", self.args.avd, "-port", "5554", "-no-window", "-no-snapshot", "-no-audio",
                   "-gpu", "swiftshader", "-cores", str(cores), "-memory", str(memory_mb)]
        log = (self.output/"avd.log").open("wb")
        self.avd = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        log.close()
        original = process_identity(self.avd.pid)
        if original is None:raise RuntimeError('Original launched wrapper identity unavailable')
        self.avd_start_ticks = original[1]
        self.owned_avd_identities = {self.avd.pid:self.avd_start_ticks}
        self.watcher = PROCESS_CONTEXT.Process(target=watchdog,
            args=(self.avd.pid, self.deadline, self.complete, str(self.output/"hard-deadline.json"), self.avd_start_ticks))
        self.watcher.start()
        self.write("launch.json", {"argv": command, "wrapperPid": self.avd.pid,"wrapperStartTicks":self.avd_start_ticks,
            "requestedRamMB": memory_mb, "requestedGuestCores": cores,
            "expectedPageBytes": self.expected_page_bytes,
            "priorActualRamMB": 2560, "hardLimitSeconds": HARD_LIMIT,
            "bootLimitSeconds": BOOT_LIMIT, "stopAtSeconds": STOP_AT})

    def gate(self):
        stable_pid = None
        stable_since = None
        while time.monotonic()-self.start < BOOT_LIMIT:
            retain_descendants(self.avd.pid,self.owned_avd_identities)
            if self.avd.poll() is not None:
                raise RuntimeError("Owned emulator exited before readiness")
            code, pages = self.adb("shell", "getconf", "PAGE_SIZE", timeout=4)
            if code == 0 and self.logcat is None:
                log = (self.output/"android-all.log").open("wb")
                self.logcat = subprocess.Popen([self.args.adb, "-s", "emulator-5554", "logcat", "-b", "all", "-v", "threadtime"],
                                              stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                log.close()
            if code != 0:
                time.sleep(1)
                continue
            if pages.strip() != str(self.expected_page_bytes).encode():
                raise RuntimeError(f"Actual Android page size differs from expected {self.expected_page_bytes}")
            self.actual_page_bytes = self.expected_page_bytes
            checks = [self.adb("shell", "getprop", "sys.boot_completed", timeout=4),
                      self.adb("shell", "service", "check", "activity", timeout=4),
                      self.adb("shell", "service", "check", "package", timeout=4),
                      self.adb("shell", "pidof", "system_server", timeout=4)]
            pid = checks[3][1].strip()
            healthy = all(c == 0 for c, _ in checks) and checks[0][1].strip() == b"1" and pid.isdigit()
            healthy = healthy and all(b"found" in checks[i][1] and b"not found" not in checks[i][1] for i in (1,2))
            self.adb("shell", "cat", "/proc/meminfo", "/proc/pressure/memory", timeout=4)
            self.adb("shell", "ps", "-A", "-o", "UID,PID,PPID,NAME", timeout=4)
            with (self.output/"host-memory.jsonl").open("a") as stream:
                stream.write(json.dumps({"elapsedSeconds":time.monotonic()-self.start,
                    "meminfo":Path("/proc/meminfo").read_text(), "pressure":Path("/proc/pressure/memory").read_text()})+"\n")
            if not healthy or pid != stable_pid:
                stable_since = time.monotonic() if healthy else None
                stable_pid = pid if healthy else None
            if healthy and time.monotonic()-stable_since >= 30:
                self.service_server_pid = pid.decode()
                if self.fresh_library:
                    if time.monotonic()-self.start >= BOOT_LIMIT:
                        raise RuntimeError("Fresh service readiness exceeded boot gate")
                    ready = {"serviceGatePassed":True,"recordGatePassed":False,"uiGatePassed":False,
                        "freshLibrary":True,"visualUIInspectionRequired":True,"uiGateDeadlineSeconds":BOOT_LIMIT,
                        "pageBytes":self.actual_page_bytes,"expectedPageBytes":self.expected_page_bytes,"systemServerPid":self.service_server_pid,"appUid":None,"recordId":None,
                        "elapsedSeconds":time.monotonic()-self.start,"guestStartedBySupervisor":False,
                        "guestAcceptancePassed":False,"stopAtSeconds":STOP_AT,"hardDeadlineSeconds":HARD_LIMIT}
                    self.write("ready.json",ready)
                    return ready
                return self.verify_record(self.service_server_pid)
            time.sleep(1)
        raise RuntimeError("Android services did not remain stable within 240 seconds")

    def verify_record(self, system_pid, imported_record=None, expected_uid=None):
        record = imported_record if imported_record is not None else self.record_id
        root = f"files/library/records/{record}"
        for path in (f"{root}/record.json", f"{root}/state", f"{root}/base/device.lock.json", f"{root}/assets"):
            if self.path_presence(path) is not True:
                raise RuntimeError("Retained record is incomplete; no import or reset attempted")
        marker = self.path_presence(f"{root}/state/.dirty")
        if marker is True:
            raise RuntimeError("Retained record is dirty; this normal-start recipe cannot silently recover")
        if marker is not False:
            raise RuntimeError("Could not confirm absence of dirty marker")
        code, selected = self.adb("exec-out", "run-as", PACKAGE, "cat", "files/library/selected")
        if code != 0 or selected.strip().decode() != record:
            raise RuntimeError("Retained record is not selected; no selection mutation attempted")
        code, metadata_bytes = self.adb("exec-out", "run-as", PACKAGE, "cat", f"{root}/record.json")
        if code:
            raise RuntimeError("Retained record metadata unreadable")
        metadata = json.loads(metadata_bytes)
        if (metadata.get("schema") != 1 or metadata.get("id") != record or
            metadata.get("base_sha256") != self.args.lock_sha256 or
            metadata.get("bootrom_sha256") != self.args.rom_sha256 or
            metadata.get("guest_package_sha256") != self.args.guest_sha256):
            raise RuntimeError("Retained record pins differ from verified source receipt")
        (self.output/"retained-record.json").write_bytes(metadata_bytes)
        pins = [("base/device.lock.json", self.args.lock_sha256),
                ("assets/bootrom.bin", self.args.rom_sha256), ("assets/guest.itpack", self.args.guest_sha256)]
        if self.args.record_sha256:
            pins.append(("record.json", self.args.record_sha256))
        for filename, expected in pins:
            code, data = self.adb("exec-out", "run-as", PACKAGE, "cat", f"{root}/{filename}",binary=filename.startswith("assets/"))
            if code or hashlib.sha256(data).hexdigest() != expected:
                raise RuntimeError("Retained record identity differs from supplied frozen receipt")
        code, uid = self.adb("exec-out", "run-as", PACKAGE, "id", "-u")
        if code or not uid.strip().isdigit() or int(uid) <= 10000:
            raise RuntimeError("Normal target app UID was not verified")
        if expected_uid is not None and int(uid) != expected_uid:
            raise RuntimeError("Import receipt UID differs from actual target UID")
        if imported_record is None and time.monotonic()-self.start >= BOOT_LIMIT:
            raise RuntimeError("Service/record readiness exceeded boot gate")
        if imported_record is not None and time.monotonic()-self.start >= STOP_AT:
            raise RuntimeError("Import verification exceeded guest-start budget")
        ready = {"serviceGatePassed": True, "recordGatePassed": True, "uiGatePassed":False,
                 "visualUIInspectionRequired": True, "uiGateDeadlineSeconds":BOOT_LIMIT, "appUid": int(uid),
                 "pageBytes":self.actual_page_bytes,"expectedPageBytes":self.expected_page_bytes, "systemServerPid": system_pid, "recordId":record,
                 "elapsedSeconds":time.monotonic()-self.start, "guestStartedBySupervisor":False,
                 "guestAcceptancePassed":False, "stopAtSeconds":STOP_AT, "hardDeadlineSeconds":HARD_LIMIT}
        self.write("record-bound.json" if imported_record is not None else "ready.json", ready)
        return ready

    def bind_imported_record(self):
        if not self.fresh_library:
            return
        command_file = self.output/"import-record.json"
        if not command_file.exists():
            return
        raw = command_file.read_bytes()
        if len(raw)>16384:
            raise RuntimeError("Import controller receipt exceeds bound")
        digest = hashlib.sha256(raw).hexdigest()
        if self.import_command_sha256 is not None:
            if digest != self.import_command_sha256:
                raise RuntimeError("Imported record choice is already fixed")
            return
        command = json.loads(raw)
        record,uid,path,pin = (command.get(k) for k in ("recordId","appUid","importReceiptPath","importReceiptSha256"))
        if (command.get("schema")!=1 or not isinstance(record,str) or not re.fullmatch(r"[0-9a-f]{32}",record)
            or type(uid) is not int or uid<=10000 or not isinstance(path,str)
            or not re.fullmatch(r"files/import-evidence-[0-9]+/result\.json",path)
            or not isinstance(pin,str) or not re.fullmatch(r"[0-9a-f]{64}",pin)):
            raise RuntimeError("Invalid fixed import controller receipt")
        code,data = self.adb("exec-out","run-as",PACKAGE,"cat",path)
        if code or len(data)>65536 or hashlib.sha256(data).hexdigest()!=pin:
            raise RuntimeError("Actual app import receipt does not match frozen controller pin")
        receipt = json.loads(data)
        if receipt.get("pass") is not True or receipt.get("recordId")!=record or receipt.get("appUid")!=uid:
            raise RuntimeError("Actual app import did not pass for the requested identity/UID")
        self.verify_record(self.service_server_pid,imported_record=record,expected_uid=uid)
        self.record_id = record
        self.bound_app_uid = uid
        self.import_command_sha256 = digest
        self.write("import-binding.json",{"recordId":record,"appUid":uid,"importReceiptPath":path,
            "importReceiptSha256":pin,"controllerSha256":digest,"elapsedSeconds":time.monotonic()-self.start,
            "verifiedLockSha256":self.args.lock_sha256,"verifiedRomSha256":self.args.rom_sha256,
            "verifiedGuestPackageSha256":self.args.guest_sha256,
            "guestStartedBySupervisor":False,"guestAcceptancePassed":False})

    def path_presence(self, path):
        # exec-out preserves bytes, but can return host exit0 for a remote stat
        # failure. Require an explicit remote sentinel, with readable parent.
        parent = str(Path(path).parent)
        script = (f"if test ! -d {shlex.quote(parent)} || test ! -x {shlex.quote(parent)}; then "
                  "printf 'LT_PATH_UNKNOWN\\n'; "
                  f"elif test -e {shlex.quote(path)} || test -L {shlex.quote(path)}; then "
                  "printf 'LT_PATH_PRESENT\\n'; else printf 'LT_PATH_ABSENT\\n'; fi")
        code, output = self.adb("shell", shlex.join(["run-as",PACKAGE,"sh","-c",script]))
        if code != 0:
            return None
        return {b"LT_PATH_PRESENT":True,b"LT_PATH_ABSENT":False}.get(output.strip())

    def stop(self):
        self.stop_attempted = True
        original=process_identity(self.avd.pid) if self.avd is not None else None
        if self.avd_start_ticks is None or not original or original[1]!=self.avd_start_ticks:
            self.write("stop-admission.json",{"guestShutdownConfirmed":False,"originalWrapperIdentityMatched":False})
            return
        code, pid = self.adb("shell", "pidof", PACKAGE+":engine", timeout=3)
        if code == 0 and pid.strip().isdigit():
            self.worker_pid = pid.strip().decode()
        self.write("stop-required.json", {"elapsedSeconds":time.monotonic()-self.start,
            "workerPid":self.worker_pid,"guestShutdownConfirmed":False})
        # Only a unique enabled semantic Stop can admit native shutdown. No guessed coordinates.
        try:
            if self.adb("shell", "am", "start", "-W", "-n", PACKAGE+"/.MainActivity", timeout=5)[0]:
                raise RuntimeError("LightTouch foreground launch failed; semantic Stop unconfirmed")
            remote = f"/data/local/tmp/lighttouch-stop-{os.getpid()}.xml"
            if self.adb("shell", "uiautomator", "dump", "--compressed", remote, timeout=6)[0]:
                raise RuntimeError("Semantic Stop hierarchy unavailable")
            code, xml = self.adb("exec-out", "cat", remote, timeout=3)
            (self.output/"stop-hierarchy.xml").write_bytes(xml)
            if code:
                raise RuntimeError("Semantic Stop hierarchy extraction failed")
            x, y = semantic_stop_bounds(xml)
            code, _ = self.adb("shell", "input", "tap", str(x), str(y), timeout=3)
            self.write("stop-admission.json", {"commandExit":code,"semanticControl":"Stop",
                       "guestShutdownConfirmed":False})
        except (RuntimeError, ValueError, ET.ParseError) as error:
            self.write("stop-admission.json", {"error":str(error),"guestShutdownConfirmed":False})
        deadline = min(self.deadline-20, time.monotonic()+35)
        while self.worker_pid and time.monotonic() < deadline:
            code, pids = self.adb("shell", "pidof", PACKAGE+":engine", timeout=2)
            if code != 0 or self.worker_pid not in pids.decode().split():
                break
            time.sleep(.5)
        if self.worker_pid:
            for prefix, suffix in (("worker-shutdown-",".json"),("worker-stop-error-",".txt")):
                code, data = self.adb("exec-out", "run-as", PACKAGE, "cat", f"files/{prefix}{self.worker_pid}{suffix}", timeout=2)
                (self.output/f"{prefix}{self.worker_pid}{suffix}").write_bytes(data)
                self.write(f"{prefix}extraction.json", {"exit":code,"guestShutdownConfirmed":False})
        if self.record_id is not None:
            self.adb("exec-out", "run-as", PACKAGE, "stat", f"files/library/records/{self.record_id}/state/.dirty", timeout=2)
            self.write("record-marker-stop.json",{"recordId":self.record_id,
                "statePathPresent":self.path_presence(f"files/library/records/{self.record_id}/state"),
                "dirtyMarkerPresent":self.path_presence(f"files/library/records/{self.record_id}/state/.dirty"),
                "guestShutdownConfirmed":False})
        else:
            self.write("unbound-record-stop.json",{"recordId":None,"dirtyMarkerPathKnown":False,
                "guestShutdownConfirmed":False})

    def cleanup(self):
        if self.avd is None:
            return
        owned = dict(self.owned_avd_identities)
        if self.avd_start_ticks is not None:retain_descendants(self.avd.pid, owned)
        original=process_identity(self.avd.pid)
        if self.avd_start_ticks is not None and original and original[1]==self.avd_start_ticks:
            self.adb("emu", "kill", timeout=3)
        try:
            self.avd.wait(timeout=min(5,max(.1,self.deadline-time.monotonic())))
        except subprocess.TimeoutExpired:
            signal_owned(owned, signal.SIGTERM)
        signal_owned(owned, signal.SIGKILL)
        try:
            self.avd.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
        if self.logcat is not None:
            self.logcat.terminate()
            try:
                self.logcat.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.logcat.kill()
        self.complete.set()
        if self.watcher is not None and self.watcher.pid is not None:
            self.watcher.join(timeout=2)
        remaining = {}
        for pid,start in owned.items():
            identity = process_identity(pid)
            if identity and identity[1] == start:
                remaining[pid] = start
        self.write("terminal.json", {"elapsedSeconds":time.monotonic()-self.start,
            "ownedLauncherExit":self.avd.poll(),"stopAttempted":self.stop_attempted,
            "remainingOwnedIdentities":remaining,
            "guestShutdownConfirmed":False,"acceptanceRequiresIndependentReceipts":True})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allocation-granted", action="store_true", required=True)
    parser.add_argument("--adb", required=True)
    parser.add_argument("--emulator", required=True)
    parser.add_argument("--avd", default="LightTouch_API35_16K")
    parser.add_argument("--expected-page-bytes",type=int,choices=(4096,16384),default=16384)
    parser.add_argument("--memory-mb",type=int,choices=(2048,4096),default=2048)
    parser.add_argument("--cores",type=int,choices=(2,4),default=2)
    parser.add_argument("--user", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--record-id")
    mode.add_argument("--fresh-library",action="store_true")
    parser.add_argument("--record-sha256")
    parser.add_argument("--lock-sha256", required=True)
    parser.add_argument("--rom-sha256", required=True)
    parser.add_argument("--guest-sha256", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.user != pwd.getpwuid(os.getuid()).pw_name:
        parser.error("Launch user must be the current user so owned children remain signalable")
    hashes = [args.lock_sha256,args.rom_sha256,args.guest_sha256]
    if args.record_sha256:
        hashes.append(args.record_sha256)
    if (args.record_id is not None and not re.fullmatch(r"[0-9a-f]{32}",args.record_id)) or any(not re.fullmatch(r"[0-9a-f]{64}",x) for x in hashes):
        parser.error("Record ID and frozen record/lock hashes must be lowercase hex")
    attempt = Attempt(args)
    try:
        attempt.launch()
        attempt.gate()
        print(f"SERVICE/RECORD READY ONLY: verify/install frozen APKs, then foreground and visually inspect UI by original240s; receipts: {args.output}",flush=True)
        while time.monotonic()-attempt.start < STOP_AT and not (attempt.output/"request-stop").exists():
            retain_descendants(attempt.avd.pid,attempt.owned_avd_identities)
            if attempt.avd.poll() is not None:
                raise RuntimeError("Owned emulator exited during guest observation")
            if attempt.fresh_library:
                attempt.bind_imported_record()
            time.sleep(.5)
        attempt.stop()
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        attempt.write("failure.json", {"error":str(error),"elapsedSeconds":time.monotonic()-attempt.start,"guestAcceptancePassed":False})
        return 1
    finally:
        attempt.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
