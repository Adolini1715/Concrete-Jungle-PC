#!/bin/sh
# Regenerate pregen/hybrid: the hybridize output the Windows installer builds with,
# so it needs neither clang nor libclang. Run after changing tools/hybridize/targets.txt,
# mods/, or the decompilation commit the installer pins (installer/install.py DECOMP_REV).
#   PCJ_DECOMP=<decomp checkout at DECOMP_REV> sh tools/pregen_hybrid.sh
set -e
cd "$(dirname "$0")/.."
: "${PCJ_DECOMP:?set PCJ_DECOMP to the decompilation checkout}"
export PCJ_DECOMP
rm -rf pregen/hybrid
python3 tools/hybridize/hybridize.py gen $(grep -v '^#' tools/hybridize/targets.txt) -o pregen/hybrid
# the preprocessed mod sources live in a random temp folder: keep the output stable
sed -i -E 's#[./]*/?tmp/hymod_[a-z0-9_]+/#mods-preprocessed/#g' pregen/hybrid/*.cpp pregen/hybrid/*.c pregen/hybrid/manifest.json
echo "pregen/hybrid: $(ls pregen/hybrid | wc -l) files"
