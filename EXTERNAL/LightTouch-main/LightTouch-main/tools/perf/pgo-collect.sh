#!/bin/bash
# pgo-collect.sh APK OUT.profdata PRESET[:RESET[:DUMP]]...
#
# Collect the Android engine's PGO profile (docs/BUILDING.md, "Engine profile"). APK is a build with a
# profile-instrumented engine:
#   LT_ENGINE_VARIANT=pgogen LT_ENGINE_CFLAGS=-fprofile-generate LT_ENGINE_LDFLAGS=-fprofile-generate \
#     scripts/build.sh android
# Each PRESET (fcbench.py's, e.g. n81-jetpack-play) runs once on the phone (ANDROID_SERIAL); the engine zeroes
# its counters RESET seconds after it starts (default 150, past boot and the game's load) and writes them at
# DUMP (default RESET+100), so the profile is gameplay. The raw profiles are merged into OUT and kept beside
# it as OUT.d/PRESET.profraw, so they can be merged again with weights (llvm-profdata merge -weighted-input).
set -eu
APK=$1; OUT=$2; shift 2
NDK_BIN=${NDK_BIN:-$(ls -d "$HOME"/Android/Sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin | tail -1)}
HERE=$(cd "$(dirname "$0")" && pwd)
PKG=org.lighttouch.android
DIR=/data/user/0/$PKG/files
TMP=$OUT.d
mkdir -p "$TMP"
raws=()
for spec in "$@"; do
    IFS=: read -r preset reset dump <<< "$spec"
    reset=${reset:-150}; dump=${dump:-$((reset + 100))}
    echo "== $preset: counters from ${reset} s to ${dump} s"
    adb shell run-as $PKG rm -f files/engine.profraw
    FC_ENV="LT_PGO_RESET=$reset LT_PGO_DUMP=$dump LT_PGO_FILE=$DIR/engine.profraw" \
        timeout 1500 python3 "$HERE/fcbench.py" "$APK" $((dump - 40)) "$preset" > "$TMP/$preset.txt" 2>&1 || true
    tail -1 "$TMP/$preset.txt"
    if adb shell run-as $PKG ls files/engine.profraw > /dev/null 2>&1; then
        adb exec-out run-as $PKG cat files/engine.profraw > "$TMP/$preset.profraw"
        raws+=("$TMP/$preset.profraw")
    else
        echo "   no profile written (the game ran shorter than ${dump} s?)" >&2
    fi
done
[ ${#raws[@]} -gt 0 ] || { echo "no profiles" >&2; exit 1; }
"$NDK_BIN/llvm-profdata" merge -o "$OUT" "${raws[@]}"
ls -la "$OUT"
