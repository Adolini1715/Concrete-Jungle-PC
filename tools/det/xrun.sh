#!/bin/sh
# xrun.sh TAG linux|wine FRAMES DISP : CJ_DET run from the save-win state with the standard pad script
cd "$(dirname "$0")/../.." || exit 1
TAG=$1; KIND=$2; FR=$3; DISP=$4
D=run/det/$TAG; rm -rf $D; mkdir -p $D
P="200:start,300:start,400:a,450:a,500:a,550:a,650:a,750:a,850:a,950:a,1050:a,1150:a,1250:a,1300:lup:40,1350:x,1380:rright:15,1420:lup+lright:60,1500:y"
Xvfb $DISP -screen 0 1024x768x24 >/dev/null 2>&1 & XV=$!; sleep 1.5
if [ $KIND = linux ]; then
  mkdir -p $D/save; tar -xzf tests/fixtures/save-pristine.tgz -C $D/save
  RUN="./run.sh"; export CJ_RUN_SAVE=$PWD/$D/save
else
  rm -rf run/save-win; mkdir -p run/save-win; tar -xzf tests/fixtures/save-pristine.tgz -C run/save-win; RUN=tools/run_wine.sh
fi
DISPLAY=$DISP CJ_DET_DUMP=${DUMPF:-} CJ_DET_DUMP_DIR=$PWD/$D RECOMP_FB_DUMP= CJ_DET=1 CJ_STOP_FRAME=$FR CJ_DET_HASH=1 CJ_GAMELOG=0 RECOMP_WATCHDOG_SECS=14000 \
 CJ_GL=1 CJ_PAD_SCRIPT="$P" CJ_GL_DUMP=$D/g CJ_GL_DUMP_EVERY=50 timeout 14000 $RUN > $D/log.txt 2>&1
kill $XV; rm -rf $D/save
grep -c "^\[DET\] f=" $D/log.txt
