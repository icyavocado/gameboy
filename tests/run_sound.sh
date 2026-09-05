#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner=$1
rom=$root/tests/roms/dmg_sound/dmg_sound.gb

if [ -f "$rom" ]; then
  "$runner" "$rom" 300 --require-memory 0xa000 0x80
fi
