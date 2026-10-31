#!/bin/sh
# Regression check: the first N flips under CJ_DET (frame-count time, so both
# renderers show the same content at the same flip) with the GL back end and
# with the software NV2A, then tools/frame_cmp.py on every 5th flip.
#   tools/gl_vs_sw.sh <tag> [N=40]
# Frames: run/<tag>/gl/gNNNNN.ppm, run/<tag>/sw/fNNNNN.bmp. Delete them after.
set -e
cd "$(dirname "$0")/.."
TAG=$1; N=${2:-40}
D=run/$TAG; mkdir -p $D/gl $D/sw
DISP=:$((80 + $$ % 9))
Xvfb $DISP -screen 0 1024x768x24 >/dev/null 2>&1 & XV=$!
sleep 1.5
COMMON="CJ_DET=1 CJ_LOCKSTEP=2 CJ_STOP_FRAME=$N RECOMP_WATCHDOG_SECS=900"
env $COMMON DISPLAY=$DISP CJ_GL=1 CJ_GL_DUMP=$D/gl/g CJ_GL_DUMP_EVERY=5 RECOMP_FB_DUMP=$D/no-such-dir/f \
    RECOMP_FB_DUMP_FLIPS=0 ./run.sh > $D/gl.log 2>&1 || true
env $COMMON RECOMP_FB_DUMP=$D/sw/f RECOMP_FB_DUMP_FLIPS=1 ./run.sh > $D/sw.log 2>&1 || true
kill $XV 2>/dev/null || true
python3 tools/frame_cmp.py $D/gl $D/sw
