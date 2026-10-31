#!/bin/bash
# Rebuild packs/sharp-ui from the dumped originals (CJ_TEX_DUMP=1 dump folder).
# usage: build_sharp_ui.sh <dump dir>
set -e
D=${1:?dump dir}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/packs/sharp-ui/textures/replace
L=/usr/share/fonts/truetype/liberation
F=$ROOT/tools/art/fonts.py
mkdir -p "$OUT"
# menu body font (Arial Bold style)
python3 $F $D/8662cbf2181cf340_256x128_0E.png $OUT/8662cbf2181cf340_font_bold.png $L/LiberationSans-Bold.ttf \
  '!"#$%&'"'"'()*+,-./0123456789' ':;<=>?@ABCDEFGHIJKLMNO' 'PQRSTUVWXYZ[\]^_`abcde' 'fghijklmnopqrstuvwxyz®©'
# titles / item descriptions (heavy italic)
python3 $F $D/531ef27bd120d318_256x128_0E.png $OUT/531ef27bd120d318_font_italic.png $L/LiberationSans-BoldItalic.ttf \
  '!"#$%&'"'"'()*+,-./0123456789' ':;<=>?@ABCDEFGHIJKLMN' 'OPQRSTUVWXYZ[\]^_`abc' 'defghijklmnopqrstuvwxyz' '®©' --stroke 0.025
# small labels (controller diagram, footers)
python3 $F $D/9594b0b78805ac05_256x64_0E.png $OUT/9594b0b78805ac05_font_small.png $L/LiberationSans-Bold.ttf \
  '!"#$%&'"'"'()*+,-./0123456789:;<=>?@AB' 'CDEFGHIJKLMNOPQRSTUVWXYZ[\]^' '_`abcdefghijklmnopqrstuvwxyz®©' --stroke 0.012
# save-slot frames (selected / plain): traced and redrawn as vectors
python3 $ROOT/tools/art/trace.py $D/395ad90729c73c4e_256x64_0E.png $OUT/395ad90729c73c4e_frame_slot.png
python3 $ROOT/tools/art/trace.py $D/3b59f8081124d76c_256x64_0E.png $OUT/3b59f8081124d76c_frame_box.png
# footer / prompt buttons and d-pad
python3 $ROOT/tools/art/buttons.py $OUT
# --- in-game HUD (needs a dump taken in a level) ---
if [ -f $D/93f9cab9e029acd6_256x128_0E.png ]; then
python3 $F $D/93f9cab9e029acd6_256x128_0E.png $OUT/93f9cab9e029acd6_font_hud.png $L/LiberationSans-Bold.ttf \
  '!"#$%&'"'"'()*+,-./0123456789' ':;<=>?@ABCDEFGHIJKLMNO' 'PQRSTUVWXYZ[\]^_`abcde' 'fghijklmnopqrstuvwxyz®'
for k in 2ed2e074fb667bc3_128x128_0E:radar_ring c81879552d133bb1_128x128_0E:hud_panels \
         4b1408a4f24be74b_128x128_0E:hud_panel_backs 971064eb47f72b46_32x32_0E:hud_energy; do
  python3 $ROOT/tools/art/trace.py $D/${k%%:*}.png $OUT/${k:0:16}_${k##*:}.png --scale 8
done
python3 $F $D/8c9d3a9be7a4eacd_16x16_0E.png $OUT/8c9d3a9be7a4eacd_radar_n.png $L/LiberationSans-Bold.ttf 'N' --scale 8
fi
