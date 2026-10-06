#!/bin/bash

set -e

cd "$(dirname "$0")"

# Reader body fonts were removed with the ebook reader; only UI faces remain.

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

# Latin (+ Latin extended, Cyrillic, Greek from the converter's default
# intervals) only: the firmware is Chinese-only since 2.7.5, CJK comes from the
# Noto Sans SC faces, and Latin text is mostly Wi-Fi names and versions. The
# Hebrew/Arabic coverage that served the RTL language packs is gone.

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    # Ubuntu lacks the Latin Extended Additional block (U+1EA0-U+1EF9) used for
    # Vietnamese tone marks. Append a Vietnamese-only Ubuntu cut so those glyphs
    # are filled from it while every glyph Ubuntu already has stays unchanged
    # (fontstack is ordered by descending priority).
    viet_path="../builtinFonts/source/Ubuntu/Ubuntu-Vietnamese-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $viet_path > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
