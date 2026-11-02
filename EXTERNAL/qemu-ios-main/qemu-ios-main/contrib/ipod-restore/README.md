# In-emulator stock restore of the iPod touch 2G (n72ap)

Runs Apple's own restore against the emulated device: SecureROM DFU, iBSS, the restore ramdisk, then `restored`
with asr and the NOR data, onto a blank NAND. The host side is stock idevicerestore; nothing here speaks the restore
protocol. Results and the current limits are in docs/ipod/from-ipsw.md ("In-emulator stock restore").

| file | what |
|---|---|
| `build-host-tools.sh` | builds the libirecovery transport and idevicerestore into a directory of your choice |
| `libirecovery-qemu/` | a private libirecovery (upstream 93c117c) whose USB transport is a Unix socket to the bridge |
| `usb_recovery_bridge.py`, `tcp_usb.py` | the bridge: QEMU's TCP USB (hw/arm/ipod_touch_tcp_usb.c) to that socket for DFU and recovery, then a relay to usbmuxd once the restore kernel enumerates (PID 0x129x) |
| `run-restore.sh` | one restore, everything in its own directory |
| `run-boot.sh` | cold boot of a restored device, with screendumps and optional monitor probes |

`libirecovery-qemu/` and the two Python files come back from the tree at 5508b504b8 (contrib/libirecovery-qemu and
imgtools/), where they served the retired Python restore tests; they are this project's own code.

## Inputs

- A build of this tree's `qemu-system-arm`.
- The S5L8720 SecureROM 240.4 image (`BOOTROM`).
- The usbmuxd fork with the QEMU backend (`USBMUXD`), as LightTouchMac ships it.
- A FirmwareKit-prepared n72ap device of the build being restored (`BASE`): its `nor.bin` gives the SysCfg and nvram,
  `gid-blobs.bin` the KBAG keys, `identity.json` the synthetic ECID. Its NAND is not used.
- The stock IPSW.

## Build the host tools

```
contrib/ipod-restore/build-host-tools.sh contrib/ipod-restore/host            # upstream idevicerestore 4b3e847
LEGACY=1 contrib/ipod-restore/build-host-tools.sh contrib/ipod-restore/host   # and LukeZGD's 5ea1b08, for 2.x
```

idevicerestore gets an rpath to the transport library; with `IRECV_QEMU_SOCKET` set, recovery-mode access goes only to
that socket, never to physical USB. Restore mode goes through the usbmuxd fork (`USBMUXD_SOCKET_ADDRESS`).

## Personalization

For 3.x and 4.x idevicerestore wants a TSS record, which Apple no longer signs for these builds. Before asking it looks
for `<cache>/shsh/<decimal ECID>-iPod2,1-<version>.shsh`. `run-restore.sh` writes an empty one there when `VERSION` is
set (a gzipped binary plist holding an empty dictionary), and idevicerestore then sends every component
unpersonalized ("Not personalizing component ..."). The emulated SecureROM, iBSS and restore kernel accept the
stock IPSW signatures; forge-sigcheck stays off. 2.x has no TSS step.

## Run

```
BOOTROM=.../bootrom_240_4 USBMUXD=.../usbmuxd VERSION=3.1.3 \
    contrib/ipod-restore/run-restore.sh /tmp/restore-7E18 iPod2,1_3.1.3_7E18_Restore.ipsw /path/to/n72ap-7E18
```

It exits 0 when restore.log has "Status: Restore Finished" (about 130 s for 7E18). The restored NAND is
`OUT/nandrw` (physical page files, FMSS_PHYSICAL), the restored NOR `OUT/nor-rw.bin`. A run needs about 2.5 GB.

```
BOOTROM=.../bootrom_240_4 STEP=20 PROBE="info registers" \
    contrib/ipod-restore/run-boot.sh /tmp/restore-7E18 /tmp/boot-7E18 /path/to/n72ap-7E18 240
```

With `USBMUXD=.../usbmuxd` as well, run-boot.sh starts a usbmuxd for the device and writes its address to
`OUT/usbmuxd.addr`. Use it as `USBMUXD_SOCKET_ADDRESS` for ideviceinfo, idevicesyslog and the other tools.
Both scripts use `aes-uid=hardware`; set `AES_UID=engine` only for an image made before that existed.

Kill only the processes a run started; the scripts track their own PIDs.
