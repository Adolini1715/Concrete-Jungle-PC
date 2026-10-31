#!/bin/sh
# Full recompiler pipeline from a clean checkout (RUN.md), for a workspace without work/.
# Regenerates build/hybrid from tools/hybridize/targets.txt, then src/recomp/gen.
set -e
cd "$(dirname "$0")/.."
[ -d pcj-decomp/include ] && D=$PWD/pcj-decomp || D=/home/claude/pcj-decomp
export PCJ_DECOMP=${PCJ_DECOMP:-$D}
python3 tools/hybridize/hybridize.py gen $(grep -v '^#' tools/hybridize/targets.txt) -o build/hybrid
cat build/hybrid/hybrid_swap.c src/det_file.c src/femouse.c src/native_gfx.c src/native_dsound.c src/mods_archive.c > build/exclude_manual.c
cd xboxrecomp
python3 -m tools.xbe_parser ../game/default.xbe --json ../game/default_analysis.json
[ -f ../work/disasm/functions.json ] || python3 -m tools.disasm ../game/default.xbe -v --seed-functions ../game/map_seeds.json -o ../work/disasm
python3 ../tools/merge_map.py ../work/disasm/functions.json "$PCJ_DECOMP/symbols/functions.csv"
[ -d ../work/func_id ] || python3 -m tools.func_id ../game/default.xbe --functions ../work/disasm/functions.json \
    --strings ../work/disasm/strings.json --xrefs ../work/disasm/xrefs.json -o ../work/func_id
[ -d ../work/abi ] || python3 -m tools.abi_analysis ../game/default.xbe --disasm-dir ../work/disasm \
    --func-id-dir ../work/func_id --output-dir ../work/abi
rm -rf ../src/recomp/gen
python3 -m tools.recomp ../game/default.xbe --all --split 250 --game-name "Predator: Concrete Jungle" \
    --gen-dir ../src/recomp/gen --disasm-dir ../work/disasm --func-id-dir ../work/func_id \
    --abi-dir ../work/abi -o ../work/recomp --manual-functions ../game/manual_functions.json \
    --exclude-manual ../build/exclude_manual.c
echo REGEN OK
