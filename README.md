# Game Boy Emulator

This repository is under active development. The current Phase 0 slice builds a
C11 core library, SDL frontend, headless runner, ncurses TUI, and smoke tests.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a ROM with `build/gb-sdl game.gb`, or use `build/gb-tui game.gb` for the
standalone debugger-oriented terminal frontend. Enable the SDL side-by-side
debug mode with `cmake -S . -B build -DGB_ENABLE_TUI=ON` and
`build/gb-sdl --debug game.gb`.

DMG CPU, cartridge, PPU, APU, CGB, persistence, link cable, and full debugger
support are being implemented according to `SPEC.md`.
