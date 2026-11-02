# n81 prepared-kboot source audit

The candidate18 Android/domain source accepts a prepared `n81ap` base without a
hardware ROM. This is source routing support, not an Android n81 boot, display,
input, audio, durability or firmware-preparation pass. Initial stock-preparation
acceptance remains n72; motion/gyro remains deferred and unqualified.

Native validates format1 `device.lock.json`, its authoritative NAND listing,
`outputs.kboot` for `kboot.bin`, and `outputs.nor` for `nor.bin`. Preserve all NAND
stores, geometry and identity under their supplied names. The firmware owner's
current-route audit found no metadata conversion necessary for the supplied base;
its earlier Mac receipts do not substitute for actual Android native admission.

Runtime selects `iPod-Touch-4G` from engine board facts. The domain composes
`kboot=<base>/kboot.bin`, `nand=<base>/nand`, `nand-overlay=<state>/nand` and
`nor-rw=<state>/nor.bin`, plus the validated identity and freshly composed guest
offer. It does not add n72 hardware ROM/GID inputs or `-m128M`. Normal Start and
explicit dirty-state recovery use this same recipe in a fresh dedicated worker.
Fixed debug hooks do not select a different board recipe.

Android's Start control depends on record/session/dirty state, not
`bootromAvailable`. JNI treats an absent readable ROM as optional, and only the
n72 domain route requires it. A cached optional ROM can still be copied into an
n81 record by the generic importer; the n81 boot recipe does not use that ROM.

Select the supplied matching raw **armv7 `.itpack` before importing the base**.
The worker copies its current app-private `guest.itpack` into the new immutable
record. A leftover n72 package can therefore be attached to an n81 import and fail
the native board/build-family check during Start. The picker changes inputs for
future imports; it does not repair an existing record's assets. Do not follow an
absolute historical Mac package path in metadata or copy a prior guest offer.
Native builds a new private offer and validates its selected payloads. If the
base declares a hashed `outputs.guest_package`, the prepared-output import path
can discover it; this is distinct from the supplied base's external raw package.

The current Android API starts library record IDs, not an arbitrary read-only
base plus separate state. SAF folder import first copies the base into app-private
staging, then native library import validates and copies it into a new record.
The supplied large fixture consequently needs substantial additional storage and
strict all-byte admission; the UI does not expose the Linux direct-base no-copy
route. Do not launch an unallocated full copy/hash, thin the fixture, alter its
lock, replace an existing record or bypass validation to hide this constraint.

A future allocated Android run must retain exact candidate/native/engine pins,
matching package provenance, new record and writer lease, full admission result,
actual640x960 frames and guest responses, shutdown confirmation and independent
writable-state persistence. Capability rows and synthetic kboot/no-ROM contract
tests establish neither that run nor actual guest delivery.
