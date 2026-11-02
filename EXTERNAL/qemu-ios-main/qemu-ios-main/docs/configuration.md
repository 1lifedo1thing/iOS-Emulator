# Machine configuration

Behavior options are moving to typed QEMU machine properties. This migration is
incremental; most existing `IT_*` variables still retain their documented behavior.

| Property | Values | Default | Legacy alias |
| --- | --- | --- | --- |
| `boot-args` | kernel command line iBoot hands over, at most 255 bytes with each `%` counted twice; empty keeps iBoot's own | no override | none: the property is the only input |
| `bt` | `on`, `off` | `on` | `IT_BT`: leading `0` disables, otherwise enables |
| `bt-latency-us` | unsigned 32-bit microseconds | `2000` | `IT_BT_LATENCY_US` |
| `osk` | `on`, `off` | `off` | `IT_OSK`: any present value enables |
| `h264-decode`, `scaler-decode`, `mpvd-decode`, `lcd-planes` | `on`, `off` | `on`; `off` selects the legacy register stubs | none |
| `amc-mode` | `registers`, `handshake`, `decode` | `decode` | `IT_AMC_STATE` selects `handshake` |
| `audio-hw` | `auto`, `on`, `off` | `auto`: CS42L58, amp, I2S0 and AMC present on every boot (2.1.1 panics without them) | `IT_AUDIO_HW`: leading `0` disables; any other value enables |

Use `-M iPod-Touch,audio-hw=on` to force audio hardware. An explicitly supplied
property, including `auto`, wins over the environment alias. The alias remains
compatible and emits a deprecation warning when used. The option controls
hardware presence, not output volume or decoding, and is immutable after the
machine starts. `-M iPod-Touch,help` lists the property and its type.

The retired regression harness accepted `--audio-hw auto|on|off`. `tests/slice/ipod-audio-config.c` checks
default behavior, alias precedence, explicit auto, invalid values and rejection
of changes after startup.

The legacy on-screen-keyboard tapper uses `osk=on`. Explicit `osk=off` overrides
any `IT_OSK` value, and the option cannot change after startup. The launcher’s
`--keyboard` / `--appsync` flags and Light Touch now use this property. The app
also passes its USB session through the existing `usb-tcp-addr` option rather
than changing process-wide `IT_USB_TCP`. Agent text insertion is unchanged.
`tests/slice/ipod-osk-config.c` checks alias presence, explicit precedence and immutability.

The existing Bluetooth HCI controller uses `bt` and `bt-latency-us`. Both are
startup-only, and explicit options override aliases. A user-supplied UART1
chardev still takes precedence over the built-in controller. Reply delay is
stored per controller and converted to nanoseconds without signed overflow;
invalid, negative and oversized legacy values are rejected. These controls do
not add Bluetooth peers. `test_bt_config.py` checked the real paused machine,
including aliases, boundaries and runtime rejection (retired; see git history at 5508b504b8).

## Boot arguments

`boot-args` is fixed before machine startup and has no environment alias (the
`IT_BOOT_ARGS*` variables are ignored). Release iBoots ignore the boot-args NVRAM
variable and format the kernel command line with
`snprintf(gBootArgs.commandLine, 0x100, "%s ", "")`. The literal-pool slot holding
that format's address is found by pattern in the staged iBoot
(`it_iboot_find_boot_args_format`, every iPod touch 2G build from 2.1.1 to 4.2.1) and
pointed at `commandLine`, which is filled with the arguments, `%` doubled, before each
NAND read iBoot makes (its BSS is cleared when it starts). iBoot then hands the kernel
exactly those arguments, on direct-iBoot and SecureROM boots alike. The arguments
become the format, so they are limited to 255 bytes with each `%` counted twice;
longer strings are rejected. An empty value leaves iBoot's own command line.
`tests/qtest/ipod-boot-args-test.c` and `tests/slice/ipod-early-boot-args.c` check it.

## Firmware profiles

There are none. The per-build kernel-banner table (`ipod_touch_firmware.c`, 5F138 and 7E18) had no caller
left and was deleted with its unit test; the build-specific addresses it once keyed (MBX, FMSS, the AMFI
task-port patch, the 5F138 clock trampoline) were removed before it. iBoot's command-line format slot and
security epoch are found by pattern in any iBoot (`hw/arm/it_iboot.c`).
