# CPU backend experiments

Real iOS guests currently use QEMU TCG on Linux and Android. TCG translates guest
ARM instructions into host instructions. Hardware EGL/GLES accelerates graphics;
it does not replace CPU translation. The default-off Dynarmic research adapter
has synthetic correctness results, not a qualified iOS or Android CPU backend.

## Poco native TLS comparison, 2026-10-15

One sequential, matched A/B experiment compared the production engine's emulated
thread-local storage with an API29 native-TLS engine. APK contents were identical
apart from the ARM64 QEMU engine and corresponding notices. Each run began from
an independent copy of the same warmed writable state, reached Home, opened
Settings twice and ended with a confirmed native clean shutdown. The original
APK and complete original state were restored afterwards.

| Engine | First complete Settings screen | Repeated complete Settings screen |
| --- | --- | --- |
| A: production TLS | 4.503–5.401 s | 4.506–5.418 s |
| B: native TLS | 4.504–5.393 s | 5.004–5.898 s |

These are conservative host input/screenshot intervals. The preceding captures
show incomplete Settings content; the positive captures show its complete title
and visible rows, continuously through the remaining observation window. The
comparison protects the visible Settings content while excluding the status
clock and the exact observed transient scrollbar rectangle. The original
settled-reference measurement also waited for scrollbar fade and must not be
reported as app-loading latency. "First" here does not mean cold storage or a
universally cold translation cache.

The intervals overlap: this pair demonstrates no native-TLS speed gain and does
not justify changing the default. Twenty-second aggregate worker CPU counters
include substantial time after the menu appears, supporting threads and observer
overhead; they are not launch CPU time, guest utilization or instruction
throughput. One pair cannot establish thermal effects or general performance.
An earlier interrupted attempt failed shutdown and remains a failed attempt.

Artifact SHA-256 prefixes: A `1d387d3ca42d16c5`, B `10a618661b7bd570`;
common instrumentation `7a58d503a56a3214`. Private raw captures, command intervals,
state inventories and independent review remain outside Git in the integration
workspace's `builds/product/poco-ab-successor-A-actual-01/`, including
`OFFLINE-COMPARISON.md` and `ROOT-FINAL-MATCHED-REVIEW.json`.

This experiment does not isolate translation, MMU lookup, guest application work,
storage, scheduling or rendering as the cause of the slow menu. It also does not
qualify multitouch, concurrent media, lifecycle, monitor overhead or a replacement
CPU engine. Those target checks remain required.
