# PowerVR capability replay and an ARM64 Mac Android emulator

PVRVFrame can preview an application's GLES/EGL feature compatibility. It does
not emulate the Pixel's PowerVR hardware or driver. Imagination describes it as
a desktop GLES-to-OpenGL layer and explicitly excludes pixel accuracy and
performance simulation. A capability profile changes advertised limits,
extensions and EGL configurations; it does not make Apple hardware execute the
Pixel's native driver. See the official [introduction](https://docs.imgtec.com/tools-manuals/pvrvframe-manual/html/topics/introduction.html)
and [profile manual](https://docs.imgtec.com/tools-manuals/pvrvframe-manual/html/topics/using-profiles.html).

## Current platform blocker

On review, the [current official downloads](https://developer.imaginationtech.com/downloads/)
list Windows and Linux64 for PVRVFrame; [current release notes](https://developer.imaginationtech.com/downloads/latest-release-notes/)
identify10.17.2. The marketing product page still mentions macOS. The newest
macOS item on the [official previous-releases page](https://developer.imaginationtech.com/downloads/previous-releases/)
is2023R1, pointing to `PVRVFrameSetup-2023_R1.pkg` on Imagination's linked S3 host.

Read-only HTTP HEAD and bounded XAR metadata ranges established:

- Package size51,994,889 bytes; server-advertised SHA256
  `3c9c8e44c41fc75c164a1bec1580fe97750f2f7af1ca31ac66a4ec5b967c756b`.
  This is a server claim, not a locally verified whole-package hash.
- Distribution metadata names GUI10.15.3 at `GUI/macOS_x86_64/PVRVFrameGUI.app`.
  The package's BOM contains `osx-intel` and EGL/GLES dylib filenames.
- Only24,183 bytes of header/TOC/BOM/installer metadata were fetched. No payload
  was downloaded, installed or executed. No ARM64 Mach-O library slice was
  established by this inspection.

There is no verified current ARM64 macOS provider to put into the M4's native
ARM64 emulator process. The legacy Intel package is not a supported answer to
an ARM64-only requirement. No Rosetta route was attempted.

## Emulator integration is separate work

The [documented Android Emulator modes](https://developer.android.com/studio/run/emulator-acceleration)
select host graphics or software backends, without a published PowerVR mode.
[AOSP emulator configuration source](https://android.googlesource.com/platform/external/qemu/+/refs/heads/emu-master-dev/android/android-ui/modules/aemu-gl-init/src/android/opengl/emugl_config.cpp)
has custom EGL/GLES backend library selection, so custom host integration is
conceivable. It is not evidence that a current shipped ARM64 emulator accepts
PVRVFrame, or that the Android graphics transport, EGL/native buffers and
required APIs would work with it. A standalone native GLES probe would be a
smaller compatibility experiment on a supported platform; it would not qualify
the Android guest or substitute for the actual Pixel driver.

There is also a decisive feature mismatch: Imagination's [limitations](https://docs.imgtec.com/tools-manuals/pvrvframe-manual/html/topics/limitations.html)
say separate `GL_STENCIL_INDEX8` storage does not work and recommend packed
depth/stencil. The measured Pixel ES1 path instead rejects packed storage and
successfully renders using separate depth24+stencil8. Hiding the packed
extension in a PVRVFrame profile therefore cannot faithfully replay the Pixel's
successful fallback path.

## Existing bounded renderer regression coverage

The production-helper test [drawable-depth.c](drawable-depth.c) already covers
this exact capability-selection policy, without impersonating a driver:

| Capability case | Existing assertion |
| --- | --- |
| ES1 packed extension present | Select packed depth24/stencil8 |
| ES1 packed absent, depth24 and stencil8 present | Select separate depth24+stencil8 |
| ES1 only stencil8 present | Select depth16+stencil8 |
| ES1 missing required stencil8 | Refuse private drawable storage |
| Extension name only a longer token's prefix | Do not accept it |
| Each storage recipe | Allocate/attach/resize; preserve bindings; delete once |
| Separate stencil allocation failure | Roll back new storage and retain prior drawable |

These assertions execute production includes with a bounded fake GL storage
interface. They test selection and lifetime, not actual FBO completeness or
`GL_INVALID_ENUM` from a PowerVR driver. The separate production-size native
probe already measured the real Pixel's packed rejection and separate24
successful color/depth/stencil readback. Neither result is a new virtual Pixel
qualification, and guest-created packed renderbuffers remain a separate native
capability contract. Another mirrored capability test or an identity-string
change adds no useful coverage here.

Keep ARM64 Mac Metal-backed actual guest testing and physical PowerVR actual
guest testing distinct. The practical portable regression is the existing
measured-capability helper suite plus actual driver probes and guest scenes;
PVRVFrame is currently blocked as an M4 drop-in and mismatches the stencil path
we need to exercise.
