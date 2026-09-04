#include "gb.h"
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "usage: %s ROM\n", argv[0]); return 2; }
  FILE *file = fopen(argv[1], "rb");
  if (!file) return 1;
  fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
  uint8_t *rom = malloc((size_t)size);
  if (!rom || fread(rom, 1, (size_t)size, file) != (size_t)size) return 1;
  fclose(file);
  gb_t *gb = gb_create();
  if (gb_load_rom(gb, rom, (size_t)size)) return 1;
  free(rom);
  initscr(); cbreak(); noecho(); keypad(stdscr, TRUE);
  gb_regs_t regs; int running = 1;
  while (running) {
    gb_dbg_regs(gb, &regs); erase();
    mvprintw(0, 0, "Game Boy TUI  s:step  f:frame  q:quit");
    mvprintw(2, 0, "AF %04X  BC %04X  DE %04X  HL %04X  SP %04X  PC %04X", regs.af, regs.bc, regs.de, regs.hl, regs.sp, regs.pc);
    refresh(); int key = getch();
    if (key == 'q') running = 0; else if (key == 's') gb_dbg_step(gb); else if (key == 'f') gb_run_frame(gb);
  }
  endwin(); gb_destroy(gb); return 0;
}
