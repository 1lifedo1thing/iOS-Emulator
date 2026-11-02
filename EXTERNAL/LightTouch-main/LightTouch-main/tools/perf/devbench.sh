#!/bin/bash
# usage: devbench.sh SERIAL RECORD tag:binary:mask ...   (boot + fling fps per spec)
cd "$(dirname "$0")"
export ANDROID_SERIAL=$1; R=files/library/records/$2; shift 2
for spec in "$@"; do
  IFS=: read -r tag bin mask vmask <<<"$spec"
  out=$(PYTHONUNBUFFERED=1 timeout 900 python3 ltbench.py adb --qemu files/bench/$bin --base $R/base --rom $R/assets/bootrom.bin \
     --out runs/$ANDROID_SERIAL-$tag --seconds 300 --poll 1 --stop-lock --workload scroll --fling --workload-seconds 20 \
     --wrap "env LD_LIBRARY_PATH=files/bench taskset $mask" ${vmask:+--vcpu-mask $vmask} -- -trace enable=ipod_touch_lcd_frame_log 2>&1 | grep -E "LOCKSCREEN|SCROLL|FAIL" | tr '\n' ' ')
  echo "$ANDROID_SERIAL $tag $bin mask=$mask $out"
done
