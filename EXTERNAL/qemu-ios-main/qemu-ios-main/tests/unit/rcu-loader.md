# Shared-entry RCU ownership regression

On Linux, use a configured native QEMU build with `libqemuutil.a` and
`compile_commands.json`. Run from any directory, with a new output directory:

```sh
python3 tests/unit/run-rcu-loader.py /path/to/native-build /path/to/new-results
```

The runner uses that build's production compiler flags and actual `util/rcu.c`.
An ordinary pthread loads a test DSO and exits. Function instrumentation counts
registration pairing without inspecting or modifying registry memory. The
standalone DSO requires explicit loader cleanup; the embedded DSO leaves the
loader unregistered and runs the real `qemu_ios_main` with a minimal VM harness.
Both verify reader participation in grace periods and child registration after
fork. The embedded test checks owned registration and preserves a caller's
existing registration through initialization, loop, and cleanup.

The shared-entry marker is hidden and resolves within its own ELF object. A
different DSO cannot change standalone constructor ownership. Non-Linux entry
behavior is unchanged. The production shared-library relink must include
`qemu-ios-entry.c`, replacing the standalone `system/main.c` object.

These tests check host thread ownership. They do not establish the identity of
a phone crash's loader thread or qualify Android guest boot, rendering, input,
audio, or lifecycle behavior. Keep the DSO mapped until process exit because
its `call_rcu` worker is permanent. The runner retains commands, output, status,
and timing in the result directory and bounds each subprocess to 25 seconds.
