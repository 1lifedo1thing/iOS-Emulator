#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Fetch public sources independently; compile only with the allocated heavy slot.
set -euo pipefail
if [ "${1:-}" = --help ]; then
    echo 'Usage: build-angle-android.sh [--fetch-only|--prepare-only|--build-only] PRIVATE_DIRECTORY [x64|arm64] [gl|vulkan]'
    echo 'Downloads pinned public Android dependencies and builds three Bionic libraries with at most two jobs.'
    echo '--fetch-only requires a new directory, skips hooks and compilation; --build-only resumes that pinned checkout.'
    echo '--prepare-only runs GN without hooks or compilation on an initialized checkout.'
    echo 'ANGLE_PROFILE=no-abseil-perfetto uses a separate output directory and disables those optional dependencies.'
    exit 0
fi
ANGLE_STAGE=all
case "${1:-}" in
    --fetch-only) ANGLE_STAGE=fetch; shift;;
    --prepare-only) ANGLE_STAGE=prepare; shift;;
    --build-only) ANGLE_STAGE=build; shift;;
esac
ANGLE_DEST=${1:?Usage: build-angle-android.sh NEW_PRIVATE_DIRECTORY [x64|arm64] [gl|vulkan]}
ANGLE_CPU=${2:-x64}
ANGLE_BACKEND=${3:-gl}
ANGLE_JOBS=${ANGLE_JOBS:-2}
ANGLE_PROFILE=${ANGLE_PROFILE:-default}
case "$ANGLE_CPU" in x64|arm64) ;; *) echo 'CPU must be x64 or arm64' >&2; exit 2;; esac
case "$ANGLE_BACKEND" in gl|vulkan) ;; *) echo 'Backend must be gl or vulkan' >&2; exit 2;; esac
case "$ANGLE_JOBS" in 1|2) ;; *) echo 'ANGLE_JOBS must be 1 or 2' >&2; exit 2;; esac
case "$ANGLE_PROFILE" in default|no-abseil-perfetto) ;; *) echo 'Unknown ANGLE_PROFILE' >&2; exit 2;; esac
[ "$(uname -s)" = Linux ] || { echo 'Android ANGLE requires Linux' >&2; exit 2; }
if [ "$ANGLE_STAGE" = build ] || [ "$ANGLE_STAGE" = prepare ]; then
    [ -f "$ANGLE_DEST/source-receipt.txt" ] || { echo 'Missing completed pinned fetch receipt' >&2; exit 2; }
else
    [ ! -e "$ANGLE_DEST" ] || { echo 'Use a new private directory' >&2; exit 2; }
fi
ANGLE_NINJA=$(command -v ninja)
[ -x "$ANGLE_NINJA" ] || { echo 'Native Ninja is required' >&2; exit 2; }
mkdir -p "$ANGLE_DEST"
ANGLE_DEST=$(cd "$ANGLE_DEST" && pwd)
fetch_pinned() {
    git init "$1"
    git -C "$1" remote add origin "$2"
    git -C "$1" fetch --depth 1 origin "$3"
    git -C "$1" checkout --detach FETCH_HEAD
    [ "$(git -C "$1" rev-parse HEAD)" = "$3" ]
}
if [ "$ANGLE_STAGE" = all ] || [ "$ANGLE_STAGE" = fetch ]; then
    fetch_pinned "$ANGLE_DEST/angle" https://chromium.googlesource.com/angle/angle 8cd050f07ebd65cce269fc92e36699fc03b20ac3
    fetch_pinned "$ANGLE_DEST/depot_tools" https://chromium.googlesource.com/chromium/tools/depot_tools.git 9b264039190fa270f3fc44779b4e26b43008ba41
fi
[ "$(git -C "$ANGLE_DEST/angle" rev-parse HEAD)" = 8cd050f07ebd65cce269fc92e36699fc03b20ac3 ]
[ "$(git -C "$ANGLE_DEST/depot_tools" rev-parse HEAD)" = 9b264039190fa270f3fc44779b4e26b43008ba41 ]
export PATH="$ANGLE_DEST/depot_tools:$PATH" DEPOT_TOOLS_UPDATE=0
if [ "$ANGLE_STAGE" = all ] || [ "$ANGLE_STAGE" = fetch ]; then
cat > "$ANGLE_DEST/.gclient" <<'GCLIENT'
solutions=[{"name":"angle","url":"https://chromium.googlesource.com/angle/angle","managed":False,"custom_deps":{},"custom_vars":{"checkout_angle_cl_deps":False,"checkout_angle_dawn_deps":False,"checkout_angle_internal":False,"checkout_angle_restricted_traces":False,"checkout_angle_mesa":False,"download_remoteexec_cfg":False}}]
target_os=['android']
GCLIENT
cd "$ANGLE_DEST"
gclient sync --shallow --no-history --nohooks -j"$ANGLE_JOBS"
gclient revinfo --actual > "$ANGLE_DEST/source-receipt.txt"
fi
if [ "$ANGLE_STAGE" = fetch ]; then
    echo "Pinned sources synchronized without hooks or compilation: $ANGLE_DEST"
    exit 0
fi
cd "$ANGLE_DEST"
if [ "$ANGLE_STAGE" != prepare ]; then gclient runhooks; fi
cd angle
# Synchronization must not move the selected frontend revision.
[ "$(git rev-parse HEAD)" = 8cd050f07ebd65cce269fc92e36699fc03b20ac3 ]
ANGLE_OUT="out/lighttouch-android-$ANGLE_CPU-$ANGLE_BACKEND"
if [ "$ANGLE_PROFILE" != default ]; then ANGLE_OUT="$ANGLE_OUT-$ANGLE_PROFILE"; fi
mkdir -p "$ANGLE_OUT"
if [ "$ANGLE_BACKEND" = gl ]; then ANGLE_GL=true; ANGLE_VULKAN=false;
else ANGLE_GL=false; ANGLE_VULKAN=true; fi
cat > "$ANGLE_OUT/args.gn" <<GN
 target_os = "android"
 target_cpu = "$ANGLE_CPU"
 is_debug = false
 is_component_build = false
 symbol_level = 1
 use_thin_lto = false
 use_remoteexec = false
 use_siso = false
 clang_use_chrome_plugins = false
 angle_assert_always_on = true
 angle_enable_gl = $ANGLE_GL
 angle_enable_vulkan = $ANGLE_VULKAN
 angle_enable_null = false
 angle_build_tests = false
 angle_build_all = false
 angle_libs_suffix = "_lighttouch_angle"
GN
if [ "$ANGLE_PROFILE" = no-abseil-perfetto ]; then
    printf '%s\n' 'angle_enable_abseil = false' 'angle_enable_perfetto = false' >> "$ANGLE_OUT/args.gn"
fi
if [ "$ANGLE_CPU" = arm64 ]; then echo 'arm_control_flow_integrity = "none"' >> "$ANGLE_OUT/args.gn"; fi
# This pin's Android build dependency sets 16384 max-page-size for 64-bit CPUs.
"$ANGLE_DEST/angle/buildtools/linux64/gn" gen "$ANGLE_OUT" --fail-on-unused-args
if [ "$ANGLE_STAGE" = prepare ]; then
    echo "Prepared without compilation: $ANGLE_DEST/angle/$ANGLE_OUT"
    exit 0
fi
"$ANGLE_NINJA" -C "$ANGLE_OUT" -j"$ANGLE_JOBS" libEGL libGLESv1_CM libGLESv2
python3 - "$ANGLE_OUT" <<'PY'
import hashlib,json,pathlib,subprocess,sys
out=pathlib.Path(sys.argv[1])
libraries=[out/f'lib{name}_lighttouch_angle.so' for name in ['EGL','GLESv1_CM','GLESv2']]
receipt={'angle':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
         'build':subprocess.check_output(['git','-C','build','rev-parse','HEAD'],text=True).strip(),
         'args_gn':(out/'args.gn').read_text(),
         'source_receipt_sha256':hashlib.sha256(pathlib.Path('../source-receipt.txt').read_bytes()).hexdigest(),
         'clang_version':subprocess.check_output(['third_party/llvm-build/Release+Asserts/bin/clang','--version'],text=True).strip(),
         'gn_version':subprocess.check_output(['buildtools/linux64/gn','--version'],text=True).strip(),
         'libraries':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in libraries}}
(out/'lighttouch-angle-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps(receipt,indent=2))
PY
printf '%s\n' "$ANGLE_DEST/angle/$ANGLE_OUT"
