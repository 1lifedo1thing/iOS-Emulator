# Portable QEMU embedding build

This lane preserves `qemu_ios` API 2.2 and the original macOS sources and
build-support dependency pins. The additive portable ABI is version 1 in
`contrib/ios-app/qemu-ios-ui.h`. A library can initialize QEMU **once**; every
restart requires a fresh worker process. The host must finish worker shutdown
before freeing callbacks, frame buffers, or audio resources.

Use fresh private build outputs outside the checkout. Input stamps record ABI,
compiler, dependency/archive and patch hashes, recipe hashes, tool versions,
and options. A changed or unstamped compiled cache is refused; choose a fresh
output instead of deleting the stamp. Engine builds require a clean committed
source tree. The shared library manifest hashes every tracked source and
selected object/archive/shared-library input, including renderer/device code. These scripts never read or
stage firmware, ROMs, guest assets, or credentials. Dependencies are downloaded
from their official release sources and SHA-256 checked against
`dependencies.json`. FFmpeg 9.0.1 retains the macOS version/hash and both
existing H.264 patches. Other dependencies add portable build inputs without
changing the macOS pin ledger. Android builds use NDK 28.2.13676358, API 28 or
newer, static PIC third-party dependencies, and 16 KiB ELF maximum page size.
The platform provides zlib; its metadata version is read from the NDK header.

The engine defaults to API 28. `build-engine.py --android-api 29` selects a
separate, unqualified native ELF TLS experiment for apps whose minimum API is
already 29. Use a fresh build directory: changing this compiler target is not
permitted by `--source-update`. See [PERFORMANCE.md](PERFORMANCE.md) for the
controlled build, thread-lifetime regressions and target comparison gates.

```sh
# MESON is Meson 1.10.0; its Python needs packaging 24.0.
# Install host tools in a private venv when unavailable, never change system Python.
python3 contrib/portable/build-deps.py /private/builds/deps-linux --only ffmpeg
python3 contrib/portable/build-crypto-deps.py /private/builds/crypto-linux
python3 contrib/portable/build-engine.py /private/builds/linux \
  --prefix /private/builds/deps-linux/prefix \
  --crypto-prefix /private/builds/crypto-linux/prefix

python3 contrib/portable/build-deps.py /private/builds/deps-arm64 \
  --abi arm64-v8a --ndk "$ANDROID_NDK_ROOT" --meson "$MESON"
python3 contrib/portable/build-crypto-deps.py /private/builds/crypto-arm64 \
  --abi arm64-v8a --ndk "$ANDROID_NDK_ROOT"
python3 contrib/portable/build-engine.py /private/builds/arm64-v8a \
  --abi arm64-v8a --ndk "$ANDROID_NDK_ROOT" \
  --prefix /private/builds/deps-arm64/prefix \
  --crypto-prefix /private/builds/crypto-arm64/prefix
# Repeat all three commands with --abi x86_64 and distinct output/prefix paths.
```

Each compiler stage uses at most two jobs. The shared library relinker uses
Ninja's real link arguments and compile configuration, retaining its response
file, replacing only `system/main.c` with the existing iOS entry/display bridge
and the macOS controls/audio implementation. The latter is generated privately
with its Mach-O image identity replaced by a compiled Git revision; its pause
handler also cancels live host input. ELF exports only `qemu_ios_*`. Inspect
`readelf -d libqemu-arm.so` before packaging, including any NDK C++ runtime.
`portable-link.json` records the exact final link arguments.
`audit-selected-sources.py BUILD` inventories linked source notices and Ninja's
actual included headers against the immutable manifest revision. It reports
SPDX terms and text review candidates, including missing or ambiguous notices;
it does not give a legal verdict. The shared manifest also resolves and hashes
libraries named through `-l` flags.

Generate Android offline notice assets from a completed immutable engine after
running its selected-source audit:

```sh
python3 contrib/portable/audit-selected-sources.py /private/builds/arm64-v8a
python3 contrib/portable/package-notices.py /private/builds/arm64-v8a \
  /private/generated/engine-notices-arm64 \
  --deps /private/builds/deps-arm64 --crypto-deps /private/builds/crypto-arm64 \
  --ndk "$ANDROID_NDK_ROOT" \
  --source-reference "https://github.com/blake-lucas/qemu-ios/tree/COMMIT"
```

The schema-1 manifest records the engine's actual revision/library hash,
selected archive hashes, public upstream references and each offline notice
file's SHA-256. The generator checks immutable tracked notices, selected link
archives, verified release archives, pinned libfdt and NDK notice inputs. It
excludes dependencies absent from the selected link, such as firmware's
OpenSSL. Repeating the command requires identical output; changed contents
require a fresh destination. No private build paths or guest assets enter the
bundle. These notice assets do not replace corresponding source, applicable
static LGPL relinking material or complete application distribution review.

Create a local corresponding-source and static relink bundle for a completed
Bionic engine with matching notices:

```sh
python3 contrib/portable/package-source.py /private/builds/arm64-v8a \
  /private/generated/source-arm64 \
  --deps /private/builds/deps-arm64 --crypto-deps /private/builds/crypto-arm64 \
  --notices /private/generated/engine-notices-arm64 --ndk "$ANDROID_NDK_ROOT" \
  --source-reference "https://github.com/blake-lucas/qemu-ios/tree/COMMIT"
python3 /private/generated/source-arm64/relink-bundle.py \
  /private/generated/source-arm64
```

`SOURCE-BUNDLE.md` documents validation against the publisher's receipt,
original/replacement-library relinking and rebuilding the filtered source.
No guest input is read, and no upload occurs. Original object debug metadata
retains compiler/build paths; textual bundle receipts use portable paths.
Only an actual executed relink/rebuild validates those respective operations.

The portable recipe selects QEMU's existing public-domain AES implementation
for the Apple AES model and pinned libgcrypt MPI for its signed Montgomery PKE
operations. The original macOS OpenSSL implementation remains selected without
`QEMU_IOS_PORTABLE_CRYPTO`. Separate crypto prefixes avoid changing OpenSSL
inputs used by the firmware tool. `crypto-dependencies.json` pins release
archives, and the builder retains their COPYING/COPYING.LIB/LICENSES notices.
The libgpg-error patch generates mutex storage from the target Bionic compiler
instead of borrowing a glibc layout. QEMU's existing libgcrypt initialization
also supplies its shared cipher/hash backend. Unused virtio IOThread mapping is
selected only when virtio block or SCSI is present; neither is selected by the
portable device config.

`portable.mak` selects the existing iOS models plus ARM virt for legal TCG
smoke programs. Device prerequisites are explicitly selected in Kconfig, as in
the audited Windows reference; default upstream configurations remain present.
Linux graphics use EGL compatibility OpenGL. Android uses EGL and native GLES1
and GLES2 selected from the compiler's `__ANDROID__` marker. A graphics-stub
build never establishes port acceptance. The backend version and separate
verified guest-asset version must be included in each run's manifest.

The portable frame operation copies tightly packed BGRA while holding the
publisher mutex. Caller storage remains valid across subsequent frames. A
small buffer returns dimensions/required bytes without acknowledging delivery.
Touch batches preserve full 32-bit host contact IDs, validate the whole batch
before any injection, map active IDs to five existing controller slots, emit
coordinates before commits, and close with one sync. Eight is the ABI batch
bound, not the emulated controller contact limit. Poll the newest ID to confirm
dispatch to the input model; `QUEUED` is only queue admission. A completed
dispatch does not acknowledge that the guest collected its SPI report or acted
on the gesture. Linux and Android retain up to 32 controller reports, preserving
START and END before contact reuse while coalescing only trailing motion.
Admission reserves cancellation capacity; a full transition queue reports BUSY
before the batch is dispatched. The original macOS controller retains its
single pending report behavior. The legacy button
operation returns no delivery acknowledgment: runtime success means its request
was admitted, and the engine applies GPIO/PMU edges later under the BQL. Button
release has a minimum 100 ms hold in guest virtual time. Pause and resume also
enqueue work; use the engine runstate to observe their completion. Foreground
resume permits rendering and resumes execution, while guest software controls
the panel and backlight. Confirm wake using guest progress and display facts.
Cancel releases touch and
buttons even while paused and resets live motion. Held contacts/live samples
are never migrated or retained across reset/worker restart.

Motion samples use guest portrait axes x-right, y-up, z-toward-viewer.
Acceleration is Android-style specific force including gravity in m/s²; it is
negated once at the existing LIS model's gravity-vector boundary, then the
existing board sensor mount/full-scale conversion supplies guest registers.
Gyroscope values are rad/s in the same portrait frame; the existing L3G model
applies its mount and CTRL_REG4 sensitivity. Accel and gyro samples expire
independently after 250 ms of virtual time. Pause cancels them. n72 has an
accelerometer and no gyro; n81 and n90 advertise their existing L3G models.
These capability facts are model support, not proof of an accepted guest run.

Focused host checks (ASan/UBSan, actual production functions):

```sh
tests/slice/run.sh tests/slice/ipod-portable-frame.c
tests/slice/run.sh tests/slice/ipod-ui-deferred-paint.c
tests/slice/run.sh tests/slice/ipad1-display-vsync-paint.c
tests/slice/run.sh tests/slice/ipod-portable-touch.c
tests/slice/run.sh tests/slice/ipod-accel-sampling.c
tests/slice/run.sh tests/slice/ipod-attitude.c
tests/slice/run.sh tests/slice/ipod-multitouch-frames.c
tests/slice/run.sh tests/slice/ipod-multitouch-order.c
tests/slice/run.sh tests/slice/ipod-pke.c
tests/slice/run.sh tests/slice/ipod-portable-pke.c
python3 tests/portable-crypto.py /private/builds/linux
python3 tests/portable-models.py /private/builds/linux/engine-run \
  /private/builds/linux/libqemu-arm.so
```

Portable frame publication adapts upstream qemu-ios
`97860b36b0e9fcdd008e95e58efff9609ca44aee`: one reusable main-context bottom
half coalesces panel dirty rectangles and captures the completed paint, rather
than waiting solely for the 30 ms display refresh. The refresh remains a
fallback, unchanged pixels keep their serial, and caller-owned frame copies
retain their lock/ownership contract. Reset cancels pending publication; engine
exit cancels and deletes the bottom half before QEMU cleanup. The Apple path
keeps its original display-refresh publication. This is a frame delivery change,
not a verified Android presentation rate or CPU speedup.

The A4 `s5l8930.display` portable path also paints its completed latch at visible
vsync, adapting the separate display hunk from the same upstream commit. Its
existing unchanged-front check suppresses duplicate publication; hidden/absent
consoles and the held-swap test path do not add paints. Timer rearm, interrupt
logic and the Apple path remain unchanged. This does not enable experimental
120 Hz or import later upstream IRQ model changes. The A4 slice exercises the
actual VBL and display-update functions with visible/hidden/null consoles,
unchanged latches, input/stop callbacks and timer rearm. Removing the new paint
block must fail the visible-paint assertion; no A4 guest result follows from it.

The deferred-paint slice uses the actual capture/publication/reset/stop functions
and AIO enqueue/dequeue/cancel/delete operations. It covers partial-rectangle
coalescing, fallback deduplication, changed-size completed surfaces, reset and
pending-stop cancellation, and absent surfaces. Run it, the frame ownership,
startup and deduplication slices, plus Linux/ARM/x86 production syntax checks in
an allocated compiler slot. A target comparison must use the same fixture and
scrolling workload with frame intervals and host CPU; keep the frozen PCM A/B
inputs separate. Added source tests alone are not an executed qualification.

These checks establish frame ownership, stable contact identity/crossing,
atomic rejection, paused release, sensor units/mounts/expiry/restore behavior,
and controller wire checksums. They do not establish actual guest reception,
physical gyroscope acceptance, ARM64 JIT qualification, or guest graphics.
The whole n72 model check uses only a legal ARM loop, erased NOR and an empty
NAND directory. It verifies actual board initialization and required registered
peripherals before packaging, including the UNIMP dependency missed by a
virt-only CPU smoke. It is separate from actual guest boot acceptance.

QEMU and reused glue retain upstream GPL terms. GLib is LGPL-2.1-or-later,
proxy-libintl retains its GNU Library GPL v2 notice, PCRE2 BSD, libffi MIT,
pixman MIT, libgcrypt/libgpg-error LGPL-2.1-or-later library terms, OpenSSL
Apache-2.0 for the separate firmware dependency, and the restricted FFmpeg
configuration LGPL. Distribution eligibility remains unresolved pending the
actual selected-source and complete application dependency audit. Vector tests
and source changes alone do not qualify a rebuilt engine or resolve licensing.
Retain upstream source/license notices and provide the corresponding source,
patches, and reproducible build/relink material when distributing engine
binaries; LGPL static dependencies also require their applicable relinking
material. The source manifest is `dependencies.json`, not permission to
redistribute proprietary guest material.

For an existing completed engine, add `--source-update` to the same engine
command after committing a source change. This explicitly permits a source
revision change while keeping ABI, configure arguments/macros, compiler/tools,
environment, NDK and dependency inputs unchanged. Configure and Ninja then
rebuild changed sources and included headers. A reviewed migration accepts the
exact earlier portable recipe once, checking its completed compiler/Ninja and
Meson baseline; subsequent unknown recipe changes still fail. The update
history records the prior library/manifest and requested/compiled revisions.
The new shared manifest records each reused object's earlier recorded engine
revision/hash and each changed object's current engine revision/hash. Earlier
manifests are retained under `BUILD/manifests` by content hash. Changes to configuration or pins
still require a separate output directory. An interrupted update's requested
revision is distinct from the last completed library manifest.


### n72 AP wake and physical button holds

A modeled n72 hibernate wake resets the AP while external buttons remain physical
inputs. The non-Apple route records those input levels separately, restores only
those levels during AP wake, and clears the wake reason after every reset hold
callback has run. Manual button holds retain their original virtual release
deadline. Automation and touch cleanup still run; cold reset and explicit input
cancellation release buttons and clear stale host intent. Guest GPIO configuration
and unrelated outputs still reset. Apple input/reset behavior is unchanged.

Run `tests/slice/run.sh tests/slice/ipod-ap-wake-buttons.c` for the coupled
production reset/timer regression. It checks both reset callback orders, all four
button polarities, original100ms deadlines, cold-reset stale-intent prevention,
explicit cancellation and automation-boundary cleanup under ASAN/UBSAN. Clock,
timer, IRQ and board delivery boundaries are stubbed; it does not execute a full
PMU/board reset, qualify guest wake or prove the complete black-screen cause.
Actual target GPIO/PMU traces, visible guest response and lifecycle acceptance
remain required.

### n72 boot arguments before kernel handoff

The non-Apple n72 route adapts upstream QEMU commit
`1b451cc86c77c153fae1d7be2d4f151e693a9d75`. Before a NAND read made by iBoot,
it finds the unique `snprintf(CommandLine, 256, "%s ", ...)` format literal and
supplies the requested arguments there, so iBoot hands them to the kernel.
Percent signs are escaped; the escaped format must fit 255 bytes. Empty arguments
keep the firmware command line. A missing or ambiguous pattern emits an
unqualified-iBoot warning and does not fall back to late kernel-memory writes.

`boot-args-delay-ms`, `boot-args-repeat` and `boot-args-interval-ms` retain their
startup parsing, validation and getters for compatibility. They are deprecated
and inert on this portable early-handoff route. Apple builds retain their
original timer, argument-length policy, legacy finder and option behavior.

Run `tests/slice/run.sh tests/slice/ipod-early-handoff.c` for the production
finder/NAND-hook regression, `ipod-early-boot-args.c` for both startup APIs and
legacy-option parsing, and `ipod-portable-boot-args.c` for the retained Apple timer.
Optionally set `IT_TEST_IBOOT` to a read-only n72 7E18 iBoot image for the known
corpus check; no proprietary fixture is included. The production-board
`ipod-boot-args-test` qtest also covers FMSS notification and reset with a
synthetic iBoot, and must be run against the rebuilt engine. Slice tests use
emulated RAM and do not execute guest snprintf or prove actual boot, improve
measured performance, or identify a Pixel-specific failure.
