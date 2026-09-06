#include "gb.h"
#include <ctype.h>
#include <ncurses.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_ROWS 8
#define DISASM_ROWS 10
#define MAX_BPS 16

typedef struct {
  gb_bp_kind_t kind;
  uint16_t addr;
  int id;
} bp_entry_t;

static int hex_nibble(int key) {
  if (key >= '0' && key <= '9')
    return key - '0';
  key = tolower(key);
  if (key >= 'a' && key <= 'f')
    return key - 'a' + 10;
  return -1;
}

/* Prompt for hex digits at the bottom row; returns -1 on cancel. */
static long prompt_hex(const char *label, int digits) {
  char buf[8] = {0};
  int len = 0, key;
  echo();
  curs_set(1);
  nodelay(stdscr, FALSE);
  while (len < digits) {
    mvprintw(LINES - 1, 0, "%s [%s]", label, buf);
    clrtoeol();
    refresh();
    key = getch();
    if (key == 27 || key == 'q') {
      len = -1;
      break;
    }
    if ((key == KEY_BACKSPACE || key == 127) && len > 0) {
      buf[--len] = '\0';
      continue;
    }
    if (hex_nibble(key) >= 0 && len < digits)
      buf[len++] = (char)key;
  }
  noecho();
  curs_set(0);
  nodelay(stdscr, TRUE);
  if (len < 0)
    return -1;
  return strtol(buf, NULL, 16);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s ROM\n", argv[0]);
    return 2;
  }
  FILE *file = fopen(argv[1], "rb");
  if (!file)
    return 1;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  rewind(file);
  uint8_t *rom = malloc((size_t)size);
  if (!rom || fread(rom, 1, (size_t)size, file) != (size_t)size)
    return 1;
  fclose(file);
  gb_t *gb = gb_create();
  if (gb_load_rom(gb, rom, (size_t)size))
    return 1;
  free(rom);
  initscr();
  cbreak();
  noecho();
  nodelay(stdscr, TRUE);
  keypad(stdscr, TRUE);
  curs_set(0);

  gb_regs_t regs;
  char disasm[64];
  int running = 1, paused = 1, hit = -1;
  uint16_t mem_base = 0xc000, mem_cursor = 0xc000;
  bp_entry_t bps[MAX_BPS];
  int bp_count = 0;

  while (running) {
    gb_dbg_regs(gb, &regs);
    erase();
    mvprintw(0, 0, "Game Boy TUI  s:step c:continue f:frame b:pc-break w:watch B:clear-bp q:quit  [%s]",
             paused ? "paused" : "running");
    mvprintw(2, 0, "AF %04X  BC %04X  DE %04X  HL %04X  SP %04X  PC %04X",
             regs.af, regs.bc, regs.de, regs.hl, regs.sp, regs.pc);
    mvprintw(3, 0, "Z %c  N %c  H %c  C %c  IME %d  HALT %d%s",
             regs.af & 0x80 ? '*' : '.', regs.af & 0x40 ? '*' : '.',
             regs.af & 0x20 ? '*' : '.', regs.af & 0x10 ? '*' : '.',
             regs.ime, regs.halted, hit >= 0 ? "  [break]" : "");
    mvprintw(5, 0, "-- OPS --");
    {
      uint16_t addr = regs.pc;
      for (int i = 0; i < DISASM_ROWS; i++) {
        int length = gb_dbg_disasm(gb, addr, disasm, sizeof disasm);
        mvprintw(6 + i, 0, "%c%04X  %s", addr == regs.pc ? '>' : ' ', addr,
                 disasm);
        addr = (uint16_t)(addr + (length > 0 ? length : 1));
      }
    }
    mvprintw(5, 32, "-- RAM (arrows move, g goto, e edit) --");
    for (int row = 0; row < MEM_ROWS; row++) {
      uint16_t row_addr = (uint16_t)(mem_base + row * 16);
      mvprintw(6 + row, 32, "%04X", row_addr);
      for (int col = 0; col < 16; col++) {
        uint16_t addr = (uint16_t)(row_addr + col);
        if (addr == mem_cursor)
          attron(A_REVERSE);
        printw(" %02X", gb_dbg_read(gb, addr));
        if (addr == mem_cursor)
          attroff(A_REVERSE);
      }
    }
    mvprintw(6 + MEM_ROWS + 1, 32, "-- BREAKPOINTS (%d) --", bp_count);
    for (int i = 0; i < bp_count && i < MEM_ROWS; i++)
      mvprintw(6 + MEM_ROWS + 2 + i, 32, "%s $%04X",
               bps[i].kind == GB_BP_PC ? "PC" : bps[i].kind == GB_BP_READ ? "R" : "W",
               bps[i].addr);
    refresh();

    int key = getch();
    if (key == ERR) {
      if (!paused) {
        gb_run_frame(gb);
        napms(16);
      }
      continue;
    }
    hit = -1;
    if (key == 'q') {
      running = 0;
    } else if (key == 's') {
      gb_dbg_step(gb);
      paused = 1;
    } else if (key == 'f') {
      gb_run_frame(gb);
    } else if (key == 'c') {
      gb_dbg_enable(gb, true);
      hit = gb_dbg_run_until_break(gb);
      paused = 1;
    } else if (key == 'b') {
      int slot = -1;
      for (int i = 0; i < bp_count; i++)
        if (bps[i].kind == GB_BP_PC && bps[i].addr == regs.pc)
          slot = i;
      if (slot >= 0) {
        gb_dbg_del_bp(gb, bps[slot].id);
        bps[slot] = bps[--bp_count];
      } else if (bp_count < MAX_BPS) {
        int id = gb_dbg_add_bp(gb, (gb_bp_t){GB_BP_PC, regs.pc});
        if (id >= 0)
          bps[bp_count++] = (bp_entry_t){GB_BP_PC, regs.pc, id};
      }
    } else if (key == 'w') {
      if (bp_count < MAX_BPS) {
        int id = gb_dbg_add_bp(gb, (gb_bp_t){GB_BP_WRITE, mem_cursor});
        if (id >= 0)
          bps[bp_count++] = (bp_entry_t){GB_BP_WRITE, mem_cursor, id};
      }
    } else if (key == 'B') {
      for (int i = 0; i < bp_count; i++)
        gb_dbg_del_bp(gb, bps[i].id);
      bp_count = 0;
    } else if (key == KEY_UP) {
      mem_cursor = (uint16_t)(mem_cursor - 16);
      if (mem_cursor < mem_base || mem_cursor >= mem_base + MEM_ROWS * 16)
        mem_base = (uint16_t)(mem_cursor & 0xfff0u);
    } else if (key == KEY_DOWN) {
      mem_cursor = (uint16_t)(mem_cursor + 16);
      if (mem_cursor < mem_base || mem_cursor >= mem_base + MEM_ROWS * 16)
        mem_base = (uint16_t)(mem_cursor & 0xfff0u);
    } else if (key == KEY_LEFT) {
      mem_cursor = (uint16_t)(mem_cursor - 1);
      if (mem_cursor < mem_base)
        mem_base = (uint16_t)(mem_cursor & 0xfff0u);
    } else if (key == KEY_RIGHT) {
      mem_cursor = (uint16_t)(mem_cursor + 1);
      if (mem_cursor >= mem_base + MEM_ROWS * 16)
        mem_base = (uint16_t)(mem_cursor & 0xfff0u);
    } else if (key == 'g') {
      long addr = prompt_hex("goto address (4 hex digits)", 4);
      if (addr >= 0) {
        mem_cursor = (uint16_t)addr;
        mem_base = (uint16_t)(mem_cursor & 0xfff0u);
      }
    } else if (key == 'e') {
      long value = prompt_hex("new byte value (2 hex digits)", 2);
      if (value >= 0)
        gb_dbg_write(gb, mem_cursor, (uint8_t)value);
    } else if (key == ' ') {
      paused = !paused;
    }
  }
  endwin();
  gb_destroy(gb);
  return 0;
}
