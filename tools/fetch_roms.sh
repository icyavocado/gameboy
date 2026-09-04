#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/tests/roms"
mkdir -p "$out"
repo=${GB_TEST_ROMS_REPO:-https://github.com/retrio/gb-test-roms.git}
tmp=${TMPDIR:-/tmp}/gb-test-roms
if [ ! -d "$tmp/.git" ]; then rm -rf "$tmp"; git clone --depth 1 "$repo" "$tmp"; fi
cp -R "$tmp/cpu_instrs" "$out/"
cp -R "$tmp/instr_timing" "$out/"
cp -R "$tmp/mem_timing" "$out/"

collection_url=${GB_TEST_COLLECTION_URL:-https://github.com/c-sp/game-boy-test-roms/releases/download/v7.0/game-boy-test-roms-v7.0.zip}
archive=${TMPDIR:-/tmp}/game-boy-test-roms-v7.0.zip
if [ ! -f "$archive" ]; then
  curl -L --fail --silent --show-error "$collection_url" -o "$archive"
fi
unzip -q -o "$archive" 'mooneye-test-suite/acceptance/instr/daa.gb' \
  'mooneye-test-suite/acceptance/timer/div_write.gb' \
  'mooneye-test-suite/acceptance/oam_dma/basic.gb' \
  'mooneye-test-suite/acceptance/ei_sequence.gb' \
  -d "$out/.collection"
rm -rf "$out/mooneye-test-suite"
mv "$out/.collection/mooneye-test-suite" "$out/mooneye-test-suite"
rm -rf "$out/.collection"
printf '%s\n' "Downloaded Blargg and selected Mooneye ROMs to $out"
