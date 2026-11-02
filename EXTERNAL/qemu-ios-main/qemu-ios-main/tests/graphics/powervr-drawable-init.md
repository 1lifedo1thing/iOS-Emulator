# Native drawable initialization diagnostics

An Android platform GLES1 driver has returned the renderer's generic `FBO
incomplete` message during a real guest boot. That message follows successful
EGL context creation and required OES framebuffer capability validation. The
following `REFUSED host:no-context` is the renderer's latched initialization
failure; it does not establish failed EGL creation or a guest reboot.

The Android-native failure diagnostic retains the first setup error/stage,
color-only and final framebuffer status, actual dimensions/object names/API,
renderer/version and packed-depth/NPOT extension flags. It preserves GL errors
using the existing per-context store. It has no per-draw logging or changed
fallback policy. Mac/CGL/EAGL diagnostic behavior remains unchanged.

`egl-drawable-init.c` compiles the production EGL and native adapters. Its
default recipe is the real n72 drawable's320x480 RGBA/unsigned-byte texture,
LINEAR filtering, default REPEAT wrapping, private high object names and packed
depth24/stencil8 attached to both points. Earlier32x32 triangle probes did not
cover that NPOT allocation. Each invocation has a20-second alarm and retains
the first failing operation. A full PASS requires actual green depth/stencil
writes followed by blue depth/stencil equality readback, not a COMPLETE enum.

Build this one probe with the same NDK/link flags as `check.sh`; it depends on
`gles-host-egl.c.inc`, `gles-host-native.c.inc`, `gles-host-rectangle.c.inc`,
`gles-host-driver.c.inc` and `gles-host-driver-entries.c.inc`. Link only public
EGL/GLESv2/GLESv1_CM/dl/libc and use16KiB ELF alignment. Preserve source/binary
hashes, stdout/stderr, real exit and actual driver/context identity per attempt.
No host, shell or run-as probe by itself qualifies an application guest.

Run the default first, then these one-factor comparisons using the SAME binary:

| Arguments | Isolated difference |
|---|---|
| none | Production320x480 recipe |
| `--size=32x32` | POT allocation matching earlier probes |
| `--generated` | glGen names instead of first-bind private names |
| `--clamp` | CLAMP_TO_EDGE for both private texture wrap axes |
| `--separate` | Separate depth16 and stencil8 renderbuffers |
| `--separate24` | Separate depth24 and stencil8, preserving depth precision |

The probe reports UID/eUID, SELinux context, actual vendor GL/EGL mappings,
renderer/version/extensions and each operation's real GL error. A run-as UID
match alone does not prove the same application namespace/driver selection;
compare those receipts with the unchanged real app. Preserve denied execution
or differing identity as limitations. The probe does not select any production
format, silently retry or downgrade guest capabilities.

`DEPTH24_STENCIL8` is supplied by the optional
[OES packed-depth/stencil extension](https://registry.khronos.org/OpenGL/extensions/OES/OES_packed_depth_stencil.txt).
[OES framebuffer objects](https://registry.khronos.org/OpenGL/extensions/OES/OES_framebuffer_object.txt)
alone do not guarantee that format. GLES1 NPOT image allocation also needs
appropriate extension support; limited NPOT texturing restricts wrapping/filtering.
However, texture sampling completeness and framebuffer attachment completeness
are distinct. The [EXT framebuffer attachment rules inherited by OES](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_framebuffer_object.txt)
do not require general texture sampling completeness. Therefore a wrap-mode
comparison is diagnostic, not proof that the original framebuffer violated a
specification. Likewise, no generated-name workaround is justified solely by
the old generic failure. The actual operation/error/status/capabilities must
select the next change.

The default and comparisons are source/build diagnostics until actually run.
Guest rendering, visible changing scenes, lifecycle and normal Stop remain
independent acceptance gates.


The physical PowerVR ES-CM1.1 probe isolated an unsupported packed storage enum:
color-only COMPLETE; packed allocation INVALID_ENUM; separate24 + stencil8
COMPLETE and real green/blue depth/stencil readback PASS. Separate16 also passed;
size32/generated/clamp did not cure packed storage. Those are shell-UID feature
results with matching vendor DSOs, not packaged application acceptance.

The production native-only selection now preserves packed24+8 where advertised
(or ES3 core). Otherwise it uses separate depth24 when OES_depth24 is present,
depth16 when it is absent, plus stencil8 (GLES1 requires OES_stencil8; ES2 core
supports it). Missing required stencil capability fails explicitly. There is no
vendor-specific retry and no extension/guest renderbuffer emulation. Mac/EAGL
keeps packed storage, including drawable resizing after old desktop snapshots.
Native snapshots remain explicitly unsupported.

Both initial allocation and drawable resize use the same production allocation
and attachment helpers. Allocation leaves depth bound as before. Resize commits
only after complete/error-free storage, replaces private bindings, preserves
unrelated guest bindings, and deletes both old separate names exactly once.
Failure deletes temporary storage and preserves the old drawable/bindings.
Checked-context teardown deletes a separate stencil only after acquiring the
correct context. A failed initialized context remains latched until its existing
context-destruction/new-context path, without a new retry policy.

Target-free checks (not driver acceptance): compile `drawable-depth.c` both with
and without GLES_HOST_EGL_ES using -O2/-Werror/ASAN/UBSAN; compile
`egl-context-cleanup.c` with GLES_HOST_EGL+GLES_HOST_EGL_ES, GLES_HOST_EGL only,
and neither. They exercise the actual production resize/allocation/deletion
functions: packed/separate24/separate16, ES1/ES2/ES3 capabilities, exact extension
matching, initial binding, private stencil rebinding, unrelated guest binding
preservation, failed stencil rollback/retry, new-context lifetime, and rejected
cleanup binding. Full engine and normal-app-UID PowerVR guest retest remain
required for the changed producer.

Guest support is separate: source `contrib/it-gles/mbxshim.c` advertises no packed
extension. `gles1x.c` forwards core extension strings and adds only the existing
rectangle/CoreSurface extensions. GLES_SLOT_RENDERBUFFER_STORAGE continues to
forward the guest format through gles_rb_format (packed is not translated), so
an unsupported guest-created packed buffer retains the real native error. This
source audit does not establish the exact strings of every frozen guest binary.
