# Scoped upstream TCG fixes

The October 14, 2026 upstream review found guest page and translation fixes,
not a new Android host 16 KiB allocator or graphics-provider change.

| Upstream QEMU commit | Imported behavior |
| --- | --- |
| `57da5d7347061971b8d0c930c864a45c0a33b388` | Remove n72 CP15 field offsets that wrote into CPU/TLB memory; use state-free maintenance operations and callback-only QEMU_CALL |
| `909d3ccb62658e3074e4cd1835a7669ef2c5e54f` | Use 4 KiB **guest** pages on supported iPod/iPhone/iPad boards; reject ARMv6 fine-page descriptors |
| `d0ffac78801f0a166a17c5964ec5a1a1f0b2d301` | Remember MMIO instruction boundaries so repeatedly polled reads can end a translation block without repeated unwind |
| `f578200af70f26206aa42b1478e24e617b692210` | Echo EA/EB touch-read commands while preserving prefix checksums and report contents |

The portable fork additionally retains the minimum cached instruction limit
under the existing mutex, including different TB variants at one physical PC.
The upstream insertion could overwrite a shorter limit with a longer one.
The import preserves the existing portable backend, RCU ownership and AP-wake
button fixes. Producer manifests identify the actual fork revision in each
build; upstream commit hashes alone are not binary provenance.

LightTouchMac `51136956066356c1ade7e04f7d7e6c10a4439fb1` only advances its
Mac QEMU dependency pin to `d0ffac7880`. Its reported Mac performance and
session results are reference evidence. There is no Android/Linux source hunk
to transplant from that commit. The existing Mac source pin in
`build-support/sources.json` stays unchanged; Android/Linux use the explicitly
supplied portable engine and its sealed manifest. Advancing that Mac pin to
upstream main would include unrelated model, gyro and firmware recipe changes.

## Verification

Run the production CP15 descriptor slice and the portable monotonic-limit
regression with ASan/UBSan. On a newly built native Linux QEMU, run
`tests/ipod/tcg-pages-io-test.sh`: a synthetic bare-metal program must pass the
1 KiB boundary without a translation block beginning at `0x400` and complete
1,000 polled device reads with at most four rewinds. These tests establish
mechanism behavior, not prepared-iOS guest acceptance.

Rebuild the native Linux and both Android ABI engines from clean committed
source. Retain their producer manifests and frozen previous candidates.
Android 16 KiB host support still requires packaged ELF/ZIP alignment,
actual app-UID library mapping and guest rendering on a 16 KiB target.
Real Linux/Poco boot, multi-touch, concurrent audio/render/input, lifecycle,
safe shutdown/restart and persistence remain required on the new candidate.
Gyro and guest motion qualification remain deferred and unqualified.

The final clean backend producer is
`b59b3620b01da9285dea3a9ee6f58c1bb3cecfc2`; the integration commit is
`a4749f1ba6`. Native Linux, arm64-v8a and x86_64 builds passed. Native Linux's
bare-metal test observed no block at `0x400` and 2 rewinds across 1,000 reads.
Production-function ASan/UBSan regressions passed for CP15 descriptors, minimum
limits, explicit-count replay, full-flush reclamation, touch frame handover in
Apple/portable variants and retained AP-wake behavior; RCU ownership tests passed.
Both Android engines retain `0x4000` PT_LOAD alignment. Full-flush reclamation
bounds hint lifetime, and explicitly counted replay blocks bypass the hint.
These results establish builds and mechanisms. Final-engine Linux/iOS guest and
Android guest acceptance remain separate, pending target evidence.
