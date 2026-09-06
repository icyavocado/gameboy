# ISSUE: `intr_2_mode0_timing_sprites.gb` failed — mode-3 sprite penalty

## Status

Resolved. The Mooneye acceptance ROM is staged at
`tests/roms/mooneye-test-suite/acceptance/ppu/intr_2_mode0_timing_sprites.gb`
(git-ignored; original source:
<https://github.com/Gekkio/mooneye-test-suite>).

The no-sprite sibling `intr_2_mode0_timing.gb` and all other wired Mooneye
ROMs also pass. Full CTest is 7/7 green.

## Reproduction

```sh
cmake --build build-frontend --parallel
./build-frontend/gb-headless \
  tests/roms/mooneye-test-suite/acceptance/ppu/intr_2_mode0_timing_sprites.gb \
  7200 --require-regs
echo rc=$?   # expect 0
```

A register probe (`FF80` = testcase id) originally showed failure at case 0
(`testcase 2, 0` — one sprite at X=0).

## Test protocol (from fetched sources)

`tests/acceptance/ppu/intr_2_mode0_timing_sprites.s` (key excerpts in
`/tmp/opencode/sprites.s`):

- `testcase <calib>, <x-coords...>`: first macro arg is calibration-only AND
  sets NOP slide lengths `d = 41+c` (round A), `e = 40+c` (round B).
  (Verified: first data bytes of the ROM are `01,00` because `.shift` drops
  the calib arg, leaving `NARGS=1, X=0`.)
- `prepare_sprites`: writes N sprites at Y=`$52`, X from args, tiles `$30+`,
  flags 0. Lines under test contain these sprites (Y=`$52` covers LY 66–73).
- `setup_and_wait_mode2`: `wait_ly $42`; `wait_mode $00`; `wait_mode $03`;
  `STAT=$20` (mode-2 IRQ only); `IF=0`; `EI`; `HALT`.
- ISR at `INTR_VEC_STAT`: `add sp,+2; ret` — discards the halt return address
  and returns into a prepared NOP slide (`d`/`e` NOPs + `RET`), which returns
  to the round code.
- Round A loop: `ld b,0; -inc b; ldh a,(STAT); and 3; jr nz,-`
  expects `B==1` (mode 0 already at first read).
  Round B (one fewer NOP): expects `B==2` (first read nonzero, second mode 0).
- Pass criterion per case with T0 = dots from mode-2 IRQ to round-A loop
  start, M0 = dots from mode-2 IRQ to mode-0 start:

  ```text
  T0 + 16 < M0 <= T0 + 20        (4-dot window)
  ```

  i.e. with `T0 = K + 4c` (K = fixed overhead) and `M0 = 252 + P`
  (P = sprite mode-3 penalty), `P` must satisfy
  `P in (K-236+4c, K-232+4c]`.
- 105 cases total: Nx0 singles, 10xX spread, split groups, 1xX singles
  (X = 0..17, 160..167), 8-apart pairs, 8-apart 10-runs + reversed
  (order-independence check), X=168/169 (fully offscreen right).

Theoretical K (all values match hardware docs AND this core's code):
`HALT(4) + IRQ dispatch(20) + ISR ADD SP,e8(16) + RET(16) + slide base(180) +
LD B,0(8) = 244`, giving windows `P in (8+4c, 12+4c]`.

## What is verified working

- Sibling `intr_2_mode0_timing.gb` (same harness, no sprites, 46/45 NOP
  slides, `LD A,(HL)` poll loop) PASSES (rc=0). ROM bytes confirm 46/45 NOPs
  and loop `04 7E E6 03 20 FA`.
- Direct line-timing measurement (debug-step sums, debug reads don't tick):
  mode2=80, mode3=172, mode0=204, line=456. Exact.
- EI delay correct (1 instruction: `ei_delay=2` set at end of EI step, then
  decremented at `done`).
- PUSH/POP/CALL/RET/ADD SP/HALT/LDH/LD (HL)/JR cycle counts verified in
  `gb_dbg_step` source.
- `timed_read`/`timed_write` tick exactly `cycles` total; end-of-step tops up
  to `c`. Blargg `mem_timing` 3/3 passes (bus accesses cycle-positioned).
- `ppu_stat` is edge-triggered and correct (stat_lyc_onoff passes).

## The paradox (measured with synthetic ROMs)

Synthetic ROM generator + probes live in `/tmp/opencode/`
(`mgen.py`, `mprobe.c`, `mtrace*.c`, `mmark.c`, `mlen.c`) — see
"Measurement rig" below. The synthetic ROM replicates the test setup
exactly (OAM sprites Y=`$52`, LCDC=`$91`, wait LY=66/mode0/mode3,
`STAT=$20`, `IF=0`, `IE=$02`, `EI`, pushes, `HALT`, same ISR, M-NOP slide +
`RET`, round-A loop storing B to `$C000`).

1. **Setup overruns the mode-3 window.** HALT is entered at LY=67 mode 0
   (mtrace3), i.e. setup takes ~680+ dots from the wait-mode-3 exit instead
   of the modeled ~152 (`STAT`wr 20 + `IF`clr 16 + `IE`wr 24 + `EI` 4 +
   pushes 56 + `HALT` 4 + exit latency ≤28). ~500 dots unaccounted for.
   Marker ROM (mmark): wait-LY exits LY=66, wait-mode0 exits LY=66,
   wait-mode3 exits LY=67 — the `wait_mode(00)`→`wait_mode(03)` pair
   inherently catches the NEXT line's mode 3, so the measurement line is
   LY=68, and setup must fit in line-67 mode 3 (172 dots). It does not.
2. **No-sprite round A reports B==1 even at slide M=20** (M=20..40 all give
   B==1). First read at K0+4·20+36 = K0+116; B==1 needs M0=252 ≤ K0+116,
   i.e. K0 ≥ 136 — impossible (K0 ≈ 56–60 by construction).
3. **Trace contradicts the result.** mtrace2 (M=20, no sprites) shows the
   slide running on LY=68 mode 3, then ~2 loop iterations sampling st=3,
   then st=0 iterations, then exit — B should be 3+, but `$C000=1`.
   B==1 after 3 `inc b` executions is impossible without B being clobbered
   (stray STAT IRQ with IE still set? LDH read clobbering B? ring-buffer
   misread?).
4. **Sibling arithmetic also fails on paper** (round B needs K<52 vs 56
   minimum) yet the sibling passes on this core. So EITHER the paper model
   of the harness path is wrong in a way that affects both ROMs, OR the
   core's behavior differs between the two paths (structural difference:
   sibling has NO pushes between EI and HALT and uses `LD A,(HL)` polling;
   sprites ROM has pushes + `LDH` polling).

## Measurement rig (`/tmp/opencode/`, outside the repo)

- `mgen.py` — builds `mtest.gb`: setup + M-NOP slide + round-A loop,
  `$C000` = B. Usage: `python3 mgen.py none|8,8,... <M>`.
  NOTE: an early version omitted the pre-HALT pushes (ISR `ret` then pops
  garbage); current version pushes round/slide addresses like the real test.
- `mprobe.c` — run 3 frames, print `$C000`, PC, LY, STAT, IF, IE, LCDC.
- `mtrace2.c` — step until `$C000≠0`, dump last 40 (PC, LY, STAT-mode).
- `mtrace3.c` — stop at setup-HALT entry, print LY/STAT-mode/IF.
- `mgen3.py`/`mmark.c` — record LY at each wait exit to `$C001..$C004`.
- `mlen.c` — mode-length measurement (80/172/204 result).
- `sprites.s`, `sib.s`, `common.s` — fetched test/`.include` sources.
- `fit*.py` — penalty-rule fitters (see below; several used WRONG windows
  due to a T0'+16 vs T0+16 slip — only fits against windows
  `(K-236+4c, K-232+4c]` are valid).

## Penalty-rule fitting status (blocked on the paradox above)

Python fits of `P(sprites)` against the 105 cases give candidate rules, but
they assume a K that contradicts the paper model, so NONE is trustworthy
until the overhead is measured:

- `first-fetched = 5+wait, rest = 6+(wait if new tile), X≥168 skipped`:
  0/105 fail at K=234–238 (WRONG K; needs halt+irq=14, impossible).
- `first = 11+wait, rest = 6+(wait if new tile), X≥168 = 1 each`:
  0/105 fail at K=240 (needs halt+irq=20, i.e. free HALT — also suspect).
- NO rule in a wide grid (bf≤17, bn≤8, wait forms, q≤3, dedup±) fits K=244
  (best: 15/105).
- wait = max(0, 5-mod), mod = (sx-8-floor((sx-8)/8)*8), tile =
  floor((sx-8)/8); X=0 IS fetched (tile −1); sort by X stable (OAM order
  breaks ties, confirmed by reversed-pair cases); X≥168 handling unknown
  (q=0 vs q=1 undecided — only 2 cases discriminate).

## Current worktree state (UNCOMMITTED — do not trust blindly)

- `src/core/gb.c` `ppu_mode3_length()`: implements `first = 11+wait`,
  `X≥168 → +1`, plus mode-0 length `456-80-ppu_mode3` for line integrity.
  The `11`/`q=1` values came from the suspect K=240 fit — LIKELY WRONG,
  revisit after the paradox is resolved.
- `src/core/gb.h`, `tests/run_mooneye.sh` (25 ROMs wired),
  `tests/test_core.c` (33 tests incl. `ppu_mode3_sprite_penalty` with spans
  252/312/364/264 — expectations depend on the rule, will need updating),
  `tools/fetch_roms.sh`.
- Previously landed (uncommitted) milestones in the same tree: TIMA
  reload-delay (state v12), OAM DMA running-CPU model, PPU STAT/OAM
  blocking + LYC freeze + LCD-enable mode-0 window (state v13), FIFO state
  v13→v14. Do NOT commit until this issue is resolved
  (user rule: commit only when explicitly asked).

## Suspects (ranked)

1. **Steps advance ≠ dots returned.** `tick`/`cpu_tick`/`instruction_cycles`
   look balanced on read, but `double_speed` truncation or a path that ticks
   without counting (or vice versa) would shift ALL setup/slide/loop timing
   while keeping line sums at 456. Verify with an internal dot counter vs
   summed returns.
2. **STAT reads see stale mode bits.** The LYC-freeze logic added for
   `stat_lyc_onoff` (`rd(FF41)` frozen-bit vs live comparison + stale-bit
   masking) may report mode 3/0 late or early inside tight poll loops.
   The trace's st=3→st=0 transition vs C000=1 smells like this.
3. **Stray STAT IRQs corrupt the measurement.** IE stays `$02` through the
   round; the NEXT line's mode-2 IRQ fires mid-loop/final-halt, ISR
   discards the stack (`add sp,+2; ret` → garbage PC — observed PC=`$0C18`
   after 3 frames) and can clobber registers/stack. The real test has the
   same exposure, but B is stored before the next line — mostly safe;
   still, rule this out by clearing IE right after the loop (synthetic) or
   tracing IF/IE/SP.
4. **LDH/`LD (HL)` read-cycle placement vs JR.** mem_timing passes, so
   unlikely — but the exact read dot within the 12/8-dot instruction is
   THE 4-dot-sensitive quantity here; re-verify `timed_read(g, addr, 12)`
   reads at dots 9–12 and the sibling's `LD A,(HL)` at dots 5–8.
5. **HALT wake path.** `irq()` clears `halted` even when IME=0 (correct:
   wake without dispatch), then execution continues at `halt+1`. With the
   EI-delay state (`ei_delay` pending across HALT in the no-push sibling
   path), verify IME lands exactly where hardware has it and no extra
   4-dot step leaks in.

## Resolution

The failure was in the scanline mode-3 length approximation, not in the
interrupt or CPU timing paths. The renderer already selected the correct
sprites, but the object-fetch penalty was modeled incorrectly for the first
sprite and for repeated fetch tiles.

`ppu_mode3_length()` now:

- sorts the selected sprites by X position while preserving OAM order for ties;
- ignores sprites at or beyond X=168;
- accounts for the fetcher's alignment wait before each new tile fetch;
- charges the first object fetch separately because it overlaps mode-3 startup;
- avoids charging the alignment wait again when consecutive sprites reuse the
  same fetch tile;
- derives mode 0 length from the complete 456-dot scanline.

The temporary FF90/FF91 diagnostics and calibration branches were removed.
The focused core timing expectations were updated for the resulting spans.

Verification:

```text
intr_2_mode0_timing_sprites.gb --require-regs: pass
Mooneye CTest: 26 ROMs pass
Full CTest: 7/7 pass
Tetris headless smoke (60 frames): pass
git diff --check: pass
```

## Historical investigation

The original investigation used temporary synthetic ROMs and probes under
`/tmp/opencode/` to distinguish CPU/IRQ timing from the PPU object-fetch
penalty. Those probes are not part of the repository. Their intermediate
calibration tables and hypotheses were discarded; only the clean sprite
penalty implementation described in the resolution above remains.
