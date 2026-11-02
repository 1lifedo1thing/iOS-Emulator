# Modern Pixel debug qualification

This lane follows satisfactory Poco operation and an explicit allocation from the
integrator. It prepares Pixel 8/9/10 compatibility work; no modern Pixel is
currently qualified. The Pixel 10 demo report describes logo/loading-bar flicker,
frame increments, then black/idle with brief flicker on Home. These observations
do not establish a native process crash.

The installed SDK supplies `pixel_8`, `pixel_9`, and `pixel_10` device profiles.
An AVD combines a profile, system image and separate writable storage. Its profile
does not turn an x86_64 emulator into the physical Tensor CPU/GPU. See the official
[AVD configuration documentation](https://developer.android.com/studio/run/managing-avds).

## Prepare without taking a device or heavy allocation

Run the read-only inventory against the exact APK chosen by the integrator. Store
the resulting receipt outside Git. It checks the APK digest, installed device
profiles, image metadata, usable emulator acceleration and debug tools; it prints
creation commands without executing them.

```sh
python3 Android/scripts/plan-pixel-debug.py \
  --sdk /home/blake/Android/Sdk \
  --private-root /home/blake/Documents/LightTouch-Development/port-workspace/builds/pixel/avds \
  --apk /home/blake/Documents/LightTouch-Development/port-workspace/builds/product/dual/attempt-22/app-debug.apk \
  --apk-sha256 2288bd375856b528fc6d4a12efae7dd6f447a54e90d296d6397e741c99150628 \
  > /home/blake/Documents/LightTouch-Development/port-workspace/evidence/pixel/prerequisites.json
```

Tool paths in the receipt include ADB, avdmanager, emulator, sdkmanager, APK
signature/alignment checkers, LLDB, and x86_64/ARM64 simpleperf. Their presence does
not establish working target profiling; retain actual profiler errors and reject
zero-sample attribution. API35/default/x86_64 revision2 and
API36/google_apis_ps16k/x86_64 revision7 were installed at preparation. The API36
image is the initial modern Android/16KiB test. The API35/4KiB image is a control
that also changes OS/API, so a difference cannot alone isolate page size. An
API36/4KiB comparison would require a separately installed and pinned image.

## Allocated virtual attempt

Create one new named AVD from the receipt, using its private path and `no` on
stdin. Do not use `--force`, reuse existing graphics-lane userdata, or wipe other
AVDs. Start with `LightTouch_pixel_10_API36_16384`; Pixel 8/9 profiles primarily
add geometry/inset/rotation coverage. Repeat virtual profiles only where those
differences justify the work.

Use the existing bounded `scripts/supervise_16k_retry.py` under the integrator's
exclusive emulator/heavy allocation with 4 cores, 4096MiB, expected16384 pages,
and a fresh library. Its fixed240-second Android UI gate,840-second Stop and
900-second disposal bounds apply to a new attempt. Readiness is not guest
acceptance. Reserve `emulator-5554`; the supervisor rejects an occupied target.
Preserve every failed receipt and userdata. The current matched diagnostic defaults
to `pixelGraphicsProvider=platform`, exercising Android's installed EGL/GLES stack
as the phone application does. Use `angle-gl` only as an explicit separate fallback.
Record requested and observed provider, fixed selection, renderer and page size;
selection failure or drift must fail rather than silently change providers. Earlier
ANGLE success remains scoped to that producer and does not qualify platform or
physical Pixel graphics. The host and matched runner contract is documented in
[scripts/pixel/README.md](scripts/pixel/README.md).

For current tests, retain the running Android instance and attach a separately
bounded guest attempt; do not restart Android between guest tests or interrupt a
user-owned interactive session. Switching the inner provider requires a fresh
LightTouch worker, not an Android restart. Use a genuine current clean-shutdown
receipt or explicit dirty-state recovery for that worker's admission.

This supervisor currently launches `-gpu swiftshader` and `-no-audio`. SwiftShader
is a software graphics backend; KVM accelerates the Android x86_64 machine, while
the iOS guest CPU still uses QEMU TCG. The no-audio attempt can collect Android PCM
counters but cannot establish physical speaker output. An allocated follow-up
audio run needs an explicitly reviewed launch change enabling host audio before
an audible-playback gate can pass. These limitations must appear in the receipt.
The current Linux VM exposes no virtio 3D capabilities and its EGL renderer is
llvmpipe. `-gpu host` is an alternate software driver route on this machine and
cannot reproduce a Tensor GPU. Platform mode may lack required GLES1 extensions;
retain its precise failure and keep explicit ANGLE fallback evidence separate.
A virtual GLES version advertisement alone does not qualify guest rendering.

Rehash and install the frozen main APK and a compatible test-only runner. Import
verified compact baseline/ROM/package into a fresh record, preserve the base
read-only, and create independent writable state. Record all input hashes and
the actual worker UID/PID/start time. Generate target-specific host endpoints and
package offers; do not copy Mac boot endpoints or live Poco state. Keep
foreground boot observation continuous and inspect raw guest plus Android surface
captures at meaningful changes. Logo, black, provider loaded or an incrementing
frame counter do not prove Home.

Required results include actual Home and app rendering, two-contact guest
pinch/crossing/lift/cancel, concurrent render/audio/input, orientation and
background/surface lifecycle, strict clean Stop and same-state fresh-worker
persistence. Record measured launch/menu intervals, frame/input/audio counters,
and native/platform logs. A sustained usable session must meet the integrator's
shared acceptance gates. Gyro remains explicitly deferred and unqualified.

## Physical Pixel follow-up

After the user connects a Pixel, obtain an exclusive phone allocation. Capture
the actual model/build fingerprint, ABI, page size, GL vendor/renderer/version,
installed APK identity, worker identity, boot log, framebuffer/presenter state,
process exit status and any crash/tombstone evidence accessible without changing
device security. Compare fresh versus retained state in isolated records. First
determine whether logo flicker is guest display/sleep behavior, a guest reboot,
or a host worker crash; diagnose from evidence before changing APK flags.

Current ARM64 uses the platform graphics provider; the packaged experimental
ANGLE provider is x86_64 only. Virtual ANGLE success cannot qualify ARM64 platform
rendering on Pixel. A separate Pixel APK is warranted only if a validated
compatibility change or packaging requirement calls for it. Static APK/ELF16KiB
alignment already checks one prerequisite; runtime verification is still needed.
See Google's [16KiB testing guidance](https://developer.android.com/guide/practices/page-sizes).

Qualify each tested physical model/build/page size precisely. An emulator pass or
one physical model pass does not advertise support across Pixel 8/9/10. Keep
firmware, captures, profiler output, APKs, AVD userdata and user state private and
outside Git/public uploads. No phone flashing or unrelated wipes are part of this
lane.
