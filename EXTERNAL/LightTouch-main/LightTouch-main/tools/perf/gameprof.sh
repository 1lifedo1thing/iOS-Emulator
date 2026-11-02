#!/bin/bash
# Attach to the in-app engine during play; no injected guest input.
# usage: ANDROID_SERIAL=... [SP_CALLGRAPH=fp|dwarf] [SP_FREQ=1000] gameprof.sh OUTDIR [seconds]
set -euo pipefail
OUT=${1:?usage: gameprof.sh OUTDIR [seconds]}; SECS=${2:-15}
mkdir -p "$OUT"; PKG=org.lighttouch.android
pid=$(adb shell pidof "$PKG:engine" | tr -d '\r')
[[ "$pid" =~ ^[0-9]+$ ]] || { echo "Expected one running engine worker; got '$pid'" >&2; exit 1; }
options=(-p "$pid" -m 8 --cpu-percent 25 -f "${SP_FREQ:-4000}" --duration "$SECS")
case ${SP_CALLGRAPH:-none} in
    none) ;;
    fp|dwarf) options+=(--call-graph "$SP_CALLGRAPH") ;;
    *) echo 'SP_CALLGRAPH must be none, fp or dwarf' >&2; exit 1 ;;
esac
if ! adb shell run-as "$PKG" simpleperf record "${options[@]}" -o files/perf.data > "$OUT/record.txt" 2>&1; then
    cat "$OUT/record.txt" >&2; exit 1
fi
r() { local args; printf -v args '%q ' "$@"; adb shell "run-as $PKG simpleperf report -i files/perf.data $args"; }
r --sort tid,comm --percent-limit 1 > "$OUT/threads.txt" 2>&1
if ! awk '/^Samples:/ { n += $2 } END { exit !(n > 0) }' "$OUT/threads.txt"; then
    cat "$OUT/record.txt" "$OUT/threads.txt" >&2
    echo 'Recording contains no samples; not a usable profile.' >&2; exit 1
fi
r --comms 'CPU 0/TCG' --sort dso --percent-limit 0.5 > "$OUT/vcpu-dso.txt" 2>&1
r --comms 'CPU 0/TCG' --sort symbol --percent-limit 0.4 > "$OUT/vcpu-sym.txt" 2>&1
r --sort comm,dso --percent-limit 1 > "$OUT/comm-dso.txt" 2>&1
if [[ ${SP_CALLGRAPH:-none} != none ]]; then
    r --comms 'CPU 0/TCG' -g callee --percent-limit 0.3 > "$OUT/vcpu-callgraph.txt" 2>&1
fi
adb exec-out run-as "$PKG" cat files/perf.data > "$OUT/perf.data"
echo "done $OUT"
# Resolve using the exact matching unstripped library. From OUT:
#   $NDK/simpleperf/binary_cache_builder.py -i perf.data -lib SYMBOLS --disable_adb_root
#   $NDK/simpleperf/bin/linux/x86_64/simpleperf report -i perf.data --symfs binary_cache
