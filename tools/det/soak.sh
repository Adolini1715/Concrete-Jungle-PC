#!/bin/bash
# Replay every recording and report which stayed exact (regression gate for engine/native changes).
#   tools/det/soak.sh [linux|wine] [recording dir ...]
# Default recordings: tests/fixtures/rec-*. Results go to run/det/soak-<name>/ and a table to
# run/det/soak.txt. Env passes through (e.g. CJ_MODS=... to test a set of native swaps,
# FR=<frame> to stop early).
# Speed: draws are skipped (NODRAW=1, set NODRAW=0 for frame dumps) and linux recordings run
# JOBS at a time (default: one per core; wine runs share a save folder, so one at a time).
# Exit status: 0 when every recording matched to its end, 1 otherwise.
cd "$(dirname "$0")/../.." || exit 1
KIND=${1:-linux}; [ $# -gt 0 ] && shift
[ $# -eq 0 ] && set -- tests/fixtures/rec-*
export NODRAW=${NODRAW:-1}
JOBS=${JOBS:-$(nproc)}; [ "$KIND" = linux ] || JOBS=1
OUT=run/det/soak.txt; mkdir -p run/det; : > $OUT
T0=$(date +%s)

one() {  # <recording dir> <display number>
  local R=$1 name t0 dt L res checks qs
  name=$(basename "$R"); t0=$(date +%s)
  DISP=:$2 tools/det/replay.sh "$R" $KIND soak-$name > run/det/soak-$name.txt 2>&1
  dt=$(( $(date +%s) - t0 ))
  L=run/det/soak-$name/log.txt
  if grep -q "all matched" $L 2>/dev/null; then res=PASS
  elif grep -q "DIVERGE" $L 2>/dev/null; then res="DIVERGE $(grep -o 'DIVERGE at frame [0-9]*' $L | head -1)"
  elif [ -f run/det/soak-$name/hang.txt ]; then res=HANG
  else res="NO END ($(tail -1 $L 2>/dev/null | cut -c1-80))"; fi
  checks=$(grep -o '([0-9]* hash checks' $L 2>/dev/null | tr -dc 0-9)
  qs=$(grep -c "\[QS\] loaded slot" $L 2>/dev/null)
  printf '%-28s %-40s %5ss  checks=%s loads=%s\n' "$name" "$res" "$dt" "${checks:-0}" "$qs" > run/det/soak-$name.res
}

names=(); n=0
for R in "$@"; do
  [ -f "$R/input.pcjr" ] || continue
  n=$((n+1)); names+=("$(basename "$R")"); rm -f run/det/soak-$(basename "$R").res
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
  one "$R" $((70 + n)) &
  sleep 2   # stagger the boots (Xvfb start, disc reads)
done
wait
fail=0
for name in "${names[@]}"; do
  if [ -f run/det/soak-$name.res ]; then cat run/det/soak-$name.res; else echo "$name  (no result)"; fi | tee -a $OUT
  grep -q " PASS " run/det/soak-$name.res 2>/dev/null || fail=1
done
echo "$n recordings, $( [ $fail = 0 ] && echo all exact || echo FAILURES ) in $(( $(date +%s) - T0 ))s (jobs=$JOBS nodraw=$NODRAW)" | tee -a $OUT
exit $fail
