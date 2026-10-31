#!/bin/sh
# Run the Windows build (build-win/pcj.exe) under Wine, the same way run.sh runs
# the Linux build: tools/route.py uses it with PCJ_RUN=tools/run_wine.sh.
# docs/windows-build.md.
DISC=${DISC:-/home/claude/pcj/retail/disc_full}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAGE=${PCJ_WIN_STAGE:-$ROOT/dist/stage}
cd "$ROOT" && mkdir -p run/save-win
export WINEPREFIX=${WINEPREFIX:-$ROOT/win-deps/wineprefix}
export WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=${WINEDEBUG:--all}
# The exe is a GUI program: its console never reaches this script's stdout. Log to a file and
# copy it to stdout (route.py's log.txt).
LOGF="$ROOT/run/save-win/pcj-log-wine.txt"; : > "$LOGF"
export PCJ_LOG="Z:$(echo "$LOGF" | tr / '\\')"
tail -n +1 -F "$LOGF" 2>/dev/null &
TAILPID=$!
trap 'kill $TAILPID 2>/dev/null' EXIT
unset RECOMP_FB_DUMP RECOMP_FB_DUMP_FLIPS
/usr/lib/wine/wine64 "$STAGE/pcj.exe" "Z:$(echo "$DISC" | tr / '\\')" "Z:$(echo "$ROOT/run/save-win" | tr / '\\')"
