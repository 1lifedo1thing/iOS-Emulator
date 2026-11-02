# Rebuilding the frame-stall diagnostic pair

The frame timeline collector needs an engine containing both read-only QOM
properties, `frame-scan-counters` and `portable-frame-counters`. A test-only APK
update cannot add those properties to an already installed engine. An older
engine produces an explicit unsupported-engine receipt.

The minimum Poco build is one fresh ARM64/API29 QEMU engine and one new
instrumentation APK. The engine's exported API and SONAME stay unchanged, so
an engine-only diagnostic Main APK can retain the existing managed code,
Portable/JNI libraries, graphics providers, firmware tools and x86 engine.
Replace the ARM64 engine and its complete verified notice bundle, then align
and sign the APK again. No JNI or firmware preparation rebuild is necessary
for this isolated diagnostic. A universal release still needs independent
qualification of every packaged ABI.

Use `scripts/repack-diagnostic-engine.py` after the native engine build and
source/notices audits have completed. It requires the exact original Main APK
hash, the clean engine revision, the engine manifest and matching notices. It
rejects a changed C++ runtime and existing output. Every unrelated APK payload
must remain byte-identical. It removes old JAR signatures, writes an **unsigned**
APK and records per-entry preservation/replacement hashes. Its result requires
`zipalign -P 16`, signing with the original certificate, and independent
signature/alignment/payload checks. It never installs or starts anything.

For example, using privately supplied paths and exact hashes:

```sh
python3 Android/scripts/repack-diagnostic-engine.py \
  --base-apk ORIGINAL_MAIN.apk --base-sha256 ORIGINAL_MAIN_SHA256 \
  --engine-root ENGINE_INPUTS --notices ARM64_ENGINE_NOTICES \
  --engine-revision EXACT_QEMU_COMMIT \
  --output diagnostic-unsigned.apk --receipt diagnostic-repack.json
zipalign -P 16 4 diagnostic-unsigned.apk diagnostic-aligned.apk
# Sign diagnostic-aligned.apk with the original certificate into a fresh file.
# Verify that certificate, zipalign -c -P 16 4, and every receipt payload hash.
```

Compile the collector from its exact committed source against the existing
pinned Main outputs. The private test-only Gradle recipe disables all Main and
native tasks and uses independent writable copies of the cached outputs. This
is compile-classpath reuse; it does not prove that source is the producer of
the original Main binary. Keep those source identities separate. Preserve the
old source packet, signed APKs and private device state.

Before target testing, bind the newly signed Main/Test hashes, actual engine
identity, unused worker PID/UID/start time and a clean isolated guest state.
Arm the fixed QMP endpoint before Start with `armFixedQmp=true` in
`GuestSessionSetupTest#armExistingUnusedWorkerBeforeOperatorStart`, retaining
the existing ANGLE and touch-wire admission. Never reuse a spent worker or
substitute old keyboard/scene bindings. For the 30-second visible-phase pilot,
keep `keyboardVisiblePhases=true`, `captureFrameTimeline=true`,
`requireConcurrentMediaWindows=true`, `requireFrameProgress=true` and
`requireGuestAudio=true`. Supply the newly inspected keyboard binding.

Retain the raw QMP transcript, exact counter bodies, timeline text, native
images, input/audio/frame samples and strict Stop receipt. Inspect the actual
images and compare scan, dirty-update, publication, native acquisition and
presentation boundaries. The counters alone do not prove guest pixels changed,
UIKit consumed input, audible playback, or sustained acceptance. Keep all
original overlap predicates and deadlines; a failed pilot stays failed. A
30-second diagnostic does not complete the required 20-minute session.

`verify-diagnostic-output.py` provides independent read-only output checks.
Its `engine` mode consumes the exact approved native plan, requires a complete
terminal receipt, and rechecks the frozen ELF ABI/exports/dependencies/SONAME,
16 KiB alignment, API and source bindings, C++ runtime, complete notice inventory
and complete sourcekit inventory/modes. It does not relink or execute the engine.
Its `apk` mode checks every original unrelated payload and replacement against
the repack receipt, requires the independently verified engine hash, checks the
original signing certificate and V2 signature, then verifies all APK native
alignment and packaged ANGLE identities through `check-apk.py`.

```sh
python3 Android/scripts/verify-diagnostic-output.py --output native-review.json \
  engine --plan NATIVE-BUILD-PLAN.json --plan-sha256 APPROVED_NATIVE_PLAN_SHA256
python3 Android/scripts/verify-diagnostic-output.py --output apk-review.json \
  apk --original ORIGINAL_MAIN.apk --signed diagnostic-signed.apk \
  --receipt diagnostic-repack.json --engine-sha256 VERIFIED_ENGINE_SHA256 \
  --apksigner SDK_BUILD_TOOLS/apksigner --zipalign SDK_BUILD_TOOLS/zipalign \
  --certificate-sha256 ORIGINAL_CERTIFICATE_SHA256
```

A successful artifact check records that target execution remains unverified.
Strict guest correctness, concurrency, clean Stop and persistence evidence
remain separate requirements.

If the original APK contains a stripped C++ runtime, pass the pinned NDK
`llvm-strip` as `--strip-tool`. The packager must reproduce the existing APK's
runtime bytes exactly using `--strip-unneeded`; a different result still fails.
The original C++ payload remains unchanged. The receipt records the tool and
input/output hashes, alongside the original unstripped runtime's notices.

Use explicit diagnostic signing flags `--v1-signing-enabled false
--v2-signing-enabled true --v3-signing-enabled false --v4-signing-enabled false`.
This keeps the exact ZIP payload closure and lets the independent check require
a verified V2 signature with the original debug certificate. V1 adds JAR members;
default V3 verification can otherwise leave the V2 verification result false.
These flags apply to this diagnostic recipe.
