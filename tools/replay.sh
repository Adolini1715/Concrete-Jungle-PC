#!/bin/sh
# Replay a recorded frame set (src/frame_rec.c) under Wine, no game boot:
#   tools/replay.sh <recording dir> <out dir> [stage dir]
# Frames land in <out dir>/gNNNNN.ppm (one per recorded flip); the log in <out dir>/log.txt.
# Record one with CJ_FRAME_REC=<dir> CJ_FRAME_REC_AT=<flip> [CJ_FRAME_REC_N=3] on any run.
set -e
REC=$(cd "$1" && pwd); OUT=$2; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$3" ] && export PCJ_WIN_STAGE=$3
W() { echo "Z:$(echo "$1" | tr / '\\')"; }
Xvfb :89 -screen 0 1024x768x24 >/dev/null 2>&1 & XV=$!
trap 'kill $XV 2>/dev/null' EXIT
sleep 1
DISPLAY=:89 CJ_GL=1 CJ_GL_PRESENT=${CJ_GL_PRESENT:-} CJ_REPLAY_LOG=${CJ_REPLAY_LOG:-} CJ_REPLAY="$REC" CJ_GL_DUMP="$OUT/g" CJ_GL_DUMP_EVERY=1 \
  timeout 600 sh "$ROOT/tools/run_wine.sh" > "$OUT/log.txt" 2>&1 || true
grep -a -E "\[REPLAY\]|\[CRASH\]" "$OUT/log.txt" || echo "no [REPLAY] line, see $OUT/log.txt"
