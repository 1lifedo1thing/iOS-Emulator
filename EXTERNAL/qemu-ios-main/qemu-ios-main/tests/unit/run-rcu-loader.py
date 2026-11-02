#!/usr/bin/env python3
"""Run shared-entry RCU ownership tests using a configured native QEMU build."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    build, output = args.build.resolve(), args.output.resolve()
    source = Path(__file__).resolve().parents[2]
    output.mkdir(parents=True, exist_ok=False)
    commands = json.loads((build / "compile_commands.json").read_text())
    command = next(c for c in commands if c["file"].endswith("/util/rcu.c"))
    raw = command.get("arguments") or shlex.split(command["command"])
    base = []
    index = 0
    while index < len(raw):
        flag = raw[index]
        if flag in ("-o", "-c", "-MF", "-MQ", "-MT"):
            index += 2
            continue
        if flag not in ("-MD", "-MMD", "-fno-pie", "-no-pie"):
            base.append(flag)
        index += 1
    base += ["-Werror", "-fPIC", "-I" + str(source / "contrib/ios-app")]
    compiler = base[0]
    libs = shlex.split(subprocess.check_output(
        ["pkg-config", "--libs", "glib-2.0", "gthread-2.0"], text=True))
    results = []

    def run(name, argv):
        started = time.monotonic()
        result = subprocess.run(argv, cwd=command["directory"],
                                capture_output=True, text=True, timeout=25)
        (output / (name + ".log")).write_text(result.stdout + result.stderr)
        results.append(dict(name=name, argv=argv, cwd=command["directory"],
                            exit=result.returncode,
                            wall_s=time.monotonic() - started))
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(name, result.returncode, flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)

    def compile_file(name, path, extra=()):
        obj = output / (name + ".o")
        run(name, base + list(extra) + ["-c", str(source / path), "-o", str(obj)])
        return str(obj)

    rcu = compile_file("rcu", "util/rcu.c", ["-finstrument-functions"])
    bql = compile_file("bql", "stubs/iothread-lock.c")
    driver = output / "driver"
    run("driver", [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-g", "-pthread",
                   str(source / "tests/unit/test-rcu-loader.c"), "-ldl", "-o", str(driver)])
    for mode in ("standalone", "embedded"):
        fixture = compile_file(mode, "tests/unit/rcu-loader-library.c",
                               ["-DRCU_TEST_EMBEDDED"] if mode == "embedded" else [])
        objects = [rcu, fixture, bql]
        if mode == "embedded":
            objects.append(compile_file("entry", "contrib/ios-app/qemu-ios-entry.c"))
        library = output / (mode + ".so")
        run(mode + "-link", [compiler, "-shared", "-Wl,-z,defs",
                             "-Wl,-Bsymbolic-functions", "-o", str(library)] +
            objects + [str(build / "libqemuutil.a")] + libs + ["-ldl"])
        run(mode + "-test", [str(driver), str(library),
                             "1" if mode == "standalone" else "0",
                             "2" if mode == "standalone" else "1"])


if __name__ == "__main__":
    main()
