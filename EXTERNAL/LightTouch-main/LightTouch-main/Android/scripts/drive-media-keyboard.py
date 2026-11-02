#!/usr/bin/env python3
"""Plan or drive two inspected scratch-Note keys during an existing media test."""
import argparse
import hashlib
import importlib.util
import io
import json
import re
import shlex
import time
from pathlib import Path

PACKAGE = "org.lighttouch.android"
MONITOR = Path(__file__).with_name("collect-monitor-phase.py")


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load_monitor():
    spec = importlib.util.spec_from_file_location("media_monitor", MONITOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rectangle(value, dimensions):
    require(isinstance(value, list) and len(value) == 4 and all(type(v) is int for v in value), "integer rectangle required")
    x0, y0, x1, y1 = value
    require(0 <= x0 < x1 <= dimensions[0] and 0 <= y0 < y1 <= dimensions[1], "rectangle outside current image")
    return value


def binding_check(b, prior):
    require(b.get("schema") == 1 and b.get("serial") == prior["serial"], "binding schema/serial mismatch")
    require(b.get("scratchNoteApproved") is True and b.get("preservesExistingTestNote") is True,
            "new scratch Note approval required; existing Test must not be edited")
    require(b.get("durationSeconds") in (30, 1200), "only reviewed30/1200-second workloads")
    require(re.fullmatch(r"media-evidence-[0-9]{13}", b.get("mediaFolder", "")), "exact new media folder required")
    require(type(b.get("measurementStartDeviceBootTimeMs")) is int and b["measurementStartDeviceBootTimeMs"] > 0,
            "measurement-start device BOOTTIME required")
    for key in ("mainApkSha256", "testApkSha256", "rawGuestImageSha256", "androidImageSha256"):
        require(re.fullmatch(r"[a-f0-9]{64}", b.get(key, "")), "invalid pin: " + key)
    require(b["mainApkSha256"] == prior["spec"]["artifactPins"]["mainApkSha256"], "main binding changed")
    require(b.get("recordId") == prior["spec"]["recordId"], "record binding changed")
    require(isinstance(b.get("scratchNoteIdentity"), str) and 0 < len(b["scratchNoteIdentity"]) <= 128, "scratch identity required")
    size = b.get("androidDimensions")
    require(isinstance(size, list) and len(size) == 2 and all(type(v) is int and v > 0 for v in size)
            and size[0] * size[1] <= 8388608, "bounded display dimensions required")
    view = rectangle(b.get("guestViewportBounds"), size)
    for key in ("letterKeyRegion", "backspaceKeyRegion"):
        rect = rectangle(b.get(key), size)
        require(view[0] < rect[0] < rect[2] < view[2] and view[1] < rect[1] < rect[3] < view[3], "key must be strictly inside inspected viewport")
    a, z = b["letterKeyRegion"], b["backspaceKeyRegion"]
    require(a[2] <= z[0] or z[2] <= a[0] or a[3] <= z[1] or z[3] <= a[1], "nonoverlapping keys required")
    rois = b.get("stableKeyboardRegions")
    require(isinstance(rois, list) and 2 <= len(rois) <= 8, "two to eight independently inspected keyboard anchors required")
    for roi in rois:
        rectangle(roi, size)
    require(sum((r[2]-r[0])*(r[3]-r[1]) for r in rois) <= 524288, "anchor decoding exceeds bound")
    require(type(b.get("surfaceGeneration")) is int and b["surfaceGeneration"] > 0, "Surface generation required")
    require(type(b.get("inputCancellationRequests")) is int and b["inputCancellationRequests"] >= 0, "cancellation baseline required")
    return b


def latest_sample(data):
    require(len(data) <= 65536 and data.endswith(b"\n"), "partial/oversized media trace; stop without retry")
    lines = data.splitlines()
    require(lines, "media trace absent")
    # tail may start inside an old line; only complete final line is consumed.
    value = json.loads(lines[-1])
    require(isinstance(value, dict), "invalid media sample")
    return value


PAIR_MIN_GAP_MS = 2000
PAIR_MAX_GAP_MS = 5500
PAIR_RESERVE_MS = 5000


class Fence:
    """Pure decision state. No host timestamp is compared with device BOOTTIME."""
    def __init__(self, b, prior):
        self.b, self.prior = b, prior
        self.cutoff = b["measurementStartDeviceBootTimeMs"] + b["durationSeconds"] * 1000 - 3000
        self.maximum_pairs = b["durationSeconds"] // 2
        self.last_pair_ms = None
        self.last_sample_ms = 0
        self.pairs = 0
        self.failed = False
        self.last_owned_tap = None

    def owned_tap_completed(self, key, clock):
        require(key in ("letterKeyRegion", "backspaceKeyRegion"), "unknown owned tap")
        require(clock["upperMs"] < self.cutoff, "owned tap crossed cutoff")
        self.last_owned_tap = {"key": key, "commandReturnedDeviceBootTimeUpperMs": clock["upperMs"]}

    def sample(self, s, clock):
        require(not self.failed, "controller already retired")
        require(s.get("label") in ("running", "measurement-start"), "measurement phase ended or changed")
        at = s.get("hostElapsedMs")  # Android SystemClock.elapsedRealtime despite legacy name.
        require(type(at) is int and self.last_sample_ms <= at and at >= self.b["measurementStartDeviceBootTimeMs"], "trace time regressed/old run")
        require(at <= clock["upperMs"] and clock["upperMs"] - at <= 2500, "media status not fresh")
        status = s["status"]; expected = self.prior["spec"]
        require(status.get("pid") == expected["workerPid"] and status.get("uid") == expected["workerUid"], "worker changed")
        require(status.get("state") == "running" and status.get("error") == "", "worker not healthy/running")
        require(s.get("surfaceValid") is True and s.get("viewFocused") is True, "Surface/focus changed")
        obs, inp = status["observations"], s["androidInput"]
        require(obs.get("surfaceGeneration") == self.b["surfaceGeneration"], "viewport/Surface generation changed")
        require(inp.get("cancellationRequests") == self.b["inputCancellationRequests"], "input ownership was cancelled")
        require(inp.get("rejectedBatches") == 0 and not inp.get("androidContactIds"), "rejected or held external input")
        self.last_sample_ms = at

    def start_pair(self, clock):
        require(self.pairs < self.maximum_pairs, "pair bound reached")
        require(clock["upperMs"] + PAIR_RESERVE_MS < self.cutoff, "quiet tail or insufficient pair budget")
        if self.last_pair_ms is not None:
            gap = clock["lowerMs"] - self.last_pair_ms
            require(PAIR_MIN_GAP_MS <= gap <= PAIR_MAX_GAP_MS, "missed cadence; never catch up")
        self.last_pair_ms = clock["lowerMs"]

    def finish_pair(self, clock):
        require(clock["upperMs"] < self.cutoff, "input crossed quiet cutoff; remote completion uncertain")
        self.pairs += 1


class PostTapSampleFence:
    """A later status read must begin after our known input command returned."""
    def __init__(self, owned_tap, first_clock, cutoff_ms):
        self.owned_tap = owned_tap
        self.first_clock = first_clock
        # This deadline is fixed once per guard, never renewed by polling.
        self.deadline_ms = min(first_clock["upperMs"] + 2000, cutoff_ms - 500)

    def ready(self, sample, clock):
        require(clock["lowerMs"] >= self.first_clock["lowerMs"], "sample-wait clock regressed")
        require(clock["upperMs"] <= self.deadline_ms, "post-owned-tap sample wait expired or reached cutoff")
        require(sample.get("label") in ("running", "measurement-start"), "measurement phase ended or changed")
        if self.owned_tap is None:
            return True
        begin, end = sample.get("readBeginElapsedNs"), sample.get("readEndElapsedNs")
        require(type(begin) is int and type(end) is int and 0 < begin <= end,
                "bounded media status read timestamps required after owned tap")
        require(end < (clock["upperMs"] + 1) * 1000000, "media status read is in the future")
        return begin > self.owned_tap["commandReturnedDeviceBootTimeUpperMs"] * 1000000

    def receipt(self, sample, clock):
        return {"ownedTap": self.owned_tap, "waitStartedDeviceClock": self.first_clock,
                "waitDeadlineDeviceBootTimeMs": self.deadline_ms, "observedDeviceClock": clock,
                "sampleReadBeginElapsedNs": sample.get("readBeginElapsedNs"),
                "sampleReadEndElapsedNs": sample.get("readEndElapsedNs"),
                "sampleLabel": sample.get("label"), "guestUpConsumptionInferred": False}


def image_regions(data, b):
    from PIL import Image
    require(len(data) <= 8388608, "Android PNG exceeds bound")
    with Image.open(io.BytesIO(data)) as image:
        require(image.format == "PNG" and list(image.size) == b["androidDimensions"], "Android image/viewport dimensions changed")
        return [sha(image.crop(tuple(roi)).convert("RGB").tobytes()) for roi in b["stableKeyboardRegions"]]


def collect_initial_evidence(identity, helper, collector, spec):
    # Both collectors read the selected record. Their exclusive-write receipt
    # namespaces must differ; otherwise execution aborts before the first tap.
    identity("initial-identity")
    helper.record_metadata(collector, spec, "initial-record")


def focused_own_window(data):
    """Read the top-focused display from a bounded full WindowManager dump."""
    require(len(data) <= 131072, "window focus dump exceeds bound")
    text = data.decode("utf-8", errors="strict")
    top = re.findall(r"(?m)^\s*mTopFocusedDisplayId=(\d+)\s*$", text)
    require(len(top) == 1, "unique top-focused display unavailable")
    current = None
    focused = {}
    for line in text.splitlines():
        display = re.match(r"\s*Display: mDisplayId=(\d+)(?:\s|$)", line)
        if display:
            current = display[1]
        focus = re.match(r"\s*mCurrentFocus=(.*)$", line)
        if focus:
            require(current is not None and current not in focused, "ambiguous display focus")
            focused[current] = focus[1]
    value = focused.get(top[0], "")
    require(re.fullmatch(r"Window\{[0-9a-f]+ u\d+ org\.lighttouch\.android/(?:org\.lighttouch\.android\.)?\.?MainActivity\}", value) is not None,
            "top-focused display is not own MainActivity")
    return {"topFocusedDisplayId": int(top[0]), "window": value}


def tap_script(pid, start, cutoff, x, y, folder):
    # Values come only from validated integer identities/rectangles and the
    # closed media-folder pattern. Android input stays at shell UID; metadata
    # reads use the owning app UID, with no privileged executable or settings.
    return (f"s=$(run-as {PACKAGE} cat /proc/{pid}/stat) || exit 20; "
            "s=${s##*) }; set -- $s; "
            f"test \"${{20}}\" = {start} || exit 21; "
            f"u=$(cut -d. -f1 /proc/uptime); test $u -lt {(cutoff - 500) // 1000} || exit 22; "
            "l=$(run-as " + PACKAGE + " tail -n 1 " + shlex.quote("files/" + folder + "/observations.jsonl") + ") || exit 23; "
            "case \"$l\" in *'\"label\":\"running\"'*|*'\"label\":\"measurement-start\"'*) ;; *) exit 24;; esac; "
            f"input touchscreen tap {x} {y}")


def run(args):
    b = json.loads(args.keyboard.read_text()); prior = json.loads(args.binding.read_text())
    monitor = load_monitor(); helper = monitor.load_collector(); helper.validate_spec(prior["spec"])
    require(prior["package"] == PACKAGE and prior["serial"] == args.serial, "target binding mismatch")
    binding_check(b, prior)
    android = args.android_image.read_bytes(); raw = args.raw_guest_image.read_bytes()
    require(sha(android) == b["androidImageSha256"] and sha(raw) == b["rawGuestImageSha256"], "approved image pin mismatch")
    anchors = image_regions(android, b)
    plan = {"status": "PLAN_ONLY_NO_ADB", "keyboardBindingSha256": sha(args.keyboard.read_bytes()),
            "identityBindingSha256": sha(args.binding.read_bytes()), "scriptSha256": sha(Path(__file__).read_bytes()),
            "durationSeconds": b["durationSeconds"], "cutoffDeviceBootTimeMs": Fence(b, prior).cutoff,
            "pairStartGapMinMs": PAIR_MIN_GAP_MS, "pairStartGapMaxMs": PAIR_MAX_GAP_MS,
            "pairStartReserveMs": PAIR_RESERVE_MS, "postTapSampleWaitMaxMs": 2000,
            "allowedActions": ["letter touchscreen tap", "backspace touchscreen tap"],
            "stableKeyboardRegionRgbSha256": anchors, "automaticStartResumeStopRecovery": False,
            "atomicImageInputBindingClaimed": False, "guestAcceptance": False, "audiblePlaybackVerified": False}
    if not args.execute:
        print(json.dumps(plan, indent=2)); return
    out = args.out.resolve()
    require(not any((p / ".git").exists() for p in (out, *out.parents)), "private output must be outside Git")
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    helper.write_json(out / "plan.json", plan)
    c = helper.Collector(out, args.serial, PACKAGE, adb=args.adb, budget=b["durationSeconds"] + 45)
    pid, uid, start = prior["spec"]["workerPid"], prior["spec"]["workerUid"], prior["workerStartTicks"]
    fence = Fence(b, prior); serial = 0; unmatched_letter = False

    def identity(label):
        require(c.boot(label + "-boot") == prior["bootId"], "boot identity changed")
        monitor.worker_identity(c.file(label + "-status", f"/proc/{pid}/status").decode("ascii"),
                                c.file(label + "-cmdline", f"/proc/{pid}/cmdline"), pid, uid)
        monitor.worker_stat(c.file(label + "-stat", f"/proc/{pid}/stat"), pid, start)
        require(c.file(label + "-selected", "files/library/selected").strip().decode() == b["recordId"], "selected record changed")

    def check_apk(package, expected, label):
        paths = c.read(label + "-path", "pm path " + package, app=False)[0].decode().splitlines()
        require(len(paths) == 1 and paths[0].startswith("package:/") and paths[0].endswith("/base.apk"), "single expected base APK required")
        require(c.read(label + "-hash", "sha256sum " + shlex.quote(paths[0][8:]))[0].split()[0].decode() == expected, "installed APK changed")

    def guard(label):
        identity(label)
        sample_fence = None
        for attempt in range(21):
            suffix = "" if attempt == 0 else f"-wait-{attempt:02d}"
            trace = c.read(label + "-trace" + suffix, "tail -c 65536 " + shlex.quote("files/" + b["mediaFolder"] + "/observations.jsonl"))[0]
            s = latest_sample(trace); clock = c.clock(label + "-clock" + suffix)
            if sample_fence is None:
                sample_fence = PostTapSampleFence(fence.last_owned_tap, clock, fence.cutoff)
            helper.write_json(out / (label + f"-sample-fence-{attempt:02d}.json"), sample_fence.receipt(s, clock))
            if sample_fence.ready(s, clock):
                break
            require(attempt < 20, "post-owned-tap sample poll bound reached")
            time.sleep(.1)
        # Empty IDs, generation, ownership, errors and all other existing checks
        # are required on this entirely post-command read; none are waived.
        fence.sample(s, clock)
        # SDK36 omits display focus from `window windows`; full output also
        # includes inactive virtual displays, whose null focus is not current.
        focus = c.read(label + "-focus", "dumpsys window | head -c 131073", app=False)[0]
        helper.write_json(out / (label + "-focused-window.json"), focused_own_window(focus))
        png = c.read(label + "-android", "screencap -p | head -c 8388609", app=False)[0]
        actual_anchors = image_regions(png, b)
        helper.write_json(out / (label + "-roi.json"), {"actualRgbSha256": actual_anchors,
                          "expectedRgbSha256": anchors, "matches": actual_anchors == anchors,
                          "androidPngSha256": sha(png)})
        require(actual_anchors == anchors, "actual keyboard ROI changed; no fallback coordinates")
        # Keep real Android checkpoint images every15 pairs (~30s), plus the
        # first pair. Keep the mismatching stdout on failure; successful guard
        # payloads between checkpoints retain hashes instead of gigabytes of PNGs.
        if label.endswith("-letter") and (serial == 1 or (serial - 1) % 15 == 0):
            (out / (label + "-android.png")).write_bytes(png)
        (out / (label + "-android.stdout")).unlink()
        after = c.clock(label + "-after-image-clock")
        require(after["upperMs"] - clock["lowerMs"] <= 1000, "image/clock bracket too old")
        fence.sample(s, after)
        return after

    try:
        require(c.read("shell-uid", "id -u", app=False)[0].strip() not in (b"0", b""), "nonroot shell required")
        check_apk(PACKAGE, b["mainApkSha256"], "main"); check_apk(PACKAGE + ".test", b["testApkSha256"], "test")
        collect_initial_evidence(identity, helper, c, prior["spec"])
        initial_clock = c.clock("approval-clock")
        require(0 <= initial_clock["upperMs"] - b["measurementStartDeviceBootTimeMs"] <= 15000, "approval/measurement start is not fresh")
        while True:
            serial += 1; label = f"pair-{serial:03d}"
            clock = c.clock(label + "-ready")
            if clock["upperMs"] + PAIR_RESERVE_MS >= fence.cutoff or fence.pairs == fence.maximum_pairs:
                break
            if fence.last_pair_ms is not None:
                wait_ms = fence.last_pair_ms + PAIR_MIN_GAP_MS - clock["lowerMs"]
                if wait_ms > 0: time.sleep(wait_ms / 1000)
            clock = guard(label + "-letter"); fence.start_pair(clock)
            for key in ("letterKeyRegion", "backspaceKeyRegion"):
                if key == "backspaceKeyRegion":
                    time.sleep(.15); clock = guard(label + "-backspace")
                require(clock["upperMs"] + 500 < fence.cutoff, "no time for a balanced input tap")
                x0, y0, x1, y1 = b[key]; x, y = (x0 + x1) // 2, (y0 + y1) // 2
                # Recheck device BOOTTIME and process start token on the target immediately
                # before one Android input tap; host timeouts do not cancel remote gestures.
                script = tap_script(pid, start, fence.cutoff, x, y, b["mediaFolder"])
                # Constrain this blocking call to the remaining quiet-window budget.
                old_deadline = c.deadline
                c.deadline = min(old_deadline, time.monotonic() + min(2, (fence.cutoff-clock["upperMs"])/1000))
                if key == "letterKeyRegion": unmatched_letter = True
                try: c.read(label + "-" + key + "-tap", script, app=False)
                finally: c.deadline = old_deadline
                clock = c.clock(label + "-" + key + "-completed")
                require(clock["upperMs"] < fence.cutoff, "tap completion exceeded cutoff")
                fence.owned_tap_completed(key, clock)
                if key == "backspaceKeyRegion": unmatched_letter = False
            fence.finish_pair(clock)
            helper.write_json(out / (label + ".json"), {"completedPairs": fence.pairs, "deviceBootTimeMs": clock,
                              "unmatchedLetterPossible": unmatched_letter, "guestConsumptionInferred": False})
        require(fence.pairs > 0, "no workload pairs completed")
        helper.write_json(out / "result.json", {**plan, "status": "KEYBOARD_ACTIONS_FINISHED_NOT_GUEST_PASS",
                          "completedPairs": fence.pairs, "unmatchedLetterPossible": unmatched_letter,
                          "measurementResultsRequireIndependentReview": True})
    except Exception as error:
        fence.failed = True
        helper.write_json(out / "failure.json", {**plan, "status": "ABORTED_PARTIAL_EVIDENCE_RETAINED",
                          "error": str(error), "completedPairs": fence.pairs,
                          "unmatchedLetterPossible": unmatched_letter, "automaticCleanupInput": False})
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--binding", type=Path, required=True, help="fresh strict collector before.json")
    parser.add_argument("--keyboard", type=Path, required=True, help="operator-reviewed new scratch keyboard binding")
    parser.add_argument("--android-image", type=Path, required=True)
    parser.add_argument("--raw-guest-image", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--execute", action="store_true", help="operator explicitly authorizes two inspected real keyboard taps")
    run(parser.parse_args())
