# Candidate22 development build and run

Candidate22 fixes audio resumption after a newly granted focus request: the
retained AudioTrack starts before the audio thread can observe the new request.
Its actual-device regression qualification is in progress. It retains the
candidate21b engine and all63 tracked native source inputs.

| Input/artifact | Verified identity |
| --- | --- |
| Android source | `05d6a5ef06a2a05d3055fd3307c3327644ae2c22` |
| Native source | `d939543dc9084b455d8fe0b3d44dccbe024acd87` |
| QEMU producer | `b59b3620b01da9285dea3a9ee6f58c1bb3cecfc2` |
| Main APK SHA256 | `2288bd375856b528fc6d4a12efae7dd6f447a54e90d296d6397e741c99150628` |
| Matched test APK SHA256 | `cd95812d5501daccbb28feade63f45fce07808b1c2f97220604caeba53249b4b` |

The matched main/test build completed in58.195seconds with20 JVM tests passing.
Independent checks covered17 packaged native libraries, their input/strip and
dependency closure,247 notice payloads, signatures and16KiB ELF/ZIP alignment.
These checks do not prove16KiB guest execution, audible playback or performance.
Debug optimization is scoped to JNI and Portable `Runtime.cpp`; symbols and
assertions remain enabled. Other domain files retain their existing policy.
QEMU was already optimized in candidate20/21b; this audio fix does not change it.

## Build

Use the [pinned tools and verified build inputs](BUILD-AND-RUN.md#inputs-and-build).
Export Android and `Portable/firmware/firmware-catalog.json` from the Android
revision above. Export `Portable` and `Packages/FirmwareKit/Sources/CActivation`
from the native revision. Export the QEMU bridge headers and fixed diagnostic
helper as described in that guide. Do not omit the catalog or CActivation.

With the verified paths assigned, this is the executed recipe with local paths
parameterized. Use a new build directory; do not overwrite older candidate seals.

```sh
export ANDROID_HOME=/path/to/Android/Sdk
export JAVA_HOME=/path/to/jdk-17
export CMAKE_BUILD_PARALLEL_LEVEL=2
LT_ANDROID_SRC=/path/to/android-05d6a5ef-source
LT_NATIVE_SRC=/path/to/native-d939-source
LT_QEMU_SRC=/path/to/qemu-b59-bridge-source
LT_INPUTS=/path/to/verified-build-inputs
LT_XPWN_SRC=/path/to/pinned-xpwn-source
LT_BUILD=/path/to/private/new-android-build
LT_APP_REVISION=05d6a5ef06a2a05d3055fd3307c3327644ae2c22
cd "$LT_ANDROID_SRC/Android"
./gradlew --offline --no-daemon --max-workers=2 \
  '-Dorg.gradle.jvmargs=-Xmx768m -XX:MaxMetaspaceSize=384m' \
  -PnativeJobs=2 -PtargetAbis=arm64-v8a,x86_64 \
  "-PportableDir=$LT_NATIVE_SRC/Portable" \
  "-PfirmwareDir=$LT_NATIVE_SRC/Portable/firmware" \
  "-PqemuSource=$LT_QEMU_SRC" "-PxpwnSource=$LT_XPWN_SRC" \
  "-PnativeDepsRoot=$LT_INPUTS/deps" "-PengineRoot=$LT_INPUTS/engine" \
  "-PengineNoticeArm64=$LT_INPUTS/engine/arm64-v8a/notices" \
  "-PengineNoticeX86=$LT_INPUTS/engine/x86_64/notices" \
  "-PsourceRevision=$LT_APP_REVISION" \
  "-PsourceReference=https://github.com/blake-lucas/LightTouch/tree/$LT_APP_REVISION" \
  "-PbuildRoot=$LT_BUILD" \
  "-PangleLibraries=$LT_INPUTS/angle/lib" \
  "-PangleManifest=$LT_INPUTS/angle/closure.json" \
  -PangleManifestSha256=b7b6988e39c227d5aa20143102fb98fce8c1a181c5dd0955b2a522c2d3199d8f \
  "-PangleNotices=$LT_INPUTS/angle/notices" \
  -PangleNoticesSha256=7d28941a751e15a2f63c060b67d0c549435f87b96d527b7c2a8ff08d831bfd57 \
  "-PfrontendNotices=$LT_INPUTS/frontend" \
  -PfrontendNoticesSha256=88918185b1a3ba3ea1b4c910c8b7dde56391597574f53824a160fef45d2b76a7 \
  :app:assembleDebug :app:assembleDebugAndroidTest \
  :app:testDebugUnitTest --tests 'org.lighttouch.android.*'
python3 "$LT_ANDROID_SRC/Android/scripts/check-apk.py" \
  "$LT_BUILD/app/outputs/apk/debug/app-debug.apk"
```

## Install and run

Verify the APK hash and signature, then install the main APK with
`adb -s DEVICE_SERIAL install -r app-debug.apk`. Install the matched test APK only
when intentionally running instrumentation. Finish an orderly Stop before an
update; a process exit alone does not establish clean state.

For the verified n72 prepared-base archive, select ROM and matching ARMv6 guest
tools **before** Import base. Select the imported record and Start. Import creates
fresh writable state; existing selected records retain their state on an update.
For n81, follow its complete prepared kboot/NAND/NOR/overlay route and matching
ARMv7 package; no separate hardware boot ROM is required. These assets stay private.

Foreground MainActivity before each intentional `--no-restart` instrumentation
call. Closing ActivityScenario normally pauses the guest; use Resume afterward.
`GuestUnlockTest` remains sealed to candidate19 and must not run against22.
Host Screenshot exports a host image; Home+Power creates an in-guest screenshot.

Gyro/motion remains deferred and unqualified. Linux/Android lifecycle, full
multi-touch/concurrent media, sustained measurements and remaining target
qualification stay open. Pixel10's candidate21b demo is not working/qualified;
its reported flicker/black behavior awaits actual ADB diagnostics. Notices and
selected source materials do not establish complete public distribution clearance.
