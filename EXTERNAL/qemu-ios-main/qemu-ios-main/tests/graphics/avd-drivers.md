# Native GLES1 framebuffer investigation

The Android backend needs a real GLES1 framebuffer implementation in addition
to GLES2. Passing the GLES2 pixel suite does not qualify the GLES1 guest path.
Use the complete production render and pixel probes after selecting a driver;
keep the capability guard active until a supported renderer passes them.

The installed emulator 37.2.12 lists `host`, `software`, `lavapipe`,
`swiftshader`, and `swangle`. Android's [official acceleration instructions](https://developer.android.com/studio/run/emulator-acceleration)
describe `host` as the host GPU driver and `swangle` as ANGLE with SwiftShader.
Use these current mode names; their `_indirect` predecessors are deprecated.

Root allocates the exclusive AVD/build slot. With that allocation, start one
fresh process with two cores and 2 GiB RAM, never wipe its data, and never change
the global ADB server or address the phone. Commands for the existing 4 KiB AVD:

```sh
sudo -n -u blake /home/blake/Android/Sdk/emulator/emulator \
  -avd LightTouch_API35 -no-window -no-snapshot -no-audio \
  -gpu host -cores 2 -memory 2048
```

Repeat in a separate stopped/started attempt with `-gpu swangle` if needed.
Every ADB command must use `-s emulator-5554`. Stop only this emulator with
`adb -s emulator-5554 emu kill`; preserve source/binary SHA256 manifests,
actual shell exit status, renderer/extension strings and emulator startup logs.
Use the 16 KiB AVD only after a separate allocation. Re-push probes on each boot,
verify remote SHA256 against the immutable local manifest, and run `sync` before
shutdown. Empty remote probe files were observed after cold boot; executing an
empty file can return zero without running any GL code. Require the expected
phase/pixel/PASS output as well as the actual shell exit status.

`egl-core-fbo` is a bounded diagnostic, with a 20-second alarm and phase logging.
Its default mode experimentally invokes GLES2 core framebuffer functions while
GLES1 is current. `--oes` instead checks advertised OES framebuffer dispatch.
Each requires a complete color framebuffer, packed depth/stencil framebuffer,
and a real red fixed-function triangle readback. Failure exits 2; signal/timeouts
exit 128 plus the signal number. It always requests native GLES contexts.
A successful core experiment would still require a supported API contract and
the full production guest path before adoption.

## Why changing the backing driver may help

At upstream gfxstream revision `07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe`,
[GLES1 extension advertisement](https://github.com/google/gfxstream/blob/07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe/host/gl/glestranslator/gles_cm/gles_cm_context.cpp)
and [OES entry points](https://github.com/google/gfxstream/blob/07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe/host/gl/glestranslator/gles_cm/gles_cm_imp.cpp)
require the backing `GL_EXT_FRAMEBUFFER_OBJECT` capability. The
[capability detector](https://github.com/google/gfxstream/blob/07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe/host/gl/glestranslator/common/gles_context.cpp)
sets it for a desktop core profile or a literal `GL_EXT_framebuffer_object`
extension token. GLES core framebuffer support alone does not satisfy that
condition. This source is an explanation to test, not a claim that the installed
binary was built from that exact revision. Desktop Mesa in `host` mode may
satisfy it; an ANGLE GLES backing in `swangle` may encounter the same gate.

## If supported emulator modes still fail

A pinned ANGLE runtime is a substantive fallback: ANGLE's
[ES1 implementation](https://chromium.googlesource.com/angle/angle/+/8cd050f07ebd65cce269fc92e36699fc03b20ac3/doc/ES1Status.md)
implements the fixed-function API through GLES3 features. The repository's
existing evaluated pin is `8cd050f07ebd65cce269fc92e36699fc03b20ac3`, with
`libEGL`, `libGLESv1_CM` and `libGLESv2` build targets. Android builds require
Linux and Android dependencies through gclient, as described in the
[official Android setup](https://chromium.googlesource.com/angle/angle/+/main/doc/DevSetupAndroid.md).

A packaged runtime would need distinct library names, explicit loading/dispatch
for both GLES APIs and EGL, and a GLES3-capable backing GLES or Vulkan driver.
It must preserve shared texture/context behavior and pass the complete frozen
adapter probes plus actual GLES1/GLES2 guest workloads on both page-size AVDs.
The pinned ES1 implementation has documented missing smooth lines and two-sided
lighting; do not assume compatibility from triangle probes or replace guest
operations with no-ops. Resolve any required workload gap before qualification.
The platform's developer-option ANGLE driver is limited to Java-launched apps;
a native shell probe cannot establish that app-driver path, and the frontend's
native worker loading must be tested separately. No alternate runtime build is
started until the heavy build slot is allocated.

## Recorded 4 KiB outcome

With frozen sources/binaries in private `attempt-avd-fbo-04`, native SwiftShader
and `swangle` both rejected the required GLES1 OES framebuffer extension. Their
experimental core-FBO calls exited 139 at `glGenFramebuffers` inside GLES1;
core calls must never replace the supported OES contract. Native GLES2 pixel,
packing, shader-query, shared lifetime and copy-storage tests passed.

`host` initially failed to initialize because no X display was available.
An isolated `xvfb-run` display then supplied Mesa llvmpipe OpenGL 4.5 core;
GLES1 advertised OES framebuffer support and the OES diagnostic passed complete
color and packed FBOs with actual red RGBA 255,0,0,255. The complete production
renderer still failed its first triangle assertion after complete FBO setup.
This is an active rendering investigation, not an AVD qualification. Earlier
empty-output swangle samples were invalid and are retained as such.

The host emulator log showed invalid `glActiveTexture` calls in the production
wrapper. Upstream [GLES1 context constants](https://github.com/google/gfxstream/blob/07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe/host/gl/glestranslator/gles_cm/gles_cm_context.h)
limit fixed-function units to four, while the backing capability detector
substitutes eight for core profiles. The adapter now verifies usable units once,
restores active state, preserves preexisting errors and returns a consistent
integer/float limit. `egl-limits.c` reproduces the eight-reported/four-accepted
capability mismatch without EGL. Frozen host `units05` then verified that limit,
prior-error preservation and the first red triangle. Its complete production
render still failed two-unit ADD with black RGBA 0,0,0,255 and no error;
the native GLES2 pixel suite passed. This is partial improvement, not complete
host-AVD graphics qualification.

The next frozen `raw-combine07` used only EGL lifetime and raw GLES1 calls,
with generated OES framebuffer/texture names. On the actual 4 KiB host AVD,
the color framebuffer was complete (`0x8cd5`) and both texture units configured
without error. With both textures enabled, active unit 0 produced blue
RGBA 0,0,255,255; active unit 1 produced black RGBA 0,0,0,255. Both draws should
produce magenta RGBA 255,0,255,255 independent of the active unit. Both reported
GL_NO_ERROR. The real probe and ADB shell exited 2 with the explicit failure
sentinel; the remote binary hash matched the frozen manifest. This reproduces
the standards gap independently of the portability adapter.

Upstream [core-profile texture emulation](https://github.com/google/gfxstream/blob/07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe/host/gl/glestranslator/gles_cm/core_profile_engine.cpp)
selects one sampler using the current active unit, and its
[fragment shader](https://github.com/google/gfxstream/blob/07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe/host/gl/glestranslator/gles_cm/core_profile_engine_shaders.h)
handles replace/modulate rather than full texture-stage ADD/COMBINE. These
sources explain the observed behavior without asserting the installed binary's
exact upstream revision. See [the pinned Android ANGLE candidate](angle-android-candidate.md)
for the next full GLES1 frontend strategy. That candidate remains unbuilt and
unqualified; another backing-driver replay cannot establish missing frontend
semantics.

Run this focused leaf on an already allocated host AVD using:

```sh
python3 tests/graphics/run-avd-probes.py --adb PATH_TO_ADB \
  --frozen FROZEN_RAW_COMBINE_DIRECTORY --output NEW_RECEIPT_DIRECTORY \
  --combine-only
```

This verifies local/remote hashes, records actual page size and fingerprint,
and requires the complete leaf PASS sentinel and explicit exit. Leaf success
would still leave full adapter and actual guest acceptance outstanding.
