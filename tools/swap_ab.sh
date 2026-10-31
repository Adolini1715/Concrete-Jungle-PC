#!/bin/sh
# A/B one boot of the native swap: swap_ab.sh <tag> <0|1> [binary]
# Writes run/ab/<tag>.log and frames to run/ab/<tag>/frames.
cd "$(dirname "$0")/.." || exit 1
TAG=$1; MODE=$2; BIN=${3:-./build/pcj}
DISC=${DISC:-/home/claude/pcj/retail/disc_full}
D=run/ab/$TAG; rm -rf "$D"; mkdir -p "$D/save" "$D/frames"
cp -r run/save/. "$D/save/" 2>/dev/null
RECOMP_AC97_READY=1 RECOMP_TITLE_KEVENTS=1 RECOMP_VBLANK=1 RECOMP_PB_EXEC=1 \
RECOMP_FB_DUMP=$D/frames/f RECOMP_FB_DUMP_FLIPS=1 \
RECOMP_WATCHDOG_SECS=${RECOMP_WATCHDOG_SECS:-50} CJ_NATIVE_SWAP=$MODE \
  "$BIN" "$DISC" "$PWD/$D/save" > run/ab/$TAG.log 2>&1
echo "$TAG exit=$?"
