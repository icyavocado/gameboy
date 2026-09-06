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
cp -R "$tmp/dmg_sound" "$out/"

collection_url=${GB_TEST_COLLECTION_URL:-https://github.com/c-sp/game-boy-test-roms/releases/download/v7.0/game-boy-test-roms-v7.0.zip}
archive=${TMPDIR:-/tmp}/game-boy-test-roms-v7.0.zip
if [ ! -f "$archive" ]; then
  curl -L --fail --silent --show-error "$collection_url" -o "$archive"
fi
unzip -q -o "$archive" 'mooneye-test-suite/acceptance/instr/daa.gb' \
  'mooneye-test-suite/acceptance/timer/div_write.gb' \
  'mooneye-test-suite/acceptance/oam_dma/basic.gb' \
  'mooneye-test-suite/acceptance/oam_dma/reg_read.gb' \
  'mooneye-test-suite/acceptance/ppu/intr_2_0_timing.gb' \
  'mooneye-test-suite/acceptance/ppu/intr_2_mode0_timing.gb' \
  'mooneye-test-suite/acceptance/ppu/intr_2_mode0_timing_sprites.gb' \
  'mooneye-test-suite/acceptance/ppu/intr_2_mode3_timing.gb' \
  'mooneye-test-suite/acceptance/ppu/intr_2_oam_ok_timing.gb' \
  'mooneye-test-suite/acceptance/ppu/stat_lyc_onoff.gb' \
  'mooneye-test-suite/acceptance/ppu/stat_irq_blocking.gb' \
  'mooneye-test-suite/acceptance/timer/rapid_toggle.gb' \
  'mooneye-test-suite/acceptance/timer/tim00.gb' \
  'mooneye-test-suite/acceptance/timer/tim01.gb' \
  'mooneye-test-suite/acceptance/timer/tim10.gb' \
  'mooneye-test-suite/acceptance/timer/tim11.gb' \
  'mooneye-test-suite/acceptance/timer/tim00_div_trigger.gb' \
  'mooneye-test-suite/acceptance/timer/tim01_div_trigger.gb' \
  'mooneye-test-suite/acceptance/timer/tim10_div_trigger.gb' \
  'mooneye-test-suite/acceptance/timer/tim11_div_trigger.gb' \
  'mooneye-test-suite/acceptance/timer/tima_reload.gb' \
  'mooneye-test-suite/acceptance/timer/tima_write_reloading.gb' \
  'mooneye-test-suite/acceptance/timer/tma_write_reloading.gb' \
  'mooneye-test-suite/acceptance/ei_sequence.gb' \
  'same-suite/apu/channel_3/channel_3_stop_delay.gb' \
  'same-suite/apu/channel_3/channel_3_delay.gb' \
  'same-suite/apu/channel_3/channel_3_first_sample.gb' \
  'same-suite/apu/channel_3/channel_3_wave_ram_dac_on_rw.gb' \
  -d "$out/.collection"
rm -rf "$out/mooneye-test-suite"
mv "$out/.collection/mooneye-test-suite" "$out/mooneye-test-suite"
rm -rf "$out/same-suite"
mv "$out/.collection/same-suite" "$out/same-suite"
rm -rf "$out/.collection"
printf '%s\n' "Downloaded Blargg and selected Mooneye ROMs to $out"
