#!/bin/sh
# Builds dist/PinPlay.cia and dist/PinPlay.3dsx.
#   ./build.sh shots    - draws every screen from made-up data and saves screenshots to sdmc:/pinplay_shots (no network)
#   ./build.sh autojoin - joins the game in sdmc:/pinplay_test.txt (PIN, nickname) and answers by itself, with screenshots
set -e; cd "$(dirname "$0")"; ROOT="$(pwd)"
CTR="${CTRTOOLS:-$HOME/Tools/ctrtools}"
EXTRA=""; NAME=PinPlay
[ "$1" = shots ] && { EXTRA="-DPP_SHOTS"; NAME=PinPlay-shots; }
[ "$1" = solvetest ] && { EXTRA="-DPP_SOLVETEST"; NAME=PinPlay-solvetest; }
[ "$1" = autojoin ] && { EXTRA="-DPP_AUTOJOIN"; NAME=PinPlay-autojoin; }
[ -f icon.png ] && [ -f meta/banner.png ] || python tools/gen_meta.py
[ -f romfs/cacert.pem ] || python tools/gen_cacert.py romfs/cacert.pem
./dkp.sh "$ROOT" make clean > /dev/null
./dkp.sh "$ROOT" make "EXTRA_CFLAGS='$EXTRA'" 2>&1 | grep -E "error|warning|built" || true
[ -f PinPlay.elf ] || { echo "build failed"; exit 1; }
mkdir -p dist; cp PinPlay.3dsx "dist/$NAME.3dsx"
if [ -z "$EXTRA" ]; then
  "$CTR/bannertool.exe" makebanner -i meta/banner.png -a meta/banner.wav -o meta/banner.bnr > /dev/null
  "$CTR/bannertool.exe" makesmdh -s "PinPlay" -l "Join live Kahoot games with the game PIN" -p "kingarthurthe10th-ops" -i icon.png -o meta/icon.icn > /dev/null
  "$CTR/makerom.exe" -f cia -o dist/PinPlay.cia -elf PinPlay.elf -rsf meta/cia.rsf -icon meta/icon.icn -banner meta/banner.bnr -exefslogo -target t
fi
ls -la dist
