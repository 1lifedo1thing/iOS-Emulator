# Native ARM64 validation runners

Use native ARM64 Android and Linux runners to exercise the AArch64 TCG host
code generator. An x86 Android emulator is useful for faster iteration, but it
does not exercise that code generator. Record the actual guest ABI, page size,
kernel, graphics provider and binary hashes for every result.

An ARM64 Linux VM on an Apple Silicon host can use HVF for its outer Linux CPU.
The legacy ARMv6 iOS machine inside Linux still uses TCG. Outer virtualization
does not demonstrate hardware acceleration of the iOS CPU. See the official
[QEMU ARM documentation](https://www.qemu.org/docs/master/system/target-arm.html)
and [virt CPU options](https://www.qemu.org/docs/master/system/arm/virt.html).

## Keep inputs and writable state separate

Verify an official ARM64 cloud image against its published SHA256 manifest and
retain it read-only. Create a new QCOW2 backing overlay and independent EFI
variable store for the Linux runner. Never boot or resize the original image.
Use a small NoCloud seed containing a runner-specific SSH public key, with SSH
forwarded to a loopback-only host port. Keep private keys and Apple assets
outside Git and public uploads. The [QEMU disk-image guide](https://www.qemu.org/docs/master/tools/qemu-img.html)
describes explicit backing formats; [NoCloud](https://docs.cloud-init.io/en/latest/reference/datasources/nocloud.html)
supports a local CIDATA ISO containing user-data and meta-data.

An example outer-VM resource proposal is four GiB and two vCPUs, with a sparse
root disk. It is a proposal until the owner reviews actual host resources.
Configured guest RAM underestimates possible renderer/process footprint.
Measure resident memory, Darwin physical footprint, compression, swap deltas,
disk headroom and the other active VM's CPU use. Keep a persistent Android VM
idle with no iOS worker during a Linux experiment. Build and benchmark in
separate windows. If pressure rises, stop only the owned Linux workload and
retain its evidence; preserve the primary Android runner.

Do not assume a Linux VM has hardware OpenGL because the host GPU is fast.
Inventory the installed QEMU GPU devices and actual Linux EGL provider. A
headless VM without a qualified renderer cannot provide accepted iOS menu
timings. A software-rendered CPU diagnostic is a separate explicit scope; it
does not provide an Android graphics fallback or a hardware graphics pass.

## Compare the same CPU experiment

Both arms must come from the same frozen source and dependency inputs. Change
only the reviewed experimental configure option. For the jump-cache
experiment, the controlled difference is 4,096 versus 16,384 entries; it is not
a different CPU model or a larger translated-code buffer. Rebuild native Linux
dependencies for AArch64 rather than copying x86 or Bionic libraries.

Use the existing bounded scene/counter operators after validating their
dependencies on the new runner. Preserve identical firmware/package identities,
read-only base, writable-state origin, renderer, portrait/audio/options and
instrumentation. Regenerate host endpoints and package offers for the runner.
Keep host paths out of the guest's configuration.

Inspect Home, then run a quiet measured idle window. For navigation, observe
Settings, General, Back and Home before issuing each next input. Compare the
first observed open in a fresh boot with repeated warm opens. Do not call it
storage-cold or process-cold without separate evidence; do not clear caches or
modify guest data to manufacture a cold case.

Retain monotonic input down/up receipts and image brackets. The last incomplete
and first visually complete image bound transition time; the first new frame
or frontmost-app report is insufficient. Ignore clock-only Home changes when
checking icon persistence. If a scene is late, censor it instead of blindly
tapping the next view.

Measure unprofiled behavior separately from sampling/accounting. Retain Linux
process/thread CPU ticks, elapsed time, page faults, I/O and optional sparse
jump-cache counters before/after first-open and warm phases. TCG run time
includes helpers/MMIO and possible driver waits; accounting I/O wait is not
disk I/O. Counter records from different worker/clock epochs cannot be joined.
Do not claim exact guest instruction throughput from outer dispatch counts.

Use balanced repeated arms, report raw distributions and visible behavior,
and require ordinary confirmed shutdown with original-worker retirement,
released lease and clean state. A native ARM64 Linux ranking remains distinct
from Android/Bionic performance and physical-phone acceptance.

## Qualify hardware ANGLE before app integration

Freeze actual AArch64 binaries, source/toolchain inputs, all three fixed
`*_lighttouch_angle.so` library names, ELF/DT_NEEDED/alignment checks and notice
receipts. Never substitute x86 binaries or execute a manifest with placeholder
hashes. The delivery owner must verify hashes before and after LAN/ADB transfer.

Place libraries and probes in one new private executable directory. Set only
process-local LD_LIBRARY_PATH, LIGHTTOUCH_GLES_PROVIDER=angle-gl and
LT_GLES_DRIVER_DIAGNOSTIC=1. Leave global Android driver settings and the app's
default provider unchanged. Use a device-side twenty-second timeout per raw
probe, with a short host transport margin. Record the actual exit, stdout,
stderr and crash evidence. A timeout/crash stops the sequence; it is not a pass
and does not authorize a provider fallback.

Run the raw GLES1 combine probe first. Require complete public-entry ownership
for the fixed ANGLE libraries, nonzero texture-unit capacity, restored active
selectors, complete FBO, no draw error, and actual ADD magenta255,0,255,255 for
both active0 and active1. A successful query or complete FBO alone is not enough.
Inspect the actual underlying renderer; an ANGLE label alone does not exclude
SwiftShader or another software backend.

Then run the production-adapter render probe separately. Require actual
texture/multitexture/rectangle/DrawTex and GLES2 pixels, cross-context sharing,
retained FBO attachment and deletion while the GLES1 context is explicitly
current, and its final teardown sentinel with exit0. Do not invoke GL deletion
after unbinding merely to create an invalid-use crash.

Raw shell-UID probe success is not app-UID or guest acceptance. A later sealed
APK must explicitly support the ARM64 provider through its closed selector and
packaging rules. Require actual app-UID entry ownership, recognized raw guest
and final Android images, input, lifecycle, clean shutdown and restart before
declaring the virtual device a working replacement for physical testing.
