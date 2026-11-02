#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Inventory installed Pixel debug prerequisites; print a plan without creating or launching an AVD."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def probe(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=30)
    return {"argv": [str(part) for part in argv], "exit": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--private-root", type=Path, required=True)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--apk-sha256", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{64}", args.apk_sha256):
        parser.error("Expected APK SHA-256 must be 64 lowercase hex digits")
    sdk, root, apk = (p.resolve() for p in (args.sdk, args.private_root, args.apk))
    # Planning paths must never target the source checkout or a parent of it.
    checkout = Path(__file__).resolve().parents[2]
    if root == checkout or root in checkout.parents or checkout in root.parents:
        parser.error("Private AVD root must be outside this Git checkout")
    # Other checkouts/worktrees are also source repositories. A linked worktree
    # uses a .git file, so do not restrict this check to directories.
    if any((ancestor / ".git").exists() for ancestor in (root, *root.parents)):
        parser.error("Private AVD root must be outside every Git checkout or worktree")
    if not apk.is_file() or sha256(apk) != args.apk_sha256:
        parser.error("Frozen APK is absent or differs from the supplied SHA-256")
    avdmanager = sdk / "cmdline-tools/latest/bin/avdmanager"
    emulator = sdk / "emulator/emulator"
    tools = {"adb": sdk / "platform-tools/adb", "emulator": emulator,
             "avdmanager": avdmanager, "sdkmanager": sdk / "cmdline-tools/latest/bin/sdkmanager",
             "apksigner": sdk / "build-tools/35.0.0/apksigner",
             "zipalign": sdk / "build-tools/35.0.0/zipalign",
             "lldb": sdk / "ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/lldb",
             "simpleperf-x86_64": sdk / "ndk/28.2.13676358/simpleperf/bin/android/x86_64/simpleperf",
             "simpleperf-arm64": sdk / "ndk/28.2.13676358/simpleperf/bin/android/arm64/simpleperf"}
    missing = [name for name, path in tools.items() if not path.is_file() or not os.access(path, os.X_OK)]
    if missing:
        parser.error("Missing executable prerequisites: " + ", ".join(missing))
    devices = probe([str(avdmanager), "list", "device"])
    acceleration = probe([str(emulator), "-accel-check"])
    version = probe([str(emulator), "-version"])
    if devices["exit"] or acceleration["exit"] or version["exit"]:
        parser.error("Read-only SDK probes failed; prerequisites are not ready")
    installed_profiles = set(re.findall(r'id: \d+ or "([^"]+)"', devices["stdout"]))
    plans = []
    for profile in ("pixel_8", "pixel_9", "pixel_10"):
        for api, tag, pages in ((35, "default", 4096), (36, "google_apis_ps16k", 16384)):
            image = sdk / "system-images" / f"android-{api}" / tag / "x86_64"
            properties = image / "source.properties"
            name = f"LightTouch_{profile}_API{api}_{pages}"
            path = root / f"{name}.avd"
            plans.append({"name": name, "profile": profile, "abi": "x86_64", "api": api,
                "expectedPageBytes": pages, "actualPageBytes": None,
                "profileInstalled": profile in installed_profiles,
                "imageInstalled": properties.is_file(),
                "imageProperties": properties.read_text() if properties.is_file() else None,
                "imagePropertiesSha256": sha256(properties) if properties.is_file() else None,
                "privateAvdPath": str(path), "pathAlreadyExists": path.exists(),
                "createArgv": [str(avdmanager), "create", "avd", "--name", name,
                    "--package", f"system-images;android-{api};{tag};x86_64",
                    "--device", profile, "--path", str(path)],
                "createStdin": "no\n", "creationRequiresRootResourceGrant": True,
                "launchRequiresRootResourceGrant": True, "guestAcceptancePassed": False})
    ready = all(p["profileInstalled"] and p["imageInstalled"] and not p["pathAlreadyExists"] for p in plans)
    print(json.dumps({"schema": 1, "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "mode": "read-only-plan", "apk": {"path": str(apk), "sha256": args.apk_sha256},
        "tools": {name: {"path": str(path), "sha256": sha256(path)} for name, path in tools.items()},
        "probes": {"deviceDefinitions": devices, "acceleration": acceleration, "emulatorVersion": version},
        "prerequisitesReadyForNewPlans": ready, "plans": plans,
        "createdAvds": [], "launchedAvds": [], "phoneAccessed": False,
        "physicalPixelEquivalent": False, "guestAcceptancePassed": False,
        "limitations": ["Pixel profiles configure virtual geometry and features; this uses x86_64, not Tensor ARM64.",
            "Virtual graphics do not reproduce the physical Pixel GPU/driver.",
            "Expected page size must be measured with target getconf PAGESIZE after launch.",
            "A working virtual guest cannot qualify a physical Pixel 8, 9, or 10."]}, indent=2))
    return 0 if ready else 1


if __name__ == "__main__":
    raise SystemExit(main())
