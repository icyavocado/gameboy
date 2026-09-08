#!/bin/sh
# GBMicrotest suite: run each ROM briefly, then check the HRAM result flag.
# Protocol (aappleby/GBMicrotest): 0xFF82 holds 0x01 on pass, 0xFF on fail.
# Most tests settle within 2 frames; is_if_set_during_ime0 needs ~380ms.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner=$1
base=$root/tests/roms/gbmicrotest

pass=0
fail=0
failed=""
for rom in "$base"/*.gb; do
  [ -f "$rom" ] || continue
  case "$(basename "$rom")" in
    is_if_set_during_ime0.gb) frames=24 ;;
    *) frames=2 ;;
  esac
  if "$runner" "$rom" "$frames" --require-hram 0xff82 0x01 >/dev/null 2>&1; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed="$failed $(basename "$rom")"
  fi
done

echo "gbmicrotest: $pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
  echo "failed:$failed"
  exit 1
fi
