#!/bin/bash
# Build the guest-side pasteboard binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

for t in it_pbd; do
    [ -f "$HERE/$t.c" ] || continue
    cc6 "$HERE/$t.c" "$HERE/$t.o"
    link6 -execute "$HERE/$t" "$HERE/$t.o"
    rm -f "$HERE/$t.o"
    file "$HERE/$t"
done
# it_pbtool: the pasteboard from a command line, for host checks (its source says which); it enters
# at its own _start for argv. contrib/ipad1-guest/build.sh builds the armv7 one.
cc6 "$HERE/it_pbtool.c" "$HERE/it_pbtool.o"
link6 -execute "$HERE/it_pbtool" "$HERE/it_pbtool.o" -e __start
rm -f "$HERE/it_pbtool.o"
"${LDID:-ldid}" -S "$HERE/it_pbtool"
file "$HERE/it_pbtool"
