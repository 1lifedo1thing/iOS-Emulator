# Android start and explicit cold recovery

`Native.start(root, recordId)` opens the selected device with `allow_dirty=0`.
A retained dirty marker therefore rejects ordinary startup through the existing
native `LT_DIRTY` error. `Native.recoverStart(root, recordId)` is a separate entry
point that opens with `allow_dirty=1` after an explicit recovery action. The two
entry points call the same private startup helper; neither exposes a caller
option for bypassing checks or supplying engine/driver paths.

Recovery admits the existing writable state. It does not remove its dirty
marker, reset overlays, import another base, copy assets, or claim a clean prior
shutdown. Both routes retain the same library selection, ROM/package checksums,
prepared-base integrity pin, exclusive device lease, board discovery, startup
diagnostics and fixed graphics-provider selection. Native failure and startup
pause/Stop handling are shared. The Android service still owns the fresh-worker
guarantee; changing admission does not permit another engine session in a
consumed process.

The existing shared native contract tests cover dirty-state refusal, explicit
recovery admission and preservation of the marker and legacy writable data.
They establish storage behavior, not Android recovery acceptance. This JNI
checkpoint was prepared without a compiler or device allocation.

Before packaging, syntax-check this JNI translation unit with both frozen NDK
compile commands and verify the matching Kotlin declarations/service routing.
On a target using a private interrupted writable record, retain these separate
results:

1. Ordinary Start in a fresh worker refuses dirty state, leaves its marker and
   overlays intact, and does not start the guest.
2. The explicit recovery action creates a fresh worker and admits that same
   record through `Native.recoverStart`, with the expected engine/provider
   identities. Inspect actual guest pixels and a previously saved user-state
   marker before calling recovery verified.
3. Orderly Stop independently confirms native terminal state, removal of the
   dirty marker and preservation of that user state. A forced disposal or
   missing shutdown receipt remains unconfirmed.

The product owner implements the fixed recovery UI/Binder/Kotlin route and its
opt-in instrumentation separately. No target pass follows from these source
entry points alone.
