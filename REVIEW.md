# Code Review — 2026-09-06 (`367cdf1`, worktree clean)

Scope: full tree — `src/core/gb.{h,c}` (~2k-line monolith), `src/core/opcodes.c`,
`src/frontend/{sdl,headless,tui,link,wasm}`, `tests/`, `CMakeLists.txt`, `SPEC.md`.
Mode: read-only review first; fixes follow in priority order below.

## What looks good

- Public API (`gb.h`) matches SPEC section 5: create/destroy/load, framebuffer,
  audio/serial callbacks, persistence, debugger.
- Test layers match SPEC section 8: unit (`gb-test`), headless serial `--require-pass`
  (Blargg), Mooneye register fingerprint, SameSuite, sound.
- State versioning discipline: `STATE_VERSION 11`, header + version + rom/ram-size
  checks in `gb_load_state`, RTC/APU/CGB fields serialized.
- Layered config: `gameboy.cfg` directory defaults + `<rom>.gb.cfg` per-game overrides,
  game wins, immediate persist, human-editable keys, unknown lines ignored.
- Slot model is coherent: save slot 0 = base `.sav`, slots 1-4 = `.sav2-5`;
  state slot 5 = `.auto`, display `[A][1-5]` with cyclic navigation;
  F5/F8 honor the selected state slot; startup auto-loads ROM + save + state.
- Recent APU work has real regressions (square 512 Hz, wave 256 Hz,
  sweep-disabled frequency stability).

## Findings (ordered)

### P0-1: config writes are not atomic and fail silently

`persist_config` -> `write_config` opens the target with `"w"` and writes line by
line. A crash/signal mid-write truncates `gameboy.cfg` or `<rom>.gb.cfg`.
Write failures are ignored (call sites do not check the return), and a read-only
ROM directory silently loses settings. `LOADROM` relative paths also resolve
against CWD, not against the config file's directory.

Fix: write to `<file>.tmp` + `rename()`; surface a one-line failure indicator in
the settings UI; resolve relative `LOADROM` against the config file directory.

### P0-2: `load_ram` + `gb_reset` ordering needs a regression test

SDL `load_ram()` calls `gb_load_ram()` then `gb_reset()`. This is correct only if
reset preserves cartridge RAM and MBC banking state. Add an explicit test:
load RAM with nonzero content -> reset -> RAM and bank state intact.

### P1-1: SDL frontend is a second monolith

`sdl/main.c` (~1.2k lines) owns settings UI, ROM browser, slots, autosave timers,
config parsing, persistence, boot logo, controller overlay, debug overlay. Extract
`config.c`/`config.h` (parse/serialize/paths/persist) out of `main.c` without
behavior change; consider `settings_ui.c` on the next settings touch.

### P1-2: `persist_config` fan-out

~12 call sites invoke `persist_config` with the same 6 arguments after every
settings mutation. Centralize behind one `settings_changed(...)` helper that
mutates + persists + (later) reports errors, cutting future edit risk.

### P2-1: invalid SM83 opcodes are silent NOPs

`gb.c` intentionally treats invalid opcodes as NOPs (Phase 1 leftover). Keep
release behavior, but trap/log in debug/headless builds so bad ROMs and decoder
bugs fail loudly.

### P2-2: SPEC vs code mismatch on save-state representation

SPEC sections 2/9 describe save states as "`memcpy` of `gb_t`" with a pointer-free
struct, but `struct gb` holds `rom/ram/boot_rom` pointers and serialization is
field-wise with ROM/RAM handled separately. Behavior is sound; fix with a SPEC
one-liner, not a code change.

### Non-blocking observations

- Audio pacing (`SDL_Delay(16/(speed+1))`, drop-when-queue-full) is approximate;
  revisit before claiming SPEC Phase 4 audio-driven sync is done.
- PPU is scanline-based while SPEC targets FIFO for dmg-acid2 edge cases; known
  and documented risk, no action here.
- ~10 local `build-*` directories exist; confirm `.gitignore` covers them.
- TUI/WASM frontends are stubs; link TCP two-player is unproven. Defer unless a
  phase is being claimed.

## Plan

1. P0-1 atomic config write + failure indicator. DONE (`config.c: write_config`
   uses tmp+rename; `settings.config_error` shown in settings panel).
   Relative `LOADROM` resolves against the config file's directory. DONE.
2. P0-2 RAM-survives-reset test. DONE (`core_battery_ram_survives_reset`;
   verified `gb_reset` never touches cartridge RAM).
3. P1-1 extract `config.c`/`config.h`. DONE (types + parse/serialize/paths/persist
   moved out of `sdl/main.c`; `gb-sdl` target updated).
   P1-2 centralize mutation+persist. DONE: the 17 call sites now set
   `settings.dirty`, flushed once per settings key event in `sdl/main.c`
   (single `persist_config` call; immediate-write behavior preserved).
4. P2 invalid-opcode trap + SPEC note. DONE (stderr diagnostic in debug builds only;
   SPEC save-state wording corrected to field-wise serialization).
5. Full build + `core|input|gameplay` (+ Blargg/Mooneye/SameSuite/sound as time allows),
   Tetris smoke, `git diff --check`. DONE for the fast suite; full ROM suites +
   Tetris smoke still to run before commit.
