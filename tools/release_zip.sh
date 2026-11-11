#!/bin/sh
# The release download players unzip: the repo's files at the top of the zip (no wrapping folder),
# so "open the zip, select all, drag into your Concrete Jungle folder, Replace all, run Install.bat"
# updates an install in place. Nothing the player owns is in it: their game, saves, settings and
# texture packs live in "Predator Concrete Jungle PC" and their disc in "PUT YOUR GAME HERE"
# (only its README is in the repo).
#   tools/release_zip.sh <tag> [out.zip]      (run in the public repo checkout)
set -e
TAG=$1; OUT=${2:-Concrete-Jungle-PC-$TAG.zip}
[ -n "$TAG" ] || { echo "usage: $0 <tag> [out.zip]"; exit 1; }
git archive --format=zip -o "$OUT" "$TAG"
unzip -l "$OUT" | grep -q " Install.bat$" || { echo "no Install.bat at the top of $OUT"; exit 1; }
if unzip -l "$OUT" | grep -E " Predator Concrete Jungle PC/| pcj\.ini$| save/"; then echo "player files in the zip"; exit 1; fi
echo "$OUT: $(unzip -l "$OUT" | tail -1)"
