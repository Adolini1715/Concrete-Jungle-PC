#!/bin/sh
# Stage and zip the Windows build: dist/stage (exe + DLLs, used by run_wine.sh)
# and dist/pcj-win64-<date>.zip (+ README-PC.txt, pcj.ini, run.bat from
# dist/pkgsrc). No XBE, no game data. docs/windows-build.md.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p dist/stage
WIN=${PCJ_WIN_BUILD:-build-winclang}; [ -f $WIN/pcj.exe ] || WIN=build-win
cp $WIN/pcj.exe win-deps/sdl2/bin/SDL2.dll win-deps/epoxy/bin/libepoxy-0.dll \
   /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll dist/stage/
x86_64-w64-mingw32-strip -o dist/stage/pcj.exe $WIN/pcj.exe
mkdir -p dist/symbols && cp $WIN/pcj.exe dist/symbols/pcj-$(git rev-parse --short HEAD).exe
NAME=pcj-win64-$(date +%Y%m%d)
TMP=$(mktemp -d) && mkdir "$TMP/$NAME"
cp dist/stage/pcj.exe dist/stage/*.dll dist/pkgsrc/pcj.ini dist/pkgsrc/run.bat "$TMP/$NAME/"
sed "s/\r*$/\r/" dist/pkgsrc/README-PC.txt > "$TMP/$NAME/README-PC.txt"
rm -f "dist/$NAME.zip"
(cd "$TMP" && zip -q -9 -r "$ROOT/dist/$NAME.zip" "$NAME")
rm -rf "$TMP"
ls -la "dist/$NAME.zip"
unzip -l "dist/$NAME.zip"
