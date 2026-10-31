#!/bin/sh
# Replay a session recording (docs/recording.md) and report whether it stayed exact.
#   tools/det/replay.sh <recording dir> [linux|wine] [tag]
# The save folder starts from tests/fixtures/save-pristine.tgz with the recording's save/
# copied over it. Linux uses build/pcj, wine the staged Windows build (dist/stage, tools/package_win.sh;
# move dist/stage/pcj.ini aside: settings such as CJ_FPS change the simulation). A run that makes no
# frame progress for 6 minutes is stopped and gdb thread backtraces go to run/det/<tag>/hang.txt.
# REC_EXIT=1 runs a diverged replay to its end (default 2: stop at the first divergence).
# NODRAW=1 skips the GL draw calls (no frame dumps): the heap hashes do not depend on them.
# Prints the [REC] summary: "input ended at poll N (K hash checks, all matched)" or DIVERGE.
cd "$(dirname "$0")/../.." || exit 1
R=$(cd "$1" && pwd); KIND=${2:-linux}; TAG=${3:-replay}
# env.txt in the recording: settings it was made with (CJ_MODS ...); the caller's environment wins
if [ -f "$R/env.txt" ]; then
  while IFS='=' read -r k v; do [ -n "$k" ] && [ -z "$(printenv "$k")" ] && export "$k=$v"; done < "$R/env.txt"
fi
D=run/det/$TAG; rm -rf $D; mkdir -p $D
if [ $KIND = linux ]; then SAVE=$PWD/$D/save; else SAVE=$PWD/run/save-win; rm -rf $SAVE; fi
mkdir -p $SAVE && tar -xzf tests/fixtures/save-pristine.tgz -C $SAVE && cp -a "$R/save/." $SAVE/
DISP=${DISP:-:91}
# frame dumps only when drawing (an empty CJ_GL_DUMP would still dump, into the current folder)
if [ "${NODRAW:-0}" = 1 ]; then unset CJ_GL_DUMP; else export CJ_GL_DUMP=$D/g; fi
Xvfb $DISP -screen 0 1024x768x24 >/dev/null 2>&1 & XV=$!; sleep 1.5
if [ $KIND = linux ]; then
  RUN=./run.sh; RP_DIR=$R
else
  RUN=tools/run_wine.sh; RP_DIR="Z:$(echo $R | tr / '\\')"
fi
DISPLAY=$DISP CJ_REC_PLAY="$RP_DIR" CJ_STOP_FRAME=${FR:-1000000} CJ_DET_HASH=${HASH:-0} CJ_GAMELOG=0 \
  RECOMP_WATCHDOG_SECS=20000 RECOMP_FB_DUMP= CJ_GL=1 CJ_RUN_SAVE=$SAVE CJ_REC_EXIT=${REC_EXIT:-2} \
  CJ_GL_NODRAW=${NODRAW:-0} CJ_GL_DUMP_EVERY=${DUMP_EVERY:-300} timeout ${TMO:-20000} $RUN > $D/log.txt 2>&1 &
RP=$!; last=-1; quiet=0
# progress check every 3 s (a finished run is noticed at once); 6 minutes without a new frame = hang
while kill -0 $RP 2>/dev/null; do
  sleep 3
  n=$(grep -c '\[REC\] frame\|^\[DET\] f=\|\[GL\] flip' $D/log.txt)
  if [ "$n" = "$last" ]; then quiet=$((quiet+3)); else quiet=0; last=$n; fi
  if [ $quiet -ge 360 ]; then
    P=$(pgrep -f "pcj.* $SAVE" | head -1); [ -n "$P" ] || P=$(pgrep -f "build/pcj|dist/stage/pcj.exe" | head -1)
    echo "no progress for 6 minutes, pid $P" > $D/hang.txt
    gdb -p $P -batch -ex "thread apply all bt 12" >> $D/hang.txt 2>&1
    kill $RP; kill $P 2>/dev/null; break
  fi
done
kill $XV 2>/dev/null
grep "\[REC\]" $D/log.txt | tail -3
[ -f $D/hang.txt ] && echo "HANG: see $D/hang.txt"
