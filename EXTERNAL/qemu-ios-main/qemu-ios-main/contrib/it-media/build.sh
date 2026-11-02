#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/itmedia.c" "$HERE/itmedia.o" -Wall -Wextra \
    -isystem "$(xcrun clang -print-resource-dir)/include"
# The embedded Info.plist names the process Music (com.apple.mobileipod): 4.x MusicLibrary opens the library for
# writing only in Music, iPod Out and itdbprepserver (MLProcessHasDatabaseWritePermissions, by bundle identifier
# or process name), the processes that add store purchases and synced changes on the device. Elsewhere its inserts
# hit a read-only database and post-processing drops the half-written item.
# Not PIE: on 3.1 a read of that section at its link address (Foundation, at the import) took SIGBUS once the
# section moved to a later page of the PIE build; 3.1's own executables, which carry theirs the same way, aren't PIE.
link6 -execute "$HERE/itmedia" "$HERE/itmedia.o" -e __start -no_pie -sectcreate __TEXT __info_plist "$HERE/itmedia-Info.plist"
rm -f "$HERE/itmedia.o"
# 3.2+ AMFI runs only signed executables; 3.1 ignores the signature. 5.x's importer signs each track's
# integrity through FairPlay, which answers only fairplay-client processes: the entitlement carries Music's client
# ID, the process the helper names itself as (4.3's FairPlay refuses atc's).
"${LDID:-ldid}" -S"$HERE/itmedia.entitlements" "$HERE/itmedia"
cc6 "$HERE/itphoto.c" "$HERE/itphoto.o" -Wall -Wextra
link6 -execute "$HERE/itphoto" "$HERE/itphoto.o" -e __start
rm -f "$HERE/itphoto.o"
# From 6.0 Photos access goes through TCC; atc, the sync agent, is allowed it with the same key.
"${LDID:-ldid}" -S"$HERE/itphoto.entitlements" "$HERE/itphoto"
