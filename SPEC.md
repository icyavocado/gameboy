# Game Boy / Game Boy Color Emulator in C — Project Spec

## 1. Decisions

| Item | Choice |
|---|---|
| Language | C11, gcc/clang, `-Wall -Wextra -Werror -std=c11`, ASan/UBSan in debug builds |
| Build | CMake; targets `gb` (static core lib), `gb-sdl`, `gb-headless`, `gb-test`, `gb-wasm` |
| Hardware | DMG + CGB (auto-detect from cartridge header, overridable) |
| Frontend | SDL2 (Emscripten ships it as a port via `-sUSE_SDL=2`, making the WASM build trivial) |
| Debugger | Core API in `libgb` + ncursesw TUI running in the launching terminal, side-by-side with the SDL game window (`gb-sdl --debug rom.gb`). CMake option `GB_ENABLE_TUI` (default ON on Linux/macOS, OFF for Emscripten) |
| Audio | Full APU, 48 kHz stereo int16, audio-driven frame pacing |
| Persistence | Battery `.sav` + RTC; versioned pointer-free save states (portable desktop <-> web) |
| Link cable | Transport callback abstraction; in-process two-instance demo, then TCP |
| Testing | Vendored single-header unit test framework (`utest.h`) + headless runner for Blargg / Mooneye / dmg-acid2 / cgb-acid2, all run in GitHub Actions |
| Boot ROM | Skip by default with correct post-boot register state; load user-supplied `dmg_boot.bin` / `cgb_boot.bin` if present |
| Standalone terminal renderer | Dropped (out of scope) |
| Goal | Portfolio project: clean code, README with screenshots/GIF, test pass table, live web demo |

## 2. Architecture Principles

1. **Core is a pure, platform-free library (`libgb`).** No SDL, no stdio for I/O, no globals. All state lives in a `gb_t` struct. This enables WASM, tests, the debugger and save states.
2. **Frontends are thin.** SDL2 desktop, Emscripten/WASM, headless test runner. They call `gb_run_frame()`, read the framebuffer, consume audio samples, feed input.
3. **Cycle-driven design.** CPU executes one instruction and reports T-cycles; PPU/APU/timer/DMA/serial are then advanced by that many cycles. Accurate enough for Mooneye tests and dmg-acid2/cgb-acid2. A per-M-cycle refactor is possible later but not planned.
4. **Save states are `memcpy` of `gb_t` plus a version header.** `gb_t` must contain no pointers and only fixed-width types. ROM and cart RAM are stored separately and rehydrated on load.
5. **Debug hook.** CPU calls `gb_dbg_before_instr()` before each instruction when debugging is enabled; compiles to nothing otherwise.

## 3. Toolchain (verified on dev machine)

- gcc 16.2 / clang 22.1, CMake 4.4, GNU Make 4.4, pkg-config
- SDL2 2.32 (also SDL3 available, not used)
- ncursesw 6.6
- Perl 5.42 with core `JSON::PP` (for `tools/gen_opcodes.pl`)
- Emscripten: **not installed**; `sudo pacman -S emscripten` at Phase 8
- Unit test framework: none system-wide; vendor `utest.h` into `tests/unit/`

## 4. Directory Layout

```
gameboy/
├── CMakeLists.txt
├── README.md                  # portfolio-facing: GIF, feature table, test pass table, build docs
├── SPEC.md                    # this file
├── src/core/                  # libgb — no platform deps
│   ├── gb.h / gb.c            # public API
│   ├── cpu.c / cpu.h          # SM83: registers, fetch/decode/execute, interrupts, HALT/STOP, CGB double speed
│   ├── opcodes.c              # generated dispatch table incl. CB prefix (committed)
│   ├── mmu.c / mmu.h          # memory map, bus read/write, I/O dispatch, OAM DMA, HDMA/GDMA
│   ├── cart.c / cart.h        # header parsing, MBC1/2/3(+RTC)/5, battery RAM, RTC
│   ├── ppu.c / ppu.h          # modes 0-3, scanline -> pixel FIFO renderer, DMG palettes, CGB palettes/attrs/VRAM bank
│   ├── apu.c / apu.h          # 4 channels, frame sequencer, mixer, downsampler to 48 kHz
│   ├── timer.c / timer.h      # DIV/TIMA/TMA/TAC incl. obscure behaviors
│   ├── joypad.c               # P1 register
│   ├── serial.c / serial.h    # link cable: internal/external clock, pluggable transport callback
│   ├── debugger.c / .h        # breakpoints, watchpoints, step, disassembler, memory inspection
│   └── state.c                # save state serialize/deserialize with version header
├── src/frontend/sdl/          # main.c: window, integer scaling, audio queue, input map, drag-drop ROM, hotkeys
├── src/frontend/tui/          # tui.c (ncursesw panes), cmd.c (command parser; also plain-stdin mode)
├── src/frontend/wasm/         # main_wasm.c, shell.html, glue.js (file picker, canvas, IDBFS saves)
├── src/frontend/headless/     # runner.c: runs N frames, checks serial output / screen hash / Mooneye regs
├── tests/
│   ├── unit/                  # utest.h; CPU ALU/flags, MBC bank math, timer edges, state round-trip
│   ├── roms/                  # fetched by tools/fetch_roms.sh (gitignored)
│   └── expected/              # reference screenshots / hashes for acid2
├── tools/
│   ├── gen_opcodes.pl         # generates opcodes.c from gbdev opcode JSON
│   └── fetch_roms.sh          # downloads Blargg, Mooneye, dmg-acid2, cgb-acid2
├── docs/                      # hardware quirk notes, architecture diagram
└── .github/workflows/ci.yml
```

## 5. Public Core API (`src/core/gb.h`)

```c
typedef enum { GB_MODEL_AUTO, GB_MODEL_DMG, GB_MODEL_CGB } gb_model_t;

typedef void (*gb_audio_cb)(void* user, const int16_t* stereo, size_t frames);
typedef void (*gb_serial_cb)(void* user, uint8_t out, uint8_t* in);   // link cable transport

gb_t*  gb_create(void);
void   gb_destroy(gb_t*);
int    gb_load_rom(gb_t*, const uint8_t* rom, size_t len);
int    gb_load_boot_rom(gb_t*, const uint8_t* rom, size_t len);   // optional
void   gb_set_model(gb_t*, gb_model_t);
void   gb_reset(gb_t*);

void   gb_run_frame(gb_t*);                          // ~70224 T-cycles (x2 in double speed)
const uint32_t* gb_framebuffer(const gb_t*);         // 160x144 RGBA8888
void   gb_set_audio_callback(gb_t*, gb_audio_cb, void* user);
void   gb_set_input(gb_t*, uint8_t buttons);         // bitmask: R L U D A B Sel Start
void   gb_set_serial_callback(gb_t*, gb_serial_cb, void* user);

/* persistence */
size_t gb_save_ram_size(const gb_t*);
size_t gb_save_ram(const gb_t*, uint8_t* out);
int    gb_load_ram(gb_t*, const uint8_t*, size_t);
size_t gb_save_state_size(const gb_t*);
size_t gb_save_state(const gb_t*, uint8_t* out);
int    gb_load_state(gb_t*, const uint8_t*, size_t);

/* debugger */
typedef enum { GB_BP_PC, GB_BP_READ, GB_BP_WRITE } gb_bp_kind_t;
typedef struct { gb_bp_kind_t kind; uint16_t addr; } gb_bp_t;
typedef struct { uint16_t af, bc, de, hl, sp, pc; bool ime, halted; } gb_regs_t;

void    gb_dbg_enable(gb_t*, bool);
int     gb_dbg_step(gb_t*);                          // one instruction; returns T-cycles
int     gb_dbg_run_until_break(gb_t*);               // returns bp id or -1 on frame end
int     gb_dbg_add_bp(gb_t*, gb_bp_t);
void    gb_dbg_del_bp(gb_t*, int id);
int     gb_dbg_disasm(const gb_t*, uint16_t addr, char* buf, size_t len); // returns instr length
uint8_t gb_dbg_read(const gb_t*, uint16_t addr);
void    gb_dbg_write(gb_t*, uint16_t addr, uint8_t val);
void    gb_dbg_regs(const gb_t*, gb_regs_t* out);
```

## 6. Phases and Exit Criteria

| # | Phase | Scope | Exit criteria |
|---|---|---|---|
| 0 | Skeleton | git init, CMake with all targets + `GB_ENABLE_TUI`, `gb_t` skeleton, blank 160x144 SDL window, headless stub, `utest.h` wired to `ctest`, `fetch_roms.sh`, CI | All targets build, one passing unit test, CI green, ROMs fetched |
| 1 | CPU + memory | Header parsing, Nintendo logo check, ROM-only + MBC1, full SM83 set via generated table, IME/IE/IF, EI delay, HALT bug, timer, joypad, serial stub to stdout | Blargg `cpu_instrs` 11/11, `instr_timing`, `mem_timing` pass headless; unit tests for DAA, ADD SP, flags |
| 2 | PPU (DMG) | Mode state machine, STAT IRQs, LY/LYC, scanline renderer (BG/window/sprites, priority), upgrade to pixel FIFO, OAM DMA | dmg-acid2 pixel-perfect; Tetris, Dr. Mario, Link's Awakening, Pokemon Red playable; Mooneye `ppu/`, `timer/` mostly green |
| 3 | Cart + persistence | MBC2, MBC3+RTC, MBC5, large banking; `.sav` next to ROM (on exit + periodic), RTC persisted; versioned save states with slots, F5/F8 hotkeys | Mooneye `emulator-only/mbc*` pass; saves/states round-trip (unit tested); Pokemon saves survive restart |
| 4 | APU | Square1 (sweep), Square2, Wave, Noise; 512 Hz frame sequencer; length/envelope; NR50-52; DAC on/off; downsample to 48 kHz stereo int16 ring buffer; `SDL_QueueAudio`; audio-driven sync | Blargg `dmg_sound` mostly pass; no crackle |
| 5 | CGB | Header detection, post-boot defaults for both models, KEY1 double speed, VBK, SVBK, BCPS/BCPD/OCPS/OCPD, BG attr map, OPRI, HDMA/GDMA, IR stub, DMG-compat colorization | cgb-acid2 passes; Pokemon Crystal, Zelda Oracle, Shantae playable |
| 6 | Debugger | Core API -> stdin command mode -> ncursesw TUI (disasm, regs + I/O, memory, breakpoints, command line); later VRAM tile viewer, trace log, APU pane | Step/break/watch/inspect alongside SDL window; command parser unit tested |
| 7 | Link cable | Null transport -> two `gb_t` in one process -> TCP between instances | Two-player Tetris works |
| 8 | WASM + polish | Emscripten build (`-sUSE_SDL=2`, `emscripten_set_main_loop`), file picker, IDBFS saves, GitHub Pages demo, README with GIF + test table; extras: scaling filters, fast-forward, rewind, gamepad | Demo live, README complete |

## 7. Debugger TUI (Phase 6)

### Layout

```
+ Disassembly -------------------++ Registers ------------+
|  0150  NOP                     || AF 01B0  BC 0013      |
|  0151  JP   0x0200             || DE 00D8  HL 014D      |
|> 0200  LD   A,(HL)     <bp>    || SP FFFE  PC 0200      |
|  0201  INC  HL                 || Z N H C  IME:1        |
|  ...                           || LY 90  STAT 81  IF 01 |
+ Memory ------------------------++ Breakpoints ----------+
| C000 00 00 00 00 ...  ........ || 1 PC=0200             |
| C010 ...                       || 2 W  FF40             |
+ Command -----------------------++-----------------------+
| > s | c | n | b 0200 | w ff40 | m c000 | q              |
+---------------------------------------------------------+
```

Panes: disassembly (follows PC), registers + key I/O regs, scrollable memory viewer, breakpoints/watchpoints, command line with history.

### Commands

`s` step, `n` step over, `c` continue, `b <addr>` PC breakpoint, `r <addr>` / `w <addr>` read/write watchpoint, `d <id>` delete, `m <addr>` set memory view, `p <addr> <val>` poke, `t` toggle trace, `q` quit.

### Main-loop integration

Single thread. Per iteration: poll SDL events -> if running, `gb_run_frame()` (or `gb_dbg_run_until_break()` when debugging) and present -> `getch()` with `nodelay(TRUE)` -> redraw dirty panes. Breakpoint hit or pause hotkey (either window) sets `paused`; TUI takes focus. `endwin()` on exit and on SDL quit. Handle `KEY_RESIZE`. Use `ncursesw` for box-drawing and the half-block tile viewer.

## 8. Testing Strategy

| Layer | What | How |
|---|---|---|
| Unit | CPU ALU/flags, MBC banking math, timer edge cases, save state round-trip, debugger command parser | `utest.h`, run via `ctest` |
| Blargg | cpu_instrs, instr_timing, mem_timing, dmg_sound, cgb_sound | headless runner captures serial output, checks for "Passed" |
| Mooneye | acceptance/, emulator-only/mbc* | headless runner detects `LD B,B` and checks register fingerprint B=3 C=5 D=8 E=13 H=21 L=34 |
| acid2 | dmg-acid2, cgb-acid2 | framebuffer hash vs reference PNG in `tests/expected/` |
| CI | all of the above on push | GitHub Actions; ROMs fetched by `tools/fetch_roms.sh` (all freely redistributable) |

## 9. Risks and Mitigations

- **Pixel FIFO vs scanline PPU**: scanline covers ~95% of games; FIFO needed for dmg-acid2 window edge cases. Start scanline, refactor to FIFO within Phase 2 when tests demand it.
- **Boot ROM not redistributable**: default to skip-boot with correct post-boot registers; optional user-supplied files.
- **Cartridge logo check**: verify the Nintendo logo at `$0104-$0133` for boot-flow compatibility, but keep loading permissive for homebrew and diagnostics.
- **Save state portability**: `gb_t` pointer-free, fixed-width types, little-endian header with version + model.
- **Audio timing**: audio-driven sync from day one of Phase 4, not vsync-driven.
- **`-Werror` + generated code**: `gen_opcodes.pl` must emit warning-free code; commit generated `opcodes.c`.
- **Emscripten missing**: install at Phase 8.

## 10. References

- Pan Docs — https://gbdev.io/pandocs (primary spec)
- gbdev opcode table JSON — https://gbdev.io/gb-opcodes/Opcodes.json (for `gen_opcodes.pl`)
- SM83 instruction reference — https://gbdev.io/gb-opcodes/optables/
- "The Ultimate Game Boy Talk" (33c3) — PPU/FIFO mental model
- Gekkio, "Game Boy: Complete Technical Reference" — precise timing
- Mooneye-gb and SameBoy sources — for disambiguating edge cases
- Test ROMs: Blargg (gb-test-roms), Mooneye test suite, dmg-acid2, cgb-acid2

## 11. First Session Checklist (Phase 0)

1. `git init`, `.gitignore` (build/, tests/roms/, *.sav, *.state)
2. CMake skeleton: `gb`, `gb-sdl`, `gb-headless`, `gb-test`, `gb-wasm`; option `GB_ENABLE_TUI`
3. `gb.h` public API + `gb_t` struct skeleton, memory map stubs
4. Vendor `utest.h`, one test, `ctest` wired up
5. `tools/fetch_roms.sh` + `.github/workflows/ci.yml`
6. Start CPU: register file, fetch loop, `tools/gen_opcodes.pl` from the gbdev opcode JSON
