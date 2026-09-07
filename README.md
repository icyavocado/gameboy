<a id="readme-top"></a>

<div align="center">
  <h1>Game Boy Emulator</h1>
  <p>A C11 DMG and Game Boy Color emulator with SDL, ncurses, headless, link, and WebAssembly frontends.</p>

  <p>
    <a href="https://github.com/icyavocado/gameboy/actions"><img src="https://img.shields.io/github/actions/workflow/status/icyavocado/gameboy/ci.yml?style=for-the-badge" alt="CI status"></a>
    <a href="https://github.com/icyavocado/gameboy/issues"><img src="https://img.shields.io/github/issues/icyavocado/gameboy?style=for-the-badge" alt="Issues"></a>
    <a href="https://github.com/icyavocado/gameboy"><img src="https://img.shields.io/github/last-commit/icyavocado/gameboy?style=for-the-badge" alt="Last commit"></a>
  </p>

  <p>
    <a href="#getting-started">Build</a>
    &middot;
    <a href="#usage">Usage</a>
    &middot;
    <a href="#testing">Testing</a>
    &middot;
    <a href="#roadmap">Roadmap</a>
  </p>
</div>

<details>
  <summary>Table of Contents</summary>
  <ol>
    <li><a href="#about-the-project">About The Project</a></li>
    <li><a href="#getting-started">Getting Started</a></li>
    <li><a href="#usage">Usage</a></li>
    <li><a href="#testing">Testing</a></li>
    <li><a href="#roadmap">Roadmap</a></li>
    <li><a href="#contributing">Contributing</a></li>
    <li><a href="#license">License</a></li>
    <li><a href="#acknowledgments">Acknowledgments</a></li>
  </ol>
</details>

## About The Project

This project is a cycle-driven Game Boy emulator written in portable C11. The
core library is platform-independent; desktop, terminal, headless, link-cable,
and browser frontends sit on top of the same public API.

### Features

| Area | Support |
|---|---|
| CPU | SM83 instruction families, CB operations, interrupts, HALT/STOP, CGB speed switching |
| Video | Timed DMG/CGB PPU modes, backgrounds, windows, sprites, palettes, attributes, DMA |
| Cartridges | ROM-only, MBC1, MBC2, MBC3 with RTC, MBC5, battery RAM |
| Audio | Four DMG APU channels, 48 kHz stereo output, SDL and Web Audio paths |
| Persistence | Battery saves, RTC persistence, versioned save states, five frontend slots plus autosave |
| Debugging | SDL memory/value scanner, flags, disassembly, edits, breakpoints, watchpoints, ncurses panes |
| Link | In-process serial peer and TCP link frontend |
| Frontends | SDL2, ncursesw TUI, headless runner, TCP link runner, WebAssembly website |

The core is intentionally kept in a small number of files while the hardware
behavior settles.

## Built With

- C11
- CMake
- SDL2
- ncursesw
- Emscripten for the WebAssembly build
- POSIX sockets for the TCP link frontend

## Getting Started

### Prerequisites

Desktop builds require:

- C11 compiler such as GCC or Clang
- CMake 3.16 or newer
- pkg-config
- SDL2 development files
- ncursesw development files for the TUI

On Arch Linux, the usual packages are:

```sh
sudo pacman -S --needed base-devel cmake pkgconf sdl2 ncurses
```

### Desktop Build

```sh
cmake -S . -B build-frontend -DGB_ENABLE_TUI=ON
cmake --build build-frontend --parallel
```

To build without the ncurses frontend:

```sh
cmake -S . -B build-frontend -DGB_ENABLE_TUI=OFF
```

## Usage

### SDL Frontend

```sh
build-frontend/gb-sdl path/to/game.gb
```

Optional boot ROM support:

```sh
build-frontend/gb-sdl --boot-rom dmg_boot.bin path/to/game.gb
```

Useful controls:

| Key | Game Boy input |
|---|---|
| Arrow keys | D-pad |
| `Z` | A |
| `X` | B |
| Shift | Select |
| Enter | Start |
| Escape | Settings |
| F5 / F8 | Save / load the selected state slot |
| Bug button | Open the SDL debugger |
| Cog button | Open settings |

The SDL settings panel supports ROM selection, battery saves, state slots,
volume, palette, speed, key remapping, reset, and quit. Battery saves are kept
beside the ROM as `.sav`; state files use `.state`, numbered slots, and an
`.auto` slot.

### SDL Debugger

Open the debugger with the bug button or use SDL debug mode when TUI support is
enabled:

```sh
build-frontend/gb-sdl --debug path/to/game.gb
```

The debugger shows registers, Z/N/H/C flags, IME/HALT state, disassembly,
memory, breakpoints, and watchpoints. Its memory scanner supports:

- Exact decimal searches from `0` to `65535`
- One-byte and two-byte searches
- Little- and big-endian matching
- Refinement by exact value, changed, or unchanged
- Click-to-edit writes through the normal hardware bus

### TUI Debugger

```sh
build-tui/gb-tui path/to/game.gb
```

Keys include `s` step, `c` continue to breakpoint, `f` run a frame, `b` toggle
a PC breakpoint, `w` add a write watchpoint, `g` go to a memory address, `e`
edit the selected byte, arrows move the memory cursor, Space pauses/runs, and
`q` quits.

### Headless Runner

```sh
build-frontend/gb-headless path/to/game.gb 60
```

The runner can capture serial output and validate ROM test results:

```sh
build-frontend/gb-headless test.gb 10000 --require-pass
build-frontend/gb-headless test.gb 10000 --expect "Passed"
```

### TCP Link Runner

Start one listener and one connector with the same compatible ROM:

```sh
build-frontend/gb-link game.gb listen 5000 600
build-frontend/gb-link game.gb connect 127.0.0.1 5000 600
```

### WebAssembly Website

Install Emscripten, then build the browser target:

```sh
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-wasm -DGB_ENABLE_TUI=OFF
cmake --build build-wasm --parallel
```

Serve the generated shell over HTTP:

```sh
python3 -m http.server 8080 --directory build-wasm
```

Open <http://localhost:8080/gb-wasm.html>. The browser build supports ROM file
loading, video, keyboard and touch input, Web Audio, per-ROM battery save
slots with configurable auto-save, and local save states.

The on-page Settings button exposes volume (muted by default), palette,
speed, save slots, auto-save interval, key remapping, and a portrait touch
keypad. Battery saves and save states can be downloaded as `.sav` / `.state`
files and imported back later. A GitHub Actions job builds the same site and
deploys it to GitHub Pages on every `main` push.

## Testing

Run the fast local tests:

```sh
ctest --test-dir build-frontend --output-on-failure -R 'core|input|gameplay'
```

Run the Tetris smoke test:

```sh
build-frontend/gb-headless tests/roms/tetris.gb 60
```

The full CTest configuration also runs fetched Blargg, Mooneye, SameSuite,
and sound ROMs when they are present under the git-ignored `tests/roms/` tree.
Fetch the supported test ROM collection with:

```sh
tools/fetch_roms.sh
ctest --test-dir build-frontend --output-on-failure
```

Some external ROM suites are model- or hardware-revision-specific. The test
harness only registers suites with a defined result protocol and skips missing
ROM assets cleanly.

## Roadmap

- [x] DMG and CGB CPU, memory, timer, PPU, cartridge, persistence, and audio foundations
- [x] SDL frontend with settings, debugger, save slots, audio, and input mapping
- [x] ncurses debugger frontend
- [x] In-process and TCP serial transport
- [x] WebAssembly browser build with canvas, Web Audio, and local persistence
- [ ] Improve cycle-level PPU FIFO accuracy across all hardware revisions
- [ ] Expand CGB and APU acceptance coverage
- [ ] Add browser gamepad support
- [x] Publish a hosted WebAssembly demo via GitHub Pages CI

## Contributing

1. Fork the project.
2. Create a branch: `git checkout -b feature/my-change`.
3. Make the smallest focused change that fits the existing architecture.
4. Build with `-Wall -Wextra -Werror` and run the relevant CTest targets.
5. Open a pull request with the behavior change and verification commands.

Please do not commit ROMs, save files, state files, Emscripten build output, or
API keys. ROM assets belong in the ignored `tests/roms/` directory.

## License

No license file has been added to this repository yet. Treat the project as
all-rights-reserved until a license is explicitly selected.

## Acknowledgments

- [Best README Template](https://github.com/othneildrew/Best-README-Template) for the documentation structure
- [Pan Docs](https://gbdev.io/pandocs/) for Game Boy hardware documentation
- [Gekkio's Game Boy CPU Manual](https://gekkio.fi/files/gb-docs/gbctr.pdf)
- [Blargg test ROMs](https://github.com/retrio/gb-test-roms)
- [Mooneye test suite](https://github.com/Gekkio/mooneye-test-suite)
- [SameSuite](https://github.com/c-sp/gameboy-test-roms)

<p align="right"><a href="#readme-top">back to top</a></p>
