# ANGLE source and provisioning route

This supplements the [application source assembly](ANDROID-SOURCE-ASSEMBLY.md)
for the explicit debug ANGLE provider. Keep its two ABI inputs distinct from
QEMU, firmware and the Android application's compiler. The source route below
was inspected for the retained hardware-GL trios; it is not a fresh build,
complete corresponding-source delivery or combined-APK license decision.
See [provider packaging](../../Android/DEBUG-ANGLE.md) for the closed loader and
[dependency coverage](../../Android/DEPENDENCY-NOTICES.md) for the wider APK.

## Retained input identities

| Input | Retained identity |
| --- | --- |
| ANGLE | `8cd050f07ebd65cce269fc92e36699fc03b20ac3` |
| Chromium build | `e60c669a94848d832d714025154f2ba1c37450df` |
| External depot_tools used to sync | `9b264039190fa270f3fc44779b4e26b43008ba41` |
| GN reported by original x86 receipt | `2579 (dc685a2b72b2)`; exact CIPD instance in original revinfo |
| Clang package | `llvmorg-24-init-7747-g62397f8b-53` |
| Compiler's reported LLVM revision | `86922d51225db089fffb99475eefc4904f79e3c8` |
| Android toolchain define in selected compilation | `ANDROID_NDK_VERSION_ROLL=2@30.0.16248370_1` |
| Target API in both trios' selected compilation | `29` |

The application/engine NDK28.2.13676358 pin does not identify this ANGLE
compiler or sysroot. Preserve ANGLE's own `source-receipt.txt`, `.gclient`,
`DEPS`, toolchain stamps and exact GCS/CIPD package instances. The original
revinfo also includes dependencies not selected by this GL build; it is a
provisioning inventory, not a list of every linked library or required notice.
Do not describe downloaded SwiftShader or Vulkan sources as an active software
renderer in these GL-only artifacts.

The retained profile is `no-abseil-perfetto`, `target_os="android"`,
`target_cpu="x64"` or `"arm64"`, with hardware GL enabled and Vulkan/null
backends disabled. It disables remote execution, Siso and thin LTO, enables
ANGLE assertions, and fixes the three `_lighttouch_angle` SONAMEs. ARM additionally
sets `arm_control_flow_integrity="none"`. Preserve actual `args.gn` per ABI
rather than recreating it from this description.

## What can be reused without copying the whole cache

The retained candidate metadata binds each trio to:

- Original per-ABI GN arguments, generated Ninja link rules, compile commands,
  direct objects and forced thin-archive members, with source hashes.
- Selected Khronos header dependencies, original license inventory and separate
  per-ABI notices/closure manifests.
- Original source revinfo, pinned Git repositories, Chromium compiler updater
  and builder recipes, GN and Android sysroot package identities.
- The actual compiler-rt builtins archives and their SHA256/member inventories;
  these are ordinary linker inputs, separate from forced whole-archive inputs.

A bounded read-only audit matched all422 distinct source paths across the two
selected manifests (417 per ABI,417 selected objects each;13,683,083 source
bytes). This establishes those selected source bytes, not every header,
generated-input or tool-source byte. Original expanded dependency inventories
remain necessary. Cache Git HEAD/remote checks identify the retained repository
objects; they do not substitute for per-file dirty-state or complete export
validation.

For the same local recipient, record readable source paths, effective UID,
manifest hashes and actual metadata reads. Preserve the shared cache and refer
to it from the private assembly receipt instead of copying its full tree.
A later recipient needs an actual accessible source/provisioning route or a
separate allowlisted source export. Exclude build outputs, credentials, unrelated
SDK packages and all guest assets from a public source export.

## Concrete remaining source/provisioning gaps

The retained `third_party/llvm/src` directory was empty. The original compiler-rt
license-origin record identifies the upstream base
`62397f8b3c3986f54187ce08f00b3448ea1f8880`; it explicitly does not establish the
exact downstream compiler/runtime source. The pinned Chromium `tools/clang`
recipe checks out the named base and applies these two cherry-picks:

```text
061865f32607cd064ab944407cc863186702d6f1
b8007a8e4020b8bca2b12e941660e10bf5bf6716
```

That recipe is useful provenance, but its presence alone does not prove it
reconstructs reported revision `86922d5` or the exact builtins archive. Retain the
package build provenance and obtain/verify the matching source through a
separately authorized provisioning step if needed. A different LLVM revision in
a generic `README.chromium` must not be silently substituted. Ordinary archive
membership also does not prove which builtins members survived extraction and
final section garbage collection in each DSO; the selected-input report marks
that limit explicitly.

No complete immutable ANGLE source export or relocated offline rebuild was
created by this audit. Complete dependency/generated-input coverage, exact
compiler/runtime provenance and an accessible source package remain distinct
from the already retained original license texts. These are specific evidence
gaps, not an assertion that the code combination is incompatible.

## Reusing the existing recipe safely

The pinned QEMU-side `tests/graphics/build-angle-android.sh` has fetch, prepare
and build stages. `--fetch-only` performs network synchronization;
`--prepare-only` runs GN; `--build-only` still calls `gclient runhooks` before
GN/Ninja. Therefore `--build-only` is not an offline/no-download guarantee.
Do not invoke any stage during a read-only inventory task.

Under a later explicit build allocation, use the recorded GN command/profile
and prepared checkout for the selected ABI, or the exact retained Ninja command
against its existing generated output. A fresh offline replay additionally
requires all package inputs/generator tools to be supplied and validated, then a
relocated fresh output with no network dependency. Preserve patches/generated
source, original and effective GN args, actual compiler/linker identities and
new output hashes. An incremental Ninja success does not prove that fresh replay.

The next application assembly must bind its final engine/app/native producer
and actual packaged ANGLE bytes independently. Do not seal it with old APK26
bindings, raw-probe results or a planned20-DSO/261-notice count. App-UID loading,
provider selection and real guest qualification are separate target evidence.
