#!/bin/bash
# Cold-boot a device run-restore.sh restored: SecureROM -> the restored NOR's LLB -> iBoot -> the restored NAND.
#
#   contrib/ipod-restore/run-boot.sh RESTORE_OUT OUT BASE [SECONDS]
#
# Works on copies (RESTORE_OUT is left as it is). Takes a QMP screendump every STEP seconds (default 30) as
# OUT/shot-T.ppm and, with PROBE set, runs that monitor command too (e.g. PROBE="info registers"), appending the
# output to OUT/probe.log. QEMU, BOOTROM and ECID as for run-restore.sh; EXTRA adds machine options. With USBMUXD set
# (the usbmuxd fork), the device's USB goes to its own usbmuxd, whose socket address is in OUT/usbmuxd.addr for
# USBMUXD_SOCKET_ADDRESS (ideviceinfo, idevicesyslog, ...).
set -u
SRC=$(cd "$1" && pwd); OUT="$2"; BASE=$(cd "$3" && pwd); SECS=${4:-300}
HERE=$(cd "$(dirname "$0")" && pwd)
QEMU=${QEMU:-$(cd "$HERE/../.." && pwd)/build/qemu-system-arm}
: "${BOOTROM:?set BOOTROM to the SecureROM image}"
mkdir "$OUT" || exit 1
OUT=$(cd "$OUT" && pwd)
cd "$OUT"
ECID=${ECID:-$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['unique-chip-id'])" "$BASE/identity.json")}
cp "$SRC/nor-rw.bin" nor.bin
cp nor.bin nor-rw.bin
mkdir nand
cp -cR "$SRC/nandrw" nandrw 2>/dev/null || cp -R "$SRC/nandrw" nandrw
M="iPod-Touch,bootrom=$BOOTROM,nor=$OUT/nor.bin,nor-rw=$OUT/nor-rw.bin,nand=$OUT/nand,nandrw=$OUT/nandrw"
M="$M,ecid=$ECID,gid-blobs=$BASE/gid-blobs.bin,aes-uid=${AES_UID:-hardware}${EXTRA:-}"
MPID=
if [ -n "${USBMUXD:-}" ]; then
    port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
    USB=$(port); MUX=$(port)
    USBMUXD_QEMU_ADDR=127.0.0.1:$USB USBMUXD_QEMU_DELAY=0 "$USBMUXD" -f -v -S 127.0.0.1:$MUX -P NONE -C "$OUT/conf" \
        > usbmuxd.log 2>&1 &
    MPID=$!
    echo "127.0.0.1:$MUX" > usbmuxd.addr
    sleep 1
    M="$M,usb-tcp-addr=127.0.0.1:$USB,usb-attached=on"
fi
echo "$M" > machine.txt
FMSS_PHYSICAL=1 IT_TVOUT_READY=1 "$QEMU" -M "$M" -m 2G -display none -audio driver=none -monitor none \
    -qmp "unix:$OUT/qmp.sock,server=on,wait=off" -serial "file:$OUT/serial.log" > qemu.log 2>&1 &
QPID=$!
trap 'kill $QPID $MPID 2>/dev/null; sleep 1; kill -9 $QPID $MPID 2>/dev/null' EXIT

qmp() {   # COMMAND: screendump to a path, or "hmp:<monitor command>"
    python3 - "$OUT/qmp.sock" "$1" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
f = s.makefile('rw')
f.readline()
f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
if sys.argv[2].startswith('hmp:'):
    cmd = {"execute": "human-monitor-command", "arguments": {"command-line": sys.argv[2][4:]}}
else:
    cmd = {"execute": "screendump", "arguments": {"filename": sys.argv[2]}}
f.write(json.dumps(cmd) + '\n'); f.flush()
print(json.loads(f.readline()).get('return', ''))
PY
}

STEP=${STEP:-30}
for t in $(seq "$STEP" "$STEP" "$SECS"); do
    sleep "$STEP"
    kill -0 $QPID 2>/dev/null || { echo "qemu exited"; break; }
    qmp "$OUT/shot-$t.ppm" > /dev/null
    if [ -n "${PROBE:-}" ]; then
        { echo "== $t"; qmp "hmp:$PROBE"; } >> probe.log
    fi
done
echo "boot done after ${SECS}s"
