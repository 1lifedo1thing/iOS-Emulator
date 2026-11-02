#!/bin/bash
# Build the guest-side GLES binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
[ $# -eq 0 ] || { echo "usage: build.sh" >&2; exit 1; }
. "$HERE/../armv6-toolchain/armv6.sh"

# The name table and the dispatch stubs the core (mbxshim.c, compiled into contrib/gles-public and gles1x.c) uses.
sh "$HERE/../gles-public/gligen.sh" >/dev/null
sh "$HERE/genstubs.sh" "$HERE/gles_stubs.h"

# 1.x (the iPod touch 1G): OpenGLES itself is the driver, with no EAGL and the old ObjC runtime, so the same core
# goes in as the framework binary under 1.x's own export names (build-gles1x.sh, gles1x.c). 2.x-5.x take the
# one front end, contrib/gles-public.
bash "$HERE/build-gles1x.sh" "$HERE/OpenGLES-1x"

cc6 "$HERE/sblaunch.c" "$HERE/sblaunch.o"
link6 -execute "$HERE/sblaunch" "$HERE/sblaunch.o"
rm -f "$HERE/sblaunch.o"
# SpringBoard rejects SBSLaunchApplicationWithIdentifier from a caller without
# com.apple.springboard.launchapplications, and reports the refusal only to the
# device console -- the call itself returns 1, exactly as it does for an app
# that is not installed. Signing with the entitlement is not optional.
if command -v ldid >/dev/null; then
    ldid "-S$HERE/sblaunch-entitlements.xml" "$HERE/sblaunch"
fi

file "$HERE/OpenGLES-1x"
