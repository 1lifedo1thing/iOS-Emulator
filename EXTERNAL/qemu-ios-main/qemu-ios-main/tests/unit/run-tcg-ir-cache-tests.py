#!/usr/bin/env python3
"""Light source tests using an existing configured QEMU ARM system build.

Does not invoke Ninja, configure, the engine, a guest or a device. The codec
harness uses actual target/TCG headers and only replaces infrastructure allocation
and QOM-name lookup; its integer oracle is a unit check, never guest acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    commands = json.loads(args.compile_commands.read_text())
    item = next(x for x in commands
                if x["file"].endswith("/target/arm/tcg/translate.c"))
    old_relative = item["file"][:-len("/target/arm/tcg/translate.c")]
    old_absolute = str((Path(item["directory"]) / old_relative).resolve())
    base, skip = [], False
    for word in shlex.split(item["command"]):
        if skip:
            skip = False
            continue
        if word in ("-MQ", "-MF", "-o"):
            skip = True
            continue
        if word in ("-MD", "-c") or word == item["file"]:
            continue
        word = word.replace(old_absolute, str(source))
        word = word.replace(old_relative, str(source))
        # Some captured compdbs retain shell quotes around -D arguments.
        if word.startswith("'") and word.endswith("'"):
            word = word[1:-1]
        base.append(word)
    records = []

    def run(label, command, cwd=item["directory"]):
        result = subprocess.run(command, cwd=cwd, text=True,
                                capture_output=True, timeout=45)
        (output / (label + ".stdout")).write_text(result.stdout)
        (output / (label + ".stderr")).write_text(result.stderr)
        records.append({"label": label, "command": command, "cwd": cwd,
                        "exitCode": result.returncode})
        (output / "commands.json").write_text(json.dumps(records, indent=2))
        if result.returncode:
            raise RuntimeError(label + " failed; inspect retained stderr")
        return result

    for filename in ("target/arm/tcg/ir-cache.c", "target/arm/tcg/translate.c",
                     "tcg/ir-cache-store.c", "contrib/ios-app/qemu-ios-entry.c"):
        run("syntax-" + Path(filename).stem,
            base + ["-fsyntax-only", str(source / filename)])
    libs = shlex.split(subprocess.check_output(
        ["pkg-config", "--libs", "glib-2.0"], text=True))
    for name, test in (("store", "test-tcg-ir-cache-store.c"),
                       ("codec", "test-arm-tcg-ir-cache.c"),
                       ("diagnostics", "test-tcg-ir-cache-diagnostics.c")):
        extra = ["-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
        if args.sanitize:
            extra += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        sources = [str(source / "tests/unit" / test)]
        if name != "diagnostics":
            sources.append(str(source / "tcg/ir-cache-store.c"))
        run(name + "-compile", base + extra + ["-o", str(output / name)] +
            sources + libs + ["-ldl"])
        run(name + "-tests", [str(output / name)])
    with tempfile.TemporaryDirectory(prefix="lighttouch-ir-process-") as d:
        Path(d).chmod(0o700)
        run("fresh-producer", [str(output / "store"), "--produce", d])
        run("fresh-consumer", [str(output / "store"), "--consume", d])
    with tempfile.TemporaryDirectory(prefix="lighttouch-ir-hook-process-") as d:
        Path(d).chmod(0o700)
        run("disabled-by-default", [str(output / "codec"), "--disabled", d])
        if list(Path(d).iterdir()):
            raise RuntimeError("Disabled cache created files")
        run("diagnostics-only-disabled", [str(output / "codec"),
                                          "--diagnostics-only", d])
        if list(Path(d).iterdir()):
            raise RuntimeError("Diagnostics alone created cache files")
        run("fresh-ir-producer", [str(output / "codec"), "--produce-ir", d])
        run("fresh-ir-consumer", [str(output / "codec"), "--consume-ir", d])
    with tempfile.TemporaryDirectory(prefix="lighttouch-ir-diagnostics-") as d:
        Path(d).chmod(0o700)
        result = run("aggregate-ir-diagnostics", [str(output / "codec"),
                                                  "--diagnostics-ir", d])
        lines = [line for line in result.stderr.splitlines()
                 if line.startswith("LightTouch TCG IR cache diagnostics:")]
        if not 1 <= len(lines) <= 3:
            raise RuntimeError("Missing or unbounded aggregate reports")
        values = dict((k, int(v)) for k, v in re.findall(
            r"([a-z0-9_]+)=([0-9]+)", lines[-1]))
        expected = dict(version=1, arm_attempts=7, a32_attempts=5,
                        thumb_attempts=1, a64_attempts=1, key_eligible=4,
                        key_rejected=3, save_preflight_skips=1,
                        save_key_changed=1, capture_rejected=1,
                        validation_rejected=0, store_failed=0, records=1,
                        put_calls=1, put_success=1, file_fsync_calls=1,
                        directory_fsync_calls=1, capacity_observed=0)
        if any(values.get(k) != v for k, v in expected.items()):
            raise RuntimeError("Aggregate report counters do not match calls")
        legacy = ("LightTouch TCG IR cache: hits=1 misses=3 rejected=0 stored=1; "
                  "frontend IR only, fresh native code")
        if legacy not in result.stderr.splitlines():
            raise RuntimeError("Legacy cache summary changed")
        terminal = [line for line in result.stderr.splitlines()
                    if line.startswith("LightTouch TCG IR cache terminal:")]
        if len(terminal) != 1 or "reason=process-exit;" not in terminal[0]:
            raise RuntimeError("Missing process-exit fallback identity")
    for mode in ("--terminal-ir", "--terminal-kill-ir"):
        with tempfile.TemporaryDirectory(prefix="lighttouch-ir-terminal-") as d:
            Path(d).chmod(0o700)
            result = run(mode[2:], [str(output / "codec"), mode, d])
            lines = result.stderr.splitlines()
            terminal = [line for line in lines
                        if line.startswith("LightTouch TCG IR cache terminal:")]
            if len(terminal) != 1 or "reason=post-qemu-cleanup;" not in terminal[0]:
                raise RuntimeError("Missing or duplicated post-cleanup totals")
            # The aggregate fixture deliberately ends between power-of-two
            # milestones. Full final totals must survive a real _exit as well
            # as subsequent atexit; the earlier milestone is insufficient.
            aggregates = [line for line in lines if line.startswith(
                "LightTouch TCG IR cache diagnostics:")]
            values = dict((k, int(v)) for k, v in re.findall(
                r"([a-z0-9_]+)=([0-9]+)", aggregates[-1]))
            if len(aggregates) != 3 or any(values.get(k) != v
                                           for k, v in expected.items()):
                raise RuntimeError("Terminal aggregate totals are incomplete")
            if lines[-1] != terminal[0] or legacy not in lines:
                raise RuntimeError("Terminal marker must follow final counters")
    for label in ("disabled-by-default", "diagnostics-only-disabled",
                  "fresh-ir-producer", "fresh-ir-consumer"):
        if "cache diagnostics:" in (output / (label + ".stderr")).read_text():
            raise RuntimeError("Unrequested aggregate diagnostics emitted")
    for label in ("disabled-by-default", "diagnostics-only-disabled"):
        if "LightTouch TCG IR cache" in (output / (label + ".stderr")).read_text():
            raise RuntimeError("Disabled terminal hook emitted cache output")
    files = ("target/arm/tcg/ir-cache.c", "target/arm/tcg/ir-cache.h",
             "include/tcg/ir-cache-lifecycle.h",
             "contrib/ios-app/qemu-ios-entry.c",
             "include/tcg/ir-cache-store.h", "tcg/ir-cache-store.c",
             "tests/unit/test-arm-tcg-ir-cache.c",
             "tests/unit/test-tcg-ir-cache-store.c",
             "tests/unit/test-tcg-ir-cache-diagnostics.c",
             "tests/unit/run-tcg-ir-cache-tests.py")
    result = {"scope": "source syntax and isolated codec/store units only",
              "guestAcceptance": False, "performanceQualified": False,
              "sanitizers": args.sanitize, "result": "PASS",
              "files": {f: hashlib.sha256((source / f).read_bytes()).hexdigest()
                        for f in files}, "commandCount": len(records)}
    (output / "RESULT.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
