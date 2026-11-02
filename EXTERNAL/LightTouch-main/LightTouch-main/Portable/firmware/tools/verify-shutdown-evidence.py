#!/usr/bin/env python3
"""Check retained Android shutdown evidence; never read a device or modify input."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


class EvidenceError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load(path):
    require(not path.is_symlink(), f"symlink input refused: {path.name}")
    require(path.is_file(), f"missing regular input: {path.name}")
    require(path.stat().st_size <= 262144, f"input exceeds metadata bound: {path.name}")
    with path.open("rb") as stream:
        data = stream.read(262145)
    require(len(data) <= 262144, f"input exceeds metadata bound: {path.name}")
    value = json.loads(data, object_pairs_hook=unique_object)
    require(type(value) is dict, f"JSON object required: {path.name}")
    return value, hashlib.sha256(data).hexdigest()


def integer(obj, key, minimum=0):
    value = obj.get(key)
    require(type(value) is int and value >= minimum, f"invalid integer: {key}")
    return value


def flag(obj, key, expected):
    require(obj.get(key) is expected, f"required {key}={str(expected).lower()}")


def sha(obj, key):
    value = obj.get(key)
    require(type(value) is str and re.fullmatch(r"[0-9a-f]{64}", value), f"invalid SHA256: {key}")
    return value


def check(directory):
    directory = Path(directory)
    hashes = {}

    def read(name):
        value, digest = load(directory / name)
        hashes[name] = digest
        return value

    expected = read("expected.json")
    require(integer(expected, "schema", 1) == 1, "unsupported expected schema")
    require(expected.get("clock") == "android-elapsed-realtime-ms", "expected clock must be Android elapsed realtime")
    require(expected.get("baseKind") in ("generated-stock", "reference-fixture"), "base kind must be explicit")
    pid = integer(expected, "workerPid", 1)
    uid = integer(expected, "workerUid", 1)
    record_id = expected.get("recordId")
    require(type(record_id) is str and re.fullmatch(r"[0-9a-f]{32}", record_id), "invalid record identity")
    pins = {key: sha(expected, key) for key in ("baseSha256", "bootromSha256", "guestPackageSha256")}
    artifacts = expected.get("artifactPins")
    require(type(artifacts) is dict, "artifact pins missing")
    for key in ("mainApkSha256", "jniSha256", "engineSha256"):
        sha(artifacts, key)
    grace = integer(expected, "shutdownGraceMs", 1)
    window_start = integer(expected, "acceptanceWindowStartMs")
    window_end = integer(expected, "acceptanceWindowEndMs")
    require(window_start <= window_end, "invalid acceptance window")

    record = read("record.json")
    require(integer(record, "schema", 1) == 1, "unsupported record schema")
    require(record.get("id") == record_id, "record identity mismatch")
    require(record.get("board") == "n72ap" and record.get("build") == "7E18", "not baseline n72ap/7E18")
    for key, actual in (("baseSha256", "base_sha256"), ("bootromSha256", "bootrom_sha256"),
                        ("guestPackageSha256", "guest_package_sha256")):
        require(record.get(actual) == pins[key], f"record {actual} mismatch")
    lock = read("device.lock.json")
    require(hashes["device.lock.json"] == pins["baseSha256"], "lock bytes do not match expected base")
    require(type(lock.get("format")) is int and lock["format"] == 1, "unsupported lock format")
    require(lock.get("board") == record["board"] and lock.get("build") == record["build"], "lock route mismatch")
    require(lock.get("boot_strategy") == "iboot", "unexpected baseline boot strategy")
    package = lock.get("guest_package", {}).get("itpack", {})
    require(package.get("sha256") == pins["guestPackageSha256"], "lock guest package mismatch")

    receipt_name = f"worker-shutdown-{pid}.json"
    receipt = read(receipt_name)
    require(integer(receipt, "workerPid", 1) == pid and integer(receipt, "workerUid", 1) == uid,
            "terminal worker identity mismatch")
    require(receipt.get("reason") == "native-terminal" and receipt.get("admissionStage") == "native-terminal",
            "not a native terminal receipt")
    require(receipt.get("nativeTerminalState") == "stopped", "native state is not stopped")
    for key in ("nativePollSnapshotValid", "guestShutdownConfirmed", "cleanShutdownConfirmed"):
        flag(receipt, key, True)
    for key in ("nativeStateDirty", "nativeStorageFailed", "shutdownConfirmationInferred",
                "nativeStateMarkersModified", "receiptPersistenceBestEffort"):
        flag(receipt, key, False)
    require(integer(receipt, "nativeEngineExit") == 0, "engine exit was not zero")
    require(receipt.get("error") == "", "terminal receipt retained an error")
    accepted = integer(receipt, "acceptedAtHostMs")
    deadline = integer(receipt, "deadlineHostMs")
    observed = integer(receipt, "observedAtHostMs")
    require(window_start <= accepted <= window_end, "receipt outside this Stop acceptance window")
    require(deadline - accepted == grace, "receipt shutdown grace mismatch")
    require(accepted <= observed < deadline, "terminal snapshot not within shutdown deadline")
    deadline_path = directory / f"worker-shutdown-deadline-{pid}.json"
    require(not deadline_path.exists() and not deadline_path.is_symlink(),
            "contradictory same-worker deadline evidence exists")

    post = read("post-stop.json")
    require(integer(post, "schema", 1) == 1 and post.get("clock") == expected["clock"], "post-stop schema/clock mismatch")
    require(integer(post, "workerPid", 1) == pid and integer(post, "workerUid", 1) == uid,
            "post-stop worker identity mismatch")
    require(post.get("recordId") == record_id and post.get("baseSha256") == pins["baseSha256"],
            "post-stop record binding mismatch")
    require(post.get("artifactPins") == artifacts, "captured artifact identity mismatch")
    post_time = integer(post, "observedAtHostMs")
    require(post_time >= observed, "post-stop probes precede terminal snapshot")
    probe = post.get("workerProbe")
    require(type(probe) is dict and integer(probe, "remoteExit") == 0, "worker probe failed remotely")
    pids = probe.get("pids")
    require(type(pids) is list and all(type(value) is int and value > 0 for value in pids), "invalid PID probe")
    require(pid not in pids, "stopped worker still present")
    for name, key, value in (("stateDirectoryProbe", "isDirectory", True), ("dirtyMarkerProbe", "exists", False)):
        probe = post.get(name)
        require(type(probe) is dict and integer(probe, "remoteExit") == 0, f"{name} failed remotely")
        flag(probe, key, value)
    return {"status": "PASS clean-shutdown evidence fields", "workerPid": pid, "recordId": record_id,
            "baseKind": expected["baseKind"], "terminalAfterAcceptanceMs": observed - accepted,
            "postProbeAfterAcceptanceMs": post_time - accepted, "shutdownGraceMs": grace,
            "inputSha256": hashes, "inputAuthenticityEstablished": False, "guestHomeChecked": False,
            "photoPersistenceChecked": False, "G6_G7GateClosed": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence_directory", type=Path)
    args = parser.parse_args()
    try:
        result = check(args.evidence_directory)
    except (EvidenceError, OSError, ValueError, AttributeError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error), "G6_G7GateClosed": False}, sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
