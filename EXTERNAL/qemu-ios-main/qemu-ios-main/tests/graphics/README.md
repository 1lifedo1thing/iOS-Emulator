# Shared renderer probes

Build and execute focused host tests, then cross-build Android probes:

```sh
tests/graphics/check.sh /absolute/private/output /absolute/Android/Sdk/ndk/28.2.13676358
```

The probes compile the production EGL lifetime and native pixel adapters. The
render probe draws actual fixed-function and shader triangles, checks private
object names and packed depth/stencil FBOs, shares a native texture across GLES
versions, switches contexts, reads pixels and releases contexts. The pixel
probe checks multirow RGB texture uploads with explicit row length plus alignment,
BGRA upload/readback, invalid pixel-store state, preserved errors, and destination
retention on failed framebuffer readback. These are host graphics tests; guest
boot, guest rendering and frontend process lifecycle remain separate gates.

With exclusive device ownership, copy the matching Android binary to a private
executable location and run it. For example:

```sh
adb -s DEVICE push /absolute/private/output/egl-render-aarch64 /data/local/tmp/lt-egl-render
adb -s DEVICE shell chmod 755 /data/local/tmp/lt-egl-render
adb -s DEVICE shell /data/local/tmp/lt-egl-render
```

Use `x86_64` binaries on the AVD, and repeat for `egl-pixels`. Both are linked
with 16 KiB ELF alignment. Android runs real EGL/GLES1/GLES2; Linux's render
probe uses Mesa compatibility OpenGL through EGL, while its pixel probe uses a
native GLES2 context. Some Mesa installations cannot create native GLES1
contexts; the Android render probe must still pass on all acceptance targets.

Native GLES1 requires `GL_OES_framebuffer_object`; the production adapter
checks the exact extension token before accepting a context. A non-null
`eglGetProcAddress` result alone is insufficient: Android Emulator's SwiftShader
GLES1 translator has returned unsupported stubs and undefined generated IDs.
The render probe exits with status 2 and a precise capability diagnostic on
such a driver. A supported graphics backend must pass the complete probe.

`egl-core-fbo.c` is a separate experiment to test core GLES2 framebuffer calls
inside a native GLES1 context, followed by required fixed-function rendering.
It does not change production dispatch or establish guest compatibility. Build
it with `check.sh` alongside the render probe. It enforces a 20-second deadline
and logs each operation; `--oes` checks advertised OES dispatch instead. Freeze the
source and binary hashes for each hardware attempt before running it; retain
failed attempts and do not overwrite their artifacts.

The native render probe also checks rectangle pixel coordinates after the guest
texture matrix, independent 2D/rectangle bindings and enables, immutable object
targets, and texture deletion/name reuse across sibling contexts and retained
framebuffer attachments. Deleted textures stay alive while referenced; a new
object with the same guest name receives separate native storage.

See [production-size drawable diagnosis](powervr-drawable-init.md) for the
bounded320x480 native probe, isolated format/name/wrap comparisons and error
conservation checks. A32x32 FBO pass does not cover production NPOT storage.

See [AVD driver investigation](avd-drivers.md) for supported GPU modes, primary
source explanation of the extension gate, and the pinned ANGLE fallback plan.

`egl-limits` uses a bounded fake capability interface to reproduce a translator
reporting eight texture units while accepting only four. It checks retained
active state, prior errors and consistent cached integer/float queries. It is
an error/state regression, not an actual rendering qualification.

For an already allocated AVD, the scoped runner re-pushes frozen binaries,
checks remote hashes and requires each complete probe's final PASS marker and
explicit shell exit. It never launches or stops an AVD and always addresses
`emulator-5554`. `--output` must be a new private directory:

```sh
python3 tests/graphics/run-avd-probes.py \
  --adb /absolute/Android/Sdk/platform-tools/adb \
  --frozen /absolute/private/frozen-attempt \
  --output /absolute/private/new-run
```

Add `--core-diagnostic` only for the experimental core/OES comparison; its
results remain separate from the two complete production probes.

`egl-combine` is a bounded raw EGL/GLES1 diagnostic using generated objects,
OES framebuffer dispatch and two enabled texture stages. It requires the same
magenta ADD result with active texture 0 and 1, independent of the portability
wrappers. It cross-builds with `check.sh`; actual Android execution is pending.
See [pinned ANGLE Android candidate](angle-android-candidate.md) for the supported
frontend alternative and the unexecuted heavy-build recipe.

`surface-attachments.c` executes the production framebuffer attachment handlers
and outgoing IOSurface dirty-mark helper. It checks texture-to-texture and
texture-to-renderbuffer replacement, deferred writeback selection, mark failure
before mutation, depth-only changes, detached textures and the default drawable.
A framebuffer whose color attachment changes must retain the old texture's
pending writeback; waiting until the next framebuffer bind/flush sees only the
new attachment. Marking does not publish an unfinished frame.

```sh
cc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer tests/graphics/surface-attachments.c \
  $(pkg-config --cflags --libs glib-2.0) -o /absolute/private/surface-attachments
/absolute/private/surface-attachments
```

The test models dirty-selected storage copying; it does not execute a GL driver
or establish actual guest rendering. `SURFACE_ATTACHMENTS_SOURCE` can select an
exact predecessor handler include as a negative control: the outgoing-texture
assertion must fail with the handlers that omit the mark.

See [PowerVR capability replay feasibility](pvrvframe-feasibility.md) for current
official platform metadata, ARM64 Mac/emulator integration limits and existing
measured-capability regression coverage.

The ELF inspector retains `x86_64` as its default. A separately compiled ARM64
hardware ANGLE candidate must use the closed `--abi arm64` selector; its report
records `arm64-v8a`. Machine type, little endian, suffixed SONAMEs, dependency
closure and 16KiB LOAD alignment remain required. Cross-ABI files are rejected,
not relabelled. Run `python3 tests/graphics/test-inspect-angle-android.py` for
target-free tests of the production inspector and its rejection paths.

`egl-combine --diagnose-units` additionally logs the raw GLES1 texture-unit
capability query through linked and public resolver entry points. It tries at
most eight server/client selectors, consumes and reports each probe error,
restores both original selectors, and then runs the existing real two-unit ADD
pixel checks. Selector acceptance alone is not a rendering pass. This mode was
added for a translator that rejects `GL_MAX_TEXTURE_UNITS`; it supplies no
replacement capability value and makes no production provider change. The
existing fake `egl-limits` regression covers an overstated limit, not rejection
of the limit query. Retain the exact probe source/binary for each target attempt.

`egl-enable-state.c` is a bounded raw platform GLES1 diagnostic for the retained
gfxstream server-enable query crash. It uses `glGetBooleanv` only, requires the
actually advertised cube-map extension, checks cube/2D state transitions on two
verified server units and lighting, and restores original states/selectors. It
does not exercise `glIsEnabled`, draw pixels, or qualify a guest/provider. Compile
and target execution require their own reviewed grants.

The retained NVIDIA/gfxstream GLES1 case advertises cube maps, but its encoder
crashes when `glIsEnabled(GL_TEXTURE_CUBE_MAP)` treats the server token as a
client-array index. The raw enable diagnostic also rejected `glGetBooleanv` as
a replacement: cube/2D enabled states on both units and global lighting read
back false with zero errors. Neither an absent-extension guard nor a constant
false/query substitution is justified by this case. A probe process returning
exit 6 is a failed state-preservation test even when all GL errors are zero.
