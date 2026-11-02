# ARM64 Android hardware ANGLE-GL candidate

The existing GL-only recipe can target `arm64` using a separate output directory. It builds ANGLE GLES1 and GLES2 frontends over Android GLES3 through the public EGL/GLES resolver. It does not enable a software renderer or change the platform default.

The pinned candidate uses ANGLE `8cd050f07ebd65cce269fc92e36699fc03b20ac3` and Chromium build `e60c669a94848d832d714025154f2ba1c37450df`, with Abseil and Perfetto disabled. The actual cached compiler targets Android API29 for these libraries. Original x86 outputs are retained separately.

Prepare with `ANGLE_PROFILE=no-abseil-perfetto` and `build-angle-android.sh --prepare-only CHECKOUT arm64 gl`; inspect the generated graph before invoking Ninja directly. A direct cached Ninja invocation avoids hooks or downloads. Keep a bounded external owner/resource supervisor around compilation.

Verify the actual outputs with `inspect-angle-android.py --abi arm64`. The original default remains x86_64. The ARM64 candidate has three fixed suffixed SONAMEs, AArch64 ELF64 little-endian objects and 16 KiB LOAD alignment. Its public Android dependencies are libc, libdl, libm, liblog and libnativewindow. These are offline checks, not Android loader or graphics acceptance.

Use expanded `ninja -t compdb -x` records with `angle-selected-inputs.py`; the GN compile database lacks output fields. Record actual thin-archive members and included headers, then generate a separate schema1 notice bundle. The first ARM64 candidate selected 417 direct/whole-archive objects and no Abseil/Perfetto headers. Incidental Vulkan-Headers are inventoried even though the Vulkan renderer is disabled. Ordinary compiler-rt archive member inventory does not establish exact linker extraction; full combined-application licensing and corresponding-source distribution remain separate reviews.

Before packaging, use the same frozen ARM64 raw probe under platform and explicit `angle-gl`, with only process-local library lookup. Require actual hardware backend identity, verified public entry owners, both two-unit ADD pixels, complete FBOs and valid-current-context deletion/teardown. Run full render and pixel probes only after the preceding stage passes. Preserve failures. Build and ELF success alone do not qualify the provider, app UID or actual guest.
