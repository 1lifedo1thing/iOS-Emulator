#!/bin/bash
# Stock erase restore of an emulated iPod touch 2G (n72ap) from SecureROM DFU onto a blank NAND.
#
#   contrib/ipod-restore/run-restore.sh OUT IPSW BASE
#
# OUT   new directory for this run: the NAND (OUT/nandrw, page files), the NOR the restore writes (OUT/nor-rw.bin)
#       and the logs (restore.log, bridge.log, usbmuxd.log, qemu.log, serial.log)
# IPSW  the stock iPod2,1 IPSW
# BASE  a FirmwareKit-prepared n72ap device of the same build: nor.bin (its superblock, SysCfg and nvram are kept,
#       the image area is blanked so the SecureROM enters DFU), gid-blobs.bin, identity.json (its synthetic ECID)
#
# Environment:
#   TOOLS      build-host-tools.sh's OUT (default: contrib/ipod-restore/host)
#   IDR        idevicerestore to run (default: $TOOLS/idevicerestore; 2.x needs $TOOLS/idevicerestore-legacy)
#   QEMU       qemu-system-arm (default: build/qemu-system-arm)
#   BOOTROM    the S5L8720 SecureROM 240.4 (required)
#   USBMUXD    the usbmuxd fork's binary (required)
#   VERSION    ProductVersion (3.1.3, 4.2.1): writes the empty SHSH record that makes idevicerestore send every
#              component unpersonalized; unset for 2.x, which has no TSS step
#   ECID       overrides identity.json's unique-chip-id
#   TIMEOUT    seconds for idevicerestore (default 1200)
# The NAND is physical (FMSS_PHYSICAL=1): page files at the addresses the guest programs, erased blocks marked.
# Audio is off; nothing is shown.
set -u
OUT="$1"; IPSW=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); BASE=$(cd "$3" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TOOLS=${TOOLS:-$HERE/host}
IDR=${IDR:-$TOOLS/idevicerestore}
QEMU=${QEMU:-$ROOT/build/qemu-system-arm}
: "${BOOTROM:?set BOOTROM to the SecureROM image}" "${USBMUXD:?set USBMUXD to the usbmuxd fork}"
mkdir "$OUT" || exit 1
OUT=$(cd "$OUT" && pwd)
cd "$OUT"
ECID=${ECID:-$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['unique-chip-id'])" "$BASE/identity.json")}
python3 -c "
import sys
b = bytearray(open(sys.argv[1], 'rb').read())
b[0x8000:0xfc000] = b'\xff' * (0xfc000 - 0x8000)
open('nor.bin', 'wb').write(b)" "$BASE/nor.bin"
cp nor.bin nor-rw.bin
mkdir nand nandrw
port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
USB=$(port); MUXUSB=$(port); MUX=$(port)
pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done; sleep 1; for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null; done; }
trap cleanup EXIT

# usbmuxd takes the restore kernel's USB; the bridge serves DFU and recovery to libirecovery and hands over to it.
USBMUXD_QEMU_ADDR=127.0.0.1:$MUXUSB USBMUXD_QEMU_DELAY=0 "$USBMUXD" -f -v -S 127.0.0.1:$MUX -P NONE -C "$OUT/conf" \
    > usbmuxd.log 2>&1 &
pids+=($!)
python3 -u "$HERE/usb_recovery_bridge.py" --socket "$OUT/usb.sock" --port $USB --usbmuxd 127.0.0.1:$MUXUSB > bridge.log 2>&1 &
pids+=($!)
for i in $(seq 100); do [ -S "$OUT/usb.sock" ] && break; sleep 0.1; done

M="iPod-Touch,bootrom=$BOOTROM,nor=$OUT/nor.bin,nor-rw=$OUT/nor-rw.bin,nand=$OUT/nand,nandrw=$OUT/nandrw"
M="$M,ecid=$ECID,gid-blobs=$BASE/gid-blobs.bin,aes-uid=${AES_UID:-hardware},usb-tcp-addr=127.0.0.1:$USB,usb-attached=on"
echo "$M" > machine.txt
FMSS_PHYSICAL=1 "$QEMU" -M "$M" -m 2G -display none -audio driver=none -monitor none -serial "file:$OUT/serial.log" \
    > qemu.log 2>&1 &
pids+=($!)

mkdir -p cache/shsh
if [ -n "${VERSION:-}" ]; then
    # idevicerestore looks for <cache>/shsh/<decimal ECID>-<ProductType>-<version>.shsh before asking Apple's TSS.
    python3 -c "
import gzip, plistlib, sys
open(sys.argv[1], 'wb').write(gzip.compress(plistlib.dumps({}, fmt=plistlib.FMT_BINARY)))" \
        "cache/shsh/$((ECID))-iPod2,1-$VERSION.shsh"
fi
# idevicerestore unpacks next to the IPSW: give it a clone inside OUT.
cp -c "$IPSW" fw.ipsw 2>/dev/null || cp "$IPSW" fw.ipsw
start=$(date +%s)
IRECV_QEMU_SOCKET="$OUT/usb.sock" USBMUXD_SOCKET_ADDRESS=127.0.0.1:$MUX \
    perl -e 'alarm shift; exec @ARGV' "${TIMEOUT:-1200}" "$IDR" -e -y -i "$ECID" -C "$OUT/cache" "$OUT/fw.ipsw" > restore.log 2>&1
rc=$?
echo "idevicerestore rc=$rc after $(( $(date +%s) - start ))s" | tee -a restore.log
# restored sets auto-boot=true in the NOR's nvram as it finishes; stopping QEMU before that lands leaves
# auto-boot=false and the cold boot stops in iBoot's recovery prompt.
for i in $(seq 120); do LC_ALL=C grep -aq 'auto-boot=true' nor-rw.bin && break; sleep 0.5; done
grep -q "Status: Restore Finished" restore.log
