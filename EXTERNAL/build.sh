#!/bin/bash
# Builds the binaries in resources\ from the sources in EXTERNAL\, with the portable MSYS2 in EXTERNAL\msys64.
# Nothing is installed outside the project. Run it from any shell:
#
#   EXTERNAL\msys64\usr\bin\bash.exe EXTERNAL/build.sh <step>...
#
# Steps, in the order "all" runs them:
#   toolchain    (once) updates MSYS2 and installs the UCRT64 packages the other steps need
#   ffmpeg       FFmpeg 9.0.1 with qemu-ios's two H.264 patches (avcodec-63, avutil-61), into EXTERNAL/build/prefix
#   engine       libqemu-arm.dll (qemu-ios, with the iPhone 4S machine), into EXTERNAL/build/dll
#   firmwarekit  firmwarekit.exe (LightTouch's lighttouch-firmware), into EXTERNAL/rust/target
#   helpers      the iPhone 4S's install helpers it_install and it_keybag, into EXTERNAL/build/helpers
#   install      copies everything above, with the DLLs it needs, into resources\ (and build\<Config>\resources\
#                where that exists), then rewrites resources\runtime-manifest.json
#
# Everything is built against the UCRT (MSYS2's UCRT64), like the app. The libimobiledevice DLLs and
# lighttouch-usbmuxd.exe in resources\ are LightTouch's prebuilt ones and are left as they are.
set -e

# Re-run as a UCRT64 login shell, which puts EXTERNAL/msys64/ucrt64/bin (gcc, clang, cargo, python) on PATH.
HERE=$(cd "$(dirname "$0")" && pwd)
if [ "$MSYSTEM" != UCRT64 ]; then
    exec env MSYSTEM=UCRT64 CHERE_INVOKING=1 "$HERE/msys64/usr/bin/bash.exe" -l "$HERE/build.sh" "$@"
fi

P=$(cd "$HERE/.." && pwd)                        # the project
SRC=$P/EXTERNAL/qemu-ios-main/qemu-ios-main      # the engine's source
FW=$P/EXTERNAL/LightTouch-main/LightTouch-main   # firmwarekit's source
B=$P/EXTERNAL/build                              # build folders (git-ignored)
R=$P/EXTERNAL/rust                               # cargo's home and target folder (git-ignored)
U=/ucrt64/bin

step_toolchain() {
    pacman --noconfirm -Syuu
    pacman --noconfirm -Syuu
    pacman --noconfirm --needed -S make patch diffutils \
        mingw-w64-ucrt-x86_64-{toolchain,pkgconf,meson,ninja,python,glib2,pixman,libgcrypt,SDL2,glew,zlib,openssl} \
        mingw-w64-ucrt-x86_64-{clang,lld,nasm,dtc,rust}
}

step_ffmpeg() {
    mkdir -p "$B" && cd "$B"
    [ -f ffmpeg-9.0.1.tar.xz ] || curl -L --fail -o ffmpeg-9.0.1.tar.xz https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz
    echo "cf38e0e28c7e5605942c4a77755349b0145804a397af37eb1fb4c77cb237f635  ffmpeg-9.0.1.tar.xz" | sha256sum -c
    if [ ! -d ffmpeg-9.0.1 ]; then
        tar xf ffmpeg-9.0.1.tar.xz
        for p in h264-chunk-er.patch h264-cavlc-pcm-offset.patch; do
            (cd ffmpeg-9.0.1 && patch -p1 < "$SRC/contrib/ffmpeg/$p")
        done
    fi
    mkdir -p ffmpeg-build && cd ffmpeg-build
    [ -f ffbuild/config.mak ] || ../ffmpeg-9.0.1/configure --prefix="$B/prefix" --target-os=mingw32 --arch=x86_64 \
        --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-x86asm \
        --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
        --enable-decoder=aac,mp3,alac,h264 --enable-shared --disable-static
    make -j"$(nproc)" && make install
}

step_engine() {
    export PKG_CONFIG_PATH=$B/prefix/lib/pkgconfig
    mkdir -p "$B/qemu" && cd "$B/qemu"
    [ -f build.ninja ] || "$SRC/configure" --target-list=arm-softmmu --without-default-features \
        --without-default-devices --with-devices-arm=portable --enable-tcg --enable-pixman --enable-gcrypt \
        --enable-slirp --enable-sdl --enable-fdt=internal --disable-docs --disable-tools --disable-werror \
        --audio-drv-list=
    # The relink below reads the exe's link command from ninja's response file, which only a link leaves behind.
    [ -f qemu-system-arm.exe.rsp ] || rm -f qemu-system-arm.exe
    ninja -d keeprsp qemu-system-arm.exe
    python "$SRC/contrib/portable/make-shared-windows.py" . ../dll
}

step_firmwarekit() {
    export CARGO_HOME=$R/cargo-home CARGO_TARGET_DIR=$R/target CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=gcc \
           CC_x86_64_pc_windows_gnu=gcc AR_x86_64_pc_windows_gnu=ar
    cd "$FW"
    cargo build --release --target x86_64-pc-windows-gnu -p lighttouch-firmware --bin firmwarekit
}

step_helpers() {
    python -I "$SRC/contrib/it-install/build.py" "$B/helpers"
}

# Every non-system DLL that the given files need, looked up in the FFmpeg prefix, then UCRT64.
closure() {
    declare -A seen
    local queue=("$@") f d dir
    while [ ${#queue[@]} -gt 0 ]; do
        f=${queue[0]}; queue=("${queue[@]:1}")
        for d in $(objdump -p "$f" | sed -n 's/.*DLL Name: //p' | tr -d '\r'); do
            for dir in "$B/prefix/bin" "$U"; do
                if [ -f "$dir/$d" ] && [ -z "${seen[$d]}" ]; then seen[$d]=$dir/$d; queue+=("$dir/$d"); break; fi
            done
        done
    done
    for d in "${!seen[@]}"; do echo "${seen[$d]}"; done | sort
}

step_install() {
    local dlls to
    # The engine's DLLs, plus OpenSSL's libssl, which the proxy (proxy.cpp) and libimobiledevice load.
    dlls=$(closure "$B/dll/libqemu-arm.dll")
    for to in "$P/resources" "$P"/build/*/resources; do
        [ -d "$to" ] || continue
        cp "$B/dll/libqemu-arm.dll" $dlls "$U/libssl-3-x64.dll" "$R/target/x86_64-pc-windows-gnu/release/firmwarekit.exe" "$to/"
        cp "$B/helpers/it_install" "$to/guest-tools/it_install-n94"
        cp "$B/helpers/it_keybag" "$to/guest-tools/it_keybag-n94"
        rm -f "$to/zlib1.dll"                    # the msvcrt zlib the old engine used; libz-1.dll replaces it
        echo "installed into $to"
    done
    # The manifest lists the engine and its DLLs, with their sizes, hashes and imports (the format of LightTouch's
    # Windows/tools/package-runtime.py).
    (cd "$P/resources" && python -I - libqemu-arm.dll $(for d in $dlls; do basename "$d"; done) <<'EOF'
import hashlib, json, pathlib, re, subprocess, sys
files = []
for name in sys.argv[1:]:
    data = pathlib.Path(name).read_bytes()
    dump = subprocess.check_output(['objdump', '-p', name], text=True)
    files.append({'file': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                  'imports': re.findall(r'^\s*DLL Name:\s*(\S+)\s*$', dump, flags=re.M)})
manifest = {'schema': 1, 'target': 'Windows10-or-later-x86_64', 'execution_tested': False, 'files': files}
pathlib.Path('runtime-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', newline='\n')
print(f'runtime-manifest.json: {len(files)} files')
EOF
    )
}

[ $# -gt 0 ] || { sed -n '2,17p' "$0"; exit 1; }
for step in "$@"; do
    case $step in
        all) step_ffmpeg; step_engine; step_firmwarekit; step_helpers; step_install ;;
        toolchain|ffmpeg|engine|firmwarekit|helpers|install) step_$step ;;
        *) echo "unknown step: $step"; exit 1 ;;
    esac
done
