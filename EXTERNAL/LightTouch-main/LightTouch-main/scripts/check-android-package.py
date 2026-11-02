#!/usr/bin/env python3
"""Check every packaged native library; runtime guest acceptance is separate."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile


MACHINES = {"arm64-v8a": 183, "x86_64": 62}
SYSTEM_LIBRARIES = {
    "libc.so", "libdl.so", "libm.so", "liblog.so", "libandroid.so",
    "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",
    "libOpenSLES.so", "libaaudio.so", "libnativewindow.so", "libjnigraphics.so",
    "libmediandk.so", "libvulkan.so", "libz.so",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def inspect_elf(data, abi):
    require(len(data) >= 64 and data[:6] == b"\x7fELF\x02\x01", "expected little-endian ELF64")
    header = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    require(header[0] == 3, "expected shared object (ET_DYN)")
    require(header[1] == MACHINES[abi], f"ELF machine does not match {abi}")
    offset, entry_size, count = header[4], header[8], header[9]
    require(entry_size >= 56 and count > 0, "invalid program header table")
    require(offset + entry_size * count <= len(data), "truncated program headers")
    loads = []
    for index in range(count):
        kind, flags, file_offset, address, _, file_size, memory_size, alignment = struct.unpack_from(
            "<IIQQQQQQ", data, offset + index * entry_size
        )
        require(file_offset + file_size <= len(data), "truncated ELF segment")
        if kind == 1:
            require(file_size <= memory_size, "LOAD filesz exceeds memsz")
            require(alignment >= 16384 and alignment & (alignment - 1) == 0,
                    "LOAD alignment must be a power of two >= 16384")
            require(file_offset % 16384 == address % 16384, "LOAD offset/address incongruent for 16 KiB")
            require(flags & 3 != 3, "writable executable LOAD segment")
            loads.append({"alignment": alignment, "offset": file_offset, "address": address})
        if kind == 0x6474E551:
            require(flags & 1 == 0, "executable GNU_STACK")
    require(loads, "missing LOAD segments")
    return loads


def command(args):
    result = subprocess.run(args, text=True, capture_output=True, check=True)
    return result.stdout + result.stderr


def inspect_apk(apk, sdk, build_tools, readelf, expected_abis=None):
    expected_abis = set(MACHINES) if expected_abis is None else set(expected_abis)
    require(expected_abis and expected_abis <= set(MACHINES), "unsupported expected ABI set")
    results = []
    with zipfile.ZipFile(apk) as archive, tempfile.TemporaryDirectory(prefix="lighttouch-elf-") as temporary:
        members = archive.infolist()
        names = [member.filename for member in members]
        require(len(names) == len(set(names)), "duplicate ZIP member names")
        native = [member for member in members if re.fullmatch(r"lib/[^/]+/[^/]+\.so", member.filename)]
        require(native, "APK contains no native libraries")
        packaged = {abi: set() for abi in MACHINES}
        for member in native:
            _, abi, name = member.filename.split("/")
            require(abi in MACHINES, f"unexpected ABI: {abi}")
            packaged[abi].add(name)
        present = {abi for abi, libraries in packaged.items() if libraries}
        require(present == expected_abis, f"required ABI set {sorted(expected_abis)} differs from packaged {sorted(present)}")
        library_sets = [packaged[abi] for abi in sorted(present)]
        require(all(libraries == library_sets[0] for libraries in library_sets), "ABI library sets differ")
        with apk.open("rb") as source:
            for member in native:
                _, abi, name = member.filename.split("/")
                require(member.compress_type == zipfile.ZIP_STORED, f"compressed native library: {member.filename}")
                source.seek(member.header_offset)
                local = source.read(30)
                require(len(local) == 30 and local[:4] == b"PK\x03\x04", "invalid local ZIP header")
                name_size, extra_size = struct.unpack_from("<HH", local, 26)
                data_offset = member.header_offset + 30 + name_size + extra_size
                require(data_offset % 16384 == 0, f"native ZIP data not 16 KiB aligned: {member.filename}")
                data = archive.read(member)
                loads = inspect_elf(data, abi)
                path = Path(temporary) / abi / name
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(data)
                dynamic = command([readelf, "-dW", str(path)])
                versions = command([readelf, "-VW", str(path)])
                require("GLIBC_" not in versions, f"glibc dependency in Android library: {member.filename}")
                require(not re.search(r"\((?:RPATH|RUNPATH)\)", dynamic), f"host RPATH in {member.filename}")
                needed = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", dynamic)
                require(all(library in SYSTEM_LIBRARIES or library in packaged[abi] for library in needed),
                        f"unpackaged native dependency in {member.filename}: {needed}")
                results.append({"path": member.filename, "sha256": hashlib.sha256(data).hexdigest(),
                                "zip_offset": data_offset, "loads": loads, "needed": needed})
    tools = sdk / "build-tools" / build_tools
    alignment = command([str(tools / "zipalign"), "-c", "-P", "16", "4", str(apk)])
    signature = command([str(tools / "apksigner"), "verify", "--verbose", str(apk)])
    return {"schema_version": 1, "apk_sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
            "result": "PASS", "scope": "package-only", "abis": sorted(expected_abis),
            "initial_abi_coverage": "COMPLETE" if expected_abis == set(MACHINES) else "PARTIAL",
            "runtime_guest_acceptance": "NOT_TESTED",
            "libraries": results, "zipalign": alignment.strip(), "signature": signature.strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--sdk", type=Path, default=Path(os.environ.get("ANDROID_HOME", "")))
    parser.add_argument("--build-tools", default="35.0.0")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--abis", default="arm64-v8a,x86_64", help="expected packaged ABIs; default requires both initial targets")
    args = parser.parse_args()
    try:
        report = inspect_apk(args.apk, args.sdk, args.build_tools, args.readelf, args.abis.split(","))
    except (ValueError, OSError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        report = {"schema_version": 1, "result": "FAIL", "scope": "package-only", "error": str(error)}
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end="")
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
