#!/bin/sh
# Build the host side of an in-emulator restore into OUT (no system install):
#   OUT/irecv/libirecovery-1.0.5.dylib   libirecovery with the QEMU socket transport (./libirecovery-qemu)
#   OUT/idevicerestore                   upstream idevicerestore 4b3e847, linked against it
#   OUT/idevicerestore-legacy            optional (LEGACY=1): LukeZGD's fork 5ea1b08, for 2.x IPSWs without a BuildManifest
# Needs clang, curl, git, autoconf, automake, libtool, pkg-config, and libusb, libimobiledevice(-glue), libplist,
# libtatsu, libzip (Homebrew has them all).
#
#   contrib/ipod-restore/build-host-tools.sh OUT
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: build-host-tools.sh OUT}
mkdir -p "$out"
out=$(cd "$out" && pwd)

sh "$here/libirecovery-qemu/build.sh" "$out/irecv"

build_idr() {   # URL REVISION NAME
    src="$out/src-$3"
    rm -rf "$src"
    git clone -q "$1" "$src"
    git -C "$src" checkout -q "$2"
    # A fork's tags may be missing; give configure a version without them.
    printf '%s\n' "1.0.0-$2" > "$src/.tarball-version"
    (cd "$src" && rm -rf .git && NOCONFIGURE=1 ./autogen.sh >/dev/null &&
        libirecovery_CFLAGS="-I$out/irecv" libirecovery_LIBS="-L$out/irecv -lirecovery-1.0.5" ./configure >/dev/null &&
        make -j4 >/dev/null)
    cp "$src/src/idevicerestore" "$out/$3"
    # The transport library sits next to it; DYLD_* does not survive a protected launcher.
    install_name_tool -add_rpath "$out/irecv" "$out/$3"
    codesign -f -s - "$out/$3" 2>/dev/null || true
}

build_idr https://github.com/libimobiledevice/idevicerestore.git 4b3e847 idevicerestore
if [ "${LEGACY:-0}" = 1 ]; then
    build_idr https://github.com/LukeZGD/idevicerestore.git 5ea1b08 idevicerestore-legacy
fi
echo "built: $out/idevicerestore${LEGACY:+ and $out/idevicerestore-legacy}"
