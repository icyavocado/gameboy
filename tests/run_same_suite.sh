#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")

for rom in \
  "$root/tests/roms/same-suite/apu/channel_3/channel_3_stop_delay.gb" \
  "$root/tests/roms/same-suite/apu/channel_3/channel_3_delay.gb" \
  "$root/tests/roms/same-suite/apu/channel_3/channel_3_first_sample.gb" \
  "$root/tests/roms/same-suite/apu/channel_3/channel_3_wave_ram_dac_on_rw.gb"; do
  [ -f "$rom" ] || continue
  "$runner" "$rom" 1200 --require-regs
done
