#!/bin/zsh
# Prepare an upstream-made base for a catalog id: prep.sh ID IPSW_URL
set -e
cd ~/Documents/LightTouch-Compat
ID=$1; URL=$2
if [ ! -d app/"Light Touch.app" ]; then
  mkdir -p app && cd app
  curl -fL -o lt.zip https://github.com/samhenrigold/LightTouchMac/releases/download/v1.0-2026.10.20b/LightTouchMac-universal.zip
  ditto -x -k lt.zip . && rm lt.zip && cd ..
fi
APP="$PWD/app/$(ls app | grep '\.app$' | head -1)"
mkdir -p ipsw
F=ipsw/$(basename "$URL")
MIRROR="https://archive.org/download/iOS_Firmware_Exhibit_Collection/$(basename "$URL" | sed s/,/%2C/)"
i=0
while [ ! -f "$F" ] && [ $i -lt 30 ]; do
  i=$((i+1)); U=$URL; [ $((i % 2)) = 0 ] && U=$MIRROR
  curl -fsSL -C - --speed-limit 200000 --speed-time 30 -o "$F.partial" "$U" && mv -f "$F.partial" "$F"
done
shasum "$F"
rm -rf "out/$ID"; mkdir -p out
"$APP/Contents/MacOS/firmwarekit" create --catalog "$APP/Contents/Resources/firmware-catalog.json" --id "$ID" --ipsw "$F" --out "out/$ID" > "out/$ID.events.jsonl"
"$APP/Contents/MacOS/firmwarekit" pack-base --base "out/$ID" --out "out/$ID.itbase"
ls -la out
echo DONE
