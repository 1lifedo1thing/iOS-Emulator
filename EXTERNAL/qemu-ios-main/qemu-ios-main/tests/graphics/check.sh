#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
cd "$(dirname "$0")/../.."
output=${1:?Usage: tests/graphics/check.sh OUTPUT_DIRECTORY [NDK_DIRECTORY]}
mkdir -p "$output"
cc -Wall -Wextra -Werror tests/graphics/gpio-wake-budget.c -o "$output/gpio-wake-budget"
"$output/gpio-wake-budget"
cc -Wall -Wextra -Werror tests/graphics/frame-pixels.c -o "$output/frame-pixels"
"$output/frame-pixels"
cc -Wall -Wextra tests/graphics/egl-render.c -o "$output/egl-desktop" $(pkg-config --cflags --libs egl gl)
LIBGL_ALWAYS_SOFTWARE=1 "$output/egl-desktop"
cc -Wall -Wextra -Wno-unused-function -DGLES_HOST_EGL_ES tests/graphics/egl-pixels.c -o "$output/egl-pixels" $(pkg-config --cflags --libs egl glesv2 glesv1_cm)
LIBGL_ALWAYS_SOFTWARE=1 "$output/egl-pixels"
cc -Wall -Wextra -Wno-unused-function -DGLES_HOST_EGL_ES tests/graphics/egl-limits.c -o "$output/egl-limits" $(pkg-config --cflags --libs egl glesv2 glesv1_cm)
"$output/egl-limits"
if [ "$#" -gt 1 ]; then
    ndk=$2
    for arch in aarch64 x86_64; do
        for probe in render pixels core-fbo combine drawable-init; do
            "$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/$arch-linux-android26-clang" \
                -Wall -Wextra -Wno-unused-function -DGLES_HOST_EGL_ES \
                "tests/graphics/egl-$probe.c" -o "$output/egl-$probe-$arch" \
                -lEGL -lGLESv2 -lGLESv1_CM -ldl -Wl,-z,max-page-size=16384
        done
    done
fi
