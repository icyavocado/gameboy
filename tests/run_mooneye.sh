#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner=$1
base=$root/tests/roms/mooneye-test-suite/acceptance

for rom in \
  "$base/instr/daa.gb" \
  "$base/timer/div_write.gb" \
  "$base/oam_dma/basic.gb" \
  "$base/oam_dma/reg_read.gb" \
  "$base/ppu/intr_2_0_timing.gb" \
  "$base/ppu/intr_2_mode0_timing.gb" \
  "$base/ppu/intr_2_mode0_timing_sprites.gb" \
  "$base/ppu/intr_2_mode3_timing.gb" \
  "$base/ppu/intr_2_oam_ok_timing.gb" \
  "$base/ppu/stat_lyc_onoff.gb" \
  "$base/ppu/stat_irq_blocking.gb" \
  "$base/timer/rapid_toggle.gb" \
  "$base/timer/tim00.gb" \
  "$base/timer/tim01.gb" \
  "$base/timer/tim10.gb" \
  "$base/timer/tim11.gb" \
  "$base/timer/tim00_div_trigger.gb" \
  "$base/timer/tim01_div_trigger.gb" \
  "$base/timer/tim10_div_trigger.gb" \
  "$base/timer/tim11_div_trigger.gb" \
  "$base/timer/tima_reload.gb" \
  "$base/timer/tima_write_reloading.gb" \
  "$base/timer/tma_write_reloading.gb"; do
  [ -f "$rom" ] || continue
  "$runner" "$rom" 7200 --require-regs
done
