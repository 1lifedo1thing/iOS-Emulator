# Frame cache allocation (future24, source only)

Candidate23 remains sealed with its earlier eager64MiB JNI cache. This future
revision starts with empty storage and queries `lt_runtime_frame_metadata` for
coherent published dimensions/required bytes before allocating. Metadata success
is not a pixel copy or paint; the presenter still calls real copy_frame and
commits sequence/frame/sample observations only after a validated successful copy.

The metadata query is an additive native symbol, with no existing frame struct
or version changes. Build requires Root's declaration a985c2a39fd3dc84c0369c28f38182d1e63e2ad8
and Runtime's matching implementation; do not point this JNI at candidate23's
older native source. No new build/payload/target result is claimed here.

`FrameCopyCache.latest` uses at most three metadata/copy attempts. A small buffer
returns LT_INVALID through the existing copy wrapper; it re-queries metadata,
never parses error text. `since` is a temporary for each copy and only advances
on validated LT_OK. Missing/unchanged metadata or copy AGAIN does not create a
frame. Oversize, zero/overflow/stride/format or incoherent copied metadata cannot
become a cache. Growth invalidates old frame and copied-pixel sample metrics BEFORE
changing storage, including allocation failure, so capture cannot read old
dimensions against new storage. Storage retains its largest actual frame size;
it never preallocates the64MiB integrity limit. Growth constructs an exact-sized
replacement vector and swaps under the guard instead of resize's geometric
capacity retention; observable replacement capacity must not exceed validated
required bytes. Old storage is released there before the next window snapshot.
Allocator bookkeeping/size-class overhead is not a measured RSS bound.
Both old and replacement vectors exist during growth until the swap scope ends;
the per-vector limit does not claim a64MiB total process/temporary peak limit.
Supported current panels use614400/2457600/3145728 bytes, not a promise that every
later engine has those sizes.

Only the presenter writes/grows frame storage under the JNI guard at iteration
start; capture reads under that guard. During outside-guard scale/window-lock/post,
the same sole presenter does not begin another iteration, so no writer resizes
the bytes referenced by its retained snapshot. Native Surface/lifecycle callers
change owner/window generation/epoch only; late old completion remains rejected
by the existing snapshot checks. No runtime pointer escapes the guard. A platform
window call that never returns still prevents future frame refresh; lazy storage
does not claim to fix that platform stall or make a stale screenshot fresh.

Seven production-helper scenarios are written in scripts/tests/frame_cache.cpp:
empty/no-frame allocation, exact first frame, racing size/grow/copy,
successive non-power-of-two growth without geometric capacity, AGAIN and
bounded failure without sequence advance, invalid/oversize metadata, and a
blocked old window with captured cache/new generation/capture before later growth.
These source tests use fake metadata/copy callbacks and real synchronization;
they do not produce guest rendering acceptance. Host validation of source
604c6f053a2c2a8faaf51c127811df4e5b54d85f passed all seven scenarios with
Clang -O2/-g/-Werror and ASan/UBSan, including leak checks, in1.723s. Receipt
`builds/product/frame-cache-validation-01/receipt.json` records actual commands
and source/header/tool hashes (SHA256dc71354387d54f1599e0b2718b6f20da51c058be0813a69164099ff380170e1c).
Two-ABI JNI syntax, matching runtime link and target measurements remain pending.
