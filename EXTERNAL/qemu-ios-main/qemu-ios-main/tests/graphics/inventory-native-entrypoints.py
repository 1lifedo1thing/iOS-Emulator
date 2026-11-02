#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Inventory driver calls after native-ES preprocessing; no link or GL execution."""
import argparse
import json
import pathlib
import re
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-commands", required=True, type=pathlib.Path)
    parser.add_argument("--renderer", required=True, type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--check", type=pathlib.Path)
    args = parser.parse_args()
    renderer = args.renderer.resolve()
    entries = json.loads(args.compile_commands.read_text())
    entry = next(item for item in entries if pathlib.Path(item["file"]).name == renderer.name)
    command = entry.get("arguments") or shlex.split(entry["command"])
    directory = pathlib.Path(entry["directory"])
    old_source = pathlib.Path(entry["file"])
    if not old_source.is_absolute():
        old_source = directory / old_source
    filtered = [command[0]]
    skip_value = False
    for item in command[1:]:
        if skip_value:
            skip_value = False
            continue
        if item in ("-o", "-MF", "-MT", "-MQ"):
            skip_value = True
            continue
        if item in ("-c", "-MD", "-MMD", "-MP"):
            continue
        if not item.startswith("-") and (directory / item).resolve() == old_source.resolve():
            continue
        filtered.append(item)
    filtered += ["-DGLES_HOST_EGL=1", "-DGLES_HOST_EGL_ES=1", "-E", str(renderer)]
    processed = subprocess.check_output(filtered, cwd=directory, text=True)
    driver_source = []
    selected = False
    for line in processed.splitlines():
        marker = re.match(r'^#\s+\d+\s+"([^"]+)"', line)
        if marker:
            name = pathlib.Path(marker.group(1)).name
            selected = name.startswith("gles-host") and name.endswith((".c", ".inc"))
        elif selected:
            driver_source.append(line)
    source = "\n".join(driver_source)
    names = set(re.findall(r'\b((?:egl|gl)[A-Z]\w*)\s*\(', source))
    revision = subprocess.check_output(
        ["git", "-C", str(renderer.parent), "rev-parse", "HEAD"], text=True
    ).strip()
    inventory = {
        "description": "Raw public driver calls in preprocessed GLES_HOST_EGL + GLES_HOST_EGL_ES renderer; dynamic OES entrypoints resolve through the same EGL provider. Inventory for planned ANGLE dispatch, not an implemented backend.",
        "source_base": revision,
        "egl": sorted(name for name in names if name.startswith("egl")),
        "gl": sorted(name for name in names if name.startswith("gl")),
        "dynamic_gl_extensions": sorted(set(re.findall(
            r'eglGetProcAddress\s*\(\s*"(gl\w+)"\s*\)', source))),
    }
    if args.check:
        expected = json.loads(args.check.read_text())
        for group in ("egl", "gl", "dynamic_gl_extensions"):
            if inventory[group] != expected[group]:
                parser.exit(1, f"FAIL {group} changed; regenerate the dispatch inventory\n")
    if args.output:
        args.output.write_text(json.dumps(inventory, indent=2) + "\n")
    print("PASS native call inventory: " + ", ".join(
        f"{len(inventory[group])} {group}" for group in ("egl", "gl", "dynamic_gl_extensions")))


if __name__ == "__main__":
    main()
