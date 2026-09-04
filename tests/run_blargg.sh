#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner=$1
roms=$root/tests/roms

run_if_present() {
  if [ -f "$1" ]; then
    "$runner" "$1" 10000 --require-pass
  fi
}

run_if_present "$roms/instr_timing/instr_timing.gb"
for rom in "$roms"/mem_timing/individual/*.gb; do
  [ -f "$rom" ] || continue
  run_if_present "$rom"
done
if [ -d "$roms/cpu_instrs/individual" ]; then
  for rom in "$roms"/cpu_instrs/individual/*.gb; do
    [ -f "$rom" ] || continue
    run_if_present "$rom"
  done
else
  run_if_present "$roms/cpu_instrs/cpu_instrs.gb"
fi
