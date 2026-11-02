# Current production-engine development baseline

Candidate29 is the compiled common ARM64/x86-64 baseline. Later repository
commits add research, tests and documentation; they do not automatically change
this APK. Candidate30/native-TLS and the engine-only Poco A/B are experiments:
the matched Poco comparison demonstrated no speedup. Real iOS guests use TCG.

| Input or artifact | Recorded identity |
| --- | --- |
| App, Portable, firmware and JNI source | `6b07201cd722602136d2207dc496faab3526d5f3` |
| Engine and bridge source | `e7a56d80b1ca59d1517c06fdddfb06cc89b377dd` |
| Main APK, 94,289,808 bytes | `c28d082f197ad1c97013062e30460179827858ccb8313f407d784afd12f6ad3f` |
| Original candidate29 test APK | `b31aa17cd1eb6c55bec11768c211aa56f53e0d5a47f21d772265ab3bbfe5025a` |
| Later test31 APK used with this main APK | `bd7723cde5d00596b9b79e2b8264ff43a9832d6077812e594d76827b7d5a8cb3` |
| Debug signing certificate SHA256 | `fcb31391994051f571c7fd2e80e68d237becdf93028fc02a6b8c42c8144d36e3` |

The actual build passed 49 JVM tests. APK checks covered all20 uncompressed,
16KiB-aligned DSOs, both packaged ANGLE identities and notices, signatures and
the exact catalog asset. Its catalog SHA256 is
`d83227ee3aa4ea509dfa09b0f68e8830b03312fcceeb18f60e85281501fdeed1`.
These are build/package results, not complete guest acceptance. A fresh-cache
replay and complete corresponding-source/relink review remain separate work.

## Rebuild the recorded baseline

Use the pinned JDK17, NDK28.2.13676358, SDK35, CMake3.22.1 and wrapper versions
in [README.md](README.md). Export the exact App revision, including `Android`,
`Portable`, `Packages/FirmwareKit/Sources/CActivation` and the tracked firmware
catalog. Stage the tracked diagnostic into its instrumentation source set:

```sh
cp "$LT_APP_SRC/Android/scripts/pixel/GuestPixelBootDiagnosisTest.kt" \
  "$LT_APP_SRC/Android/app/src/androidTest/java/org/lighttouch/android/GuestPixelBootDiagnosisTest.kt"
```

Export matching QEMU bridge headers/helper from the engine revision. Obtain the
two real Bionic engine closures, static PIC firmware dependencies, pinned XPwn
source, verified frontend notices and both ANGLE bundles using their producer
recipes. No Apple firmware or guest package is needed to compile the app.
Set the paths below to independently verified local inputs; build into a new
directory outside the checkout. The engine directories must include their
matching NDK runtime, producer manifest and notice bundle.

| ABI | Unstripped engine input SHA256 | Packaged engine SHA256 | Engine notice manifest SHA256 |
| --- | --- | --- | --- |
| arm64-v8a | `4dbfde07f43e1f385c40e2f338c27fb8f6e0ae8fb14a0a6abb65024fd061e9ba` | `cb56106b1a8f1b5ba4f5202f7120e69e7f367343145965120bb628367ee8eac3` | `2f63d9d911c3b669cdfde26dc0804fe58e6697522d5c32c58a7a2d315d0168c9` |
| x86_64 | `62d91d18a84c59a68eef5844b80da40eb61af1c65470889e286497672257ddcd` | `5b83021d4bb481118566eda1180a637dffdf374bea605cc66cb2144c5c95192f` | `021693a2bb4e2e3f3fd8c7a0173bf368ff4e0aa2851310450496610348182107` |

With `LT_APP_SRC`, `LT_QEMU_BRIDGE`, `LT_XPWN_SRC`, `LT_INPUTS` and `LT_BUILD`
set to those directories, the tested command is:

```sh
export ANDROID_HOME=/path/to/Android/Sdk
export JAVA_HOME=/path/to/jdk-17
export CMAKE_BUILD_PARALLEL_LEVEL=1
cd "$LT_APP_SRC/Android"
./gradlew --offline --no-daemon --max-workers=1 \
  '-Dorg.gradle.jvmargs=-Xmx768m -XX:MaxMetaspaceSize=384m -XX:ActiveProcessorCount=2' \
  -PnativeJobs=1 -PtargetAbis=arm64-v8a,x86_64 \
  "-PportableDir=$LT_APP_SRC/Portable" \
  "-PfirmwareDir=$LT_APP_SRC/Portable/firmware" \
  "-PqemuSource=$LT_QEMU_BRIDGE" "-PxpwnSource=$LT_XPWN_SRC" \
  "-PnativeDepsRoot=$LT_INPUTS/deps" "-PengineRoot=$LT_INPUTS/engine" \
  "-PengineNoticeArm64=$LT_INPUTS/engine/arm64-v8a/notices" \
  "-PengineNoticeX86=$LT_INPUTS/engine/x86_64/notices" \
  -PsourceRevision=6b07201cd722602136d2207dc496faab3526d5f3 \
  -PsourceReference=https://github.com/blake-lucas/LightTouch/tree/6b07201cd722602136d2207dc496faab3526d5f3 \
  "-PbuildRoot=$LT_BUILD" -Pkotlin.compiler.execution.strategy=in-process \
  "-PangleLibraries=$LT_INPUTS/angle-x86/lib" \
  "-PangleManifest=$LT_INPUTS/angle-x86/closure.json" \
  -PangleManifestSha256=b7b6988e39c227d5aa20143102fb98fce8c1a181c5dd0955b2a522c2d3199d8f \
  "-PangleNotices=$LT_INPUTS/angle-x86/notices" \
  -PangleNoticesSha256=7d28941a751e15a2f63c060b67d0c549435f87b96d527b7c2a8ff08d831bfd57 \
  "-PangleArm64Libraries=$LT_INPUTS/angle-arm64/lib" \
  "-PangleArm64Manifest=$LT_INPUTS/angle-arm64/elf-closure.json" \
  -PangleArm64ManifestSha256=3abde58ea5d9863453b12e3d7f67eff7c96ee989fbcfa4c00a16859cfd694945 \
  "-PangleArm64Notices=$LT_INPUTS/angle-arm64/notices" \
  -PangleArm64NoticesSha256=63d2221b0fab9bff7788dc0c4e9a94c74079b3ab5ed3c09bd3ef72c909d1f924 \
  "-PfrontendNotices=$LT_INPUTS/frontend" \
  -PfrontendNoticesSha256=88918185b1a3ba3ea1b4c910c8b7dde56391597574f53824a160fef45d2b76a7 \
  :app:assembleDebug :app:assembleDebugAndroidTest \
  :app:testDebugUnitTest --tests 'org.lighttouch.android.*'
```

Run the APK checker, `apksigner verify` and `zipalign -c -P 16 4` checks from
[BUILD-AND-RUN.md](BUILD-AND-RUN.md) on both outputs. Recheck packaged catalog,
notice manifests and producer-to-stripped-library correspondence. Different
signing keys or paths can change APK bytes; record the actual output identities.
No claim is made that this command rebuilds the separately identified test31.

## Install and run

Use one authorized device explicitly. Finish the current guest with Stop and
retain its confirmed native clean-shutdown receipt before updating. Preserve
the signing key and package data; do not uninstall to solve an update conflict.

```sh
LT_DEVICE_SERIAL=your-authorized-device
LT_APK="$LT_BUILD/app/outputs/apk/debug/app-debug.apk"
adb -s "$LT_DEVICE_SERIAL" install -r "$LT_APK"
adb -s "$LT_DEVICE_SERIAL" shell am start -W -n org.lighttouch.android/.MainActivity
```

Use the import/preparation instructions in [BUILD-AND-RUN.md](BUILD-AND-RUN.md)
to supply legitimate matching private assets. Select the intended board/build,
then Start. Home and Power operate guest buttons. Screenshot captures the guest
frame to Android Pictures/LightTouch; it does not press the guest screenshot
chord. Backgrounding pauses and cancels input; return and explicitly Resume.
Stop requires actual guest confirmation. A restarted session gets a fresh
worker. Ordinary Start refuses dirty state; use the explicit retained-state
recovery flow rather than deleting markers or overlays.

## Qualification limits

Physical Pixel10 rendering has scoped APK26 Lock/Home/Settings and clean Stop
evidence after capability-based separate depth/stencil handling. Candidate29
has scoped virtual16KiB Lock rendering; full virtual interaction and physical
regression remain open. Apple Metal/NVIDIA virtual GPUs do not reproduce
Tensor/PowerVR or Adreno driver behavior. Packaging both ABIs and16KiB alignment
does not prove every Android chipset or lifecycle scenario works.

The seven-row Performance panel reports the QEMU worker's CPU rate, cumulative
CPU seconds and PSS plus Android available/total RAM. Genuine iOS CPU/RAM are
unavailable. Target monitor lifecycle/overhead, complete multi-touch/concurrent
audio-render coverage, restart/persistence, the sustained20-minute phone session
and final release qualification remain open. Gyro is deferred and unqualified;
device/iOS/CPU configuration parity is a follow-on gate.
