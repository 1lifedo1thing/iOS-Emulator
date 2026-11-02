# Explicit debug ANGLE providers

The normal provider remains Android platform EGL/GLES. Debug-only provider enum
`0` selects that route; `1` requests the packaged ANGLE route in a fresh unused
worker. Packaging ANGLE does not select it, change the default or introduce an
automatic software fallback. The existing selection becomes fixed for that
worker; failures require a fresh worker after correcting the package.

The closed host ABI set is `arm64-v8a` and `x86_64`. Guest ARMv6/ARMv7 package
names do not choose this host provider. Selection accepts no library path,
environment-variable name or general loader request. The three public SONAMEs
remain fixed:

* `libGLESv2_lighttouch_angle.so`
* `libEGL_lighttouch_angle.so`
* `libGLESv1_CM_lighttouch_angle.so`

The debug JNI loader opens these names inside the app linker namespace before
guest creation. Release APKs do not stage the optional provider or expose this
debug selection. A same-host raw pixel/renderer probe is useful evidence but is
not app-UID library loading, an actual guest or a compatibility pass.

## Build inputs

Supply all five properties for each requested ABI. The existing x86 properties
remain unchanged:

```text
-PangleLibraries=/private/x86/lib
-PangleManifest=/private/x86/closure.json
-PangleNotices=/private/x86/notices
-PangleManifestSha256=CHECKED_X86_CLOSURE_SHA256
-PangleNoticesSha256=CHECKED_X86_NOTICE_MANIFEST_SHA256
```

The ARM64 inputs have independent pins and notice material:

```text
-PangleArm64Libraries=/private/arm64/lib
-PangleArm64Manifest=/private/arm64/elf-closure.json
-PangleArm64Notices=/private/arm64/notices
-PangleArm64ManifestSha256=CHECKED_ARM64_CLOSURE_SHA256
-PangleArm64NoticesSha256=CHECKED_ARM64_NOTICE_MANIFEST_SHA256
```

Either group or both may be supplied with the usual exact engine/native/source
build inputs. Each selected provider ABI must be in `targetAbis`. The fixed
ARM64 candidate is built for Android API29; the app already has minSdk29 and the
wiring refuses a lower minimum when ARM64 is selected. It does not raise the
global minimum. API28 probe executables do not lower the provider DSO's minimum.
Do not substitute an entire candidate tree containing probes for its library,
closure and notices subpaths.

All selected ELF64 shared objects must match the requested machine (183 ARM64,
62 x86-64), have bounded valid program headers and16KiB LOAD alignment, match
their fixed SONAME and allowed platform/provider dependency closure, and match
the approved closure and notice hashes. ARM64 closure metadata must declare
`"abi": "arm64-v8a"`; the historical x86 closure without an ABI field remains
accepted as x86 only. Both bundles finish validation before any generated output
is replaced. Missing, malformed, cross-ABI or duplicate inputs fail the task.

## Independent generated paths

| ABI | JNI libraries | Notices | Provider metadata |
| --- | --- | --- | --- |
| x86_64 | `jniLibs/x86_64/` | `assets/licenses/angle/` | `assets/providers/angle-runtime.json` |
| arm64-v8a | `jniLibs/arm64-v8a/` | `assets/licenses/angle-arm64-v8a/` | `assets/providers/angle-runtime-arm64-v8a.json` |

These are under the generated debug `angleRuntime` directory. The legacy x86
paths remain valid; a common APK carries separate notices, manifest identities
and ABI metadata without overwriting one group's provenance. The existing
offline licenses viewer discovers both groups. A disabled provider configuration
clears stale generated provider output.

## Source-only and package checks

Validate existing approved bundles without copying libraries or changing any
generated output, using the pinned NDK's readelf:

```sh
python3 Android/scripts/stage-angle-runtime.py /private/unused-generated-path \
  --bundle arm64-v8a /private/arm64/lib /private/arm64/elf-closure.json \
    /private/arm64/notices ARM64_CLOSURE_SHA256 ARM64_NOTICES_SHA256 \
  --bundle x86_64 /private/x86/lib /private/x86/closure.json \
    /private/x86/notices X86_CLOSURE_SHA256 X86_NOTICES_SHA256 \
  --readelf /path/to/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf \
  --validate-only
```

The single-bundle CLI `--libraries/--manifest/--notices` and two pins still
defaults to x86; `--abi arm64-v8a` selects the closed ARM64 variant. `--readelf`
is required whenever a bundle is provided. No-input normal staging clears the
disabled route; no-input `--validate-only` leaves output untouched.

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s Android/scripts/tests -p test_angle_runtime.py -v
python3 Android/scripts/check-apk.py /private/actual-app-debug.apk
```

The APK checker retains the general uncompressed-native and ELF/ZIP16KiB
alignment checks. For every packaged provider ABI it requires all three DSOs,
the corresponding ABI metadata and independent notice manifest, matching actual
ELF machine/identity, fixed dependencies and complete notice hashes. Extra,
partial, crossed or colliding provider metadata/notices fail. Historical x86
single-provider metadata remains accepted. Host tests exercise synthetic valid
and rejected packages; the JNI test is a source contract, not native compilation.

Next acceptance needs the actual matched APK, app-UID public-library loading,
recorded selected/observed provider and target GPU, real GLES1/GLES2 guest work,
input/audio/lifecycle/cleanup and required page-size coverage. Neither staging,
raw probe success nor package validation closes those gates. Preserve separate
platform-route results and all original notice/source provenance; source access
and component license review remain separate from rendering acceptance.

The matched `GuestAngleProviderTest` selects the metadata and ZIP entries for
the actual closed host ABI. `coldRecover=true` explicitly requests recovery of
the selected interrupted record; it never retries an ordinary Start as recovery.
The existing foreground QMP observer accepts `debugGraphicsProvider=angle-gl`
(or `platform`) only before a fresh worker starts, so a bounded observed guest
session can use this same packaged route. Neither argument changes the app's
normal default. Snapshot-only observation refuses a provider selector.
