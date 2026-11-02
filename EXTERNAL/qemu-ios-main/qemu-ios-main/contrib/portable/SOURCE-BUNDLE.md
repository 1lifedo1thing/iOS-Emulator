# Portable engine corresponding source and relink material

This immutable directory represents one exact native engine identified by
`bundle.json`. It includes its recorded QEMU source revision, selected upstream
dependency source archives, original recipe build prerequisites, downstream
patches/build recipes, pinned libfdt/keymap
source, actual linked objects/static archives, explicit relink arguments and
offline notices. Guest firmware, ROMs, NAND, prepared bases and captures are
excluded. Original objects retain compiler debug/build-path metadata.
`bundle.json` labels dependency archives as linked or build prerequisites.
For example, the original dependency builder also builds libffi and OpenSSL;
their source archives support replaying that recipe, while neither library
appears in the engine's recorded link inputs. Including an unlinked source
archive does not describe it as part of the engine's binary license combination.
The immutable source includes QEMU's tracked pure-Python build-tool wheels so
configure can run offline; those archives retain their embedded license notices.

This material covers the native engine. Complete application/firmware source
and notices, publication availability and chosen license-compliance route must
be reviewed separately. Bundling files does not establish guest qualification
or a complete distribution clearance. QEMU is distributed under GPLv2; retain
the separate upstream grants and notices under `notices/`. Static LGPL libraries
can be modified/rebuilt and replaced through the relink recipe below.

Validate every file against the publisher's supplied receipt SHA256:

```sh
python3 relink-bundle.py . --manifest-sha256 RECEIPT_SHA256
```

Relink without configure, Ninja or guest execution, using Linux-host Android
NDK 28.2.13676358 and a fresh output outside this directory:

```sh
python3 relink-bundle.py . --manifest-sha256 RECEIPT_SHA256 \
  --ndk /path/to/android-ndk-28.2.13676358 \
  --output /private/relinked/libqemu-arm.so
```

The tool verifies the recorded compiler, NDK properties, link stubs, runtime
and all objects/archives before linking. It writes a separate `.relink.json`
receipt and stages the matching `libc++_shared.so`. A relocated build can change
debug metadata or linker identity; compare ELF closure/API and run the same
target tests before qualification. Byte identity is reported only if observed.

To exercise modification/relinking, first unpack the selected source archives
from `source/dependencies/` into a separate writable build tree. The preserved
`source/qemu/contrib/portable/build-deps.py`, `build-crypto-deps.py`, dependency
pins and downstream patches define the original static/PIC target recipes.
Use the same ABI, API 28 or newer and NDK. Modified sources require a separate
build directory/recipe rather than bypassing the original cache checks. Retain
your modifications and source notices. Substitute any selected LGPL archive:

```sh
python3 relink-bundle.py . --ndk /path/to/android-ndk-28.2.13676358 \
  --output /private/relinked-modified/libqemu-arm.so \
  --replace libavcodec.a=/private/modified-prefix/lib/libavcodec.a \
  --replace libavutil.a=/private/modified-prefix/lib/libavutil.a
```

Other recorded replacements include selected GLib, proxy-libintl, libgcrypt
and libgpg-error archives. `bundle.json` is authoritative for this artifact's
available names. The original engine objects stay available so replacement
does not require rebuilding application code. Replacement results need actual
target testing; a successful link alone does not establish compatibility.

To rebuild the engine source, prepare a writable checkout with the validated
helper below. It unpacks pinned libfdt/keymap source and disables installation
and unpacking of unrelated firmware blobs and host UI icons omitted from this
bundle. Those build options
do not change the selected engine object code. Actual configure/rebuild
validation remains required before claiming a reproduced build. The filtered
source archive excludes nonbuild assets and research captures, so
its new local Git tree/identity differs from the original full upstream commit;
`bundle.json` records the original corresponding source revision and every
included file hash. The helper initializes a clean local commit for the build
guard and reports the installation/unpacking-only adjustments. Use the original
pinned dependency archives (the builders verify
their SHA256), Python 3, Meson 1.10.0 with packaging 24.0, Ninja and pkg-config.

```sh
python3 prepare-bundle-source.py . /private/writable-qemu \
  --manifest-sha256 RECEIPT_SHA256
# Stage the included pinned release archives before invoking the builders.
# This reads only source metadata and copies files; it does not download/build.
python3 - . /private/writable-qemu /private/builds/deps /private/builds/crypto <<'PYSEED'
import json
import pathlib
import shutil
import sys
bundle, source, deps, crypto = map(pathlib.Path, sys.argv[1:])
for filename, output in [('dependencies.json', deps),
                         ('crypto-dependencies.json', crypto)]:
    output.mkdir(parents=True, exist_ok=False)
    pins = json.loads((source / 'contrib/portable' / filename).read_text())
    for package in pins['packages']:
        archive = package['archive']
        shutil.copyfile(bundle / 'source/dependencies' / archive, output / archive)
PYSEED
# ABI is the bundle.json value.
cd /private/writable-qemu
python3 contrib/portable/build-deps.py /private/builds/deps \
  --abi arm64-v8a --ndk /path/to/android-ndk-28.2.13676358 --meson /path/to/meson
python3 contrib/portable/build-crypto-deps.py /private/builds/crypto \
  --abi arm64-v8a --ndk /path/to/android-ndk-28.2.13676358
python3 contrib/portable/build-engine.py /private/builds/engine \
  --abi arm64-v8a --ndk /path/to/android-ndk-28.2.13676358 \
  --prefix /private/builds/deps/prefix --crypto-prefix /private/builds/crypto/prefix
```

Use `x86_64` for an x86 bundle. Each builder verifies the staged archive hashes
before extraction. Use fresh output directories to preserve its configuration
guards. `--prepare-only` verifies and extracts without compiling; the exact
source/dependency configuration and rebuild still require separate validation. The original generator
and selected-source audit are available in the source reference recorded in
`bundle.json`. A rebuild embeds the new local Git identity; it is not silently
represented as the publisher's original binary.
