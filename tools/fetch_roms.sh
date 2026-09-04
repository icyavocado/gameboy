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
printf '%s\n' "Downloaded baseline Blargg ROMs to $out"
