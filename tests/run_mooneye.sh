#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner=$1
base=$root/tests/roms/mooneye-test-suite/acceptance

for rom in \
  "$base/instr/daa.gb" \
  "$base/timer/div_write.gb" \
  "$base/oam_dma/basic.gb"; do
  [ -f "$rom" ] || continue
  "$runner" "$rom" 7200 --require-regs
done
