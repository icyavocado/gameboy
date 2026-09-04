#include "gb.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef GB_ENABLE_TUI
#include <ncurses.h>
#endif
int main(int argc, char **argv) {
  int debug = argc == 3 && strcmp(argv[1], "--debug") == 0;
  const char *path = debug ? argv[2] : argc == 2 ? argv[1] : NULL;
  if (!path) { fprintf(stderr, "usage: %s [--debug] ROM\n", argv[0]); return 2; }
  FILE *f=fopen(path,"rb"); if(!f)return 1; fseek(f,0,SEEK_END); long n=ftell(f);rewind(f); uint8_t *rom=malloc((size_t)n); fread(rom,1,(size_t)n,f);fclose(f);
  gb_t *g=gb_create(); if(gb_load_rom(g,rom,(size_t)n)){free(rom);return 1;} free(rom); if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS))return 1;
  SDL_Window *w=SDL_CreateWindow("Game Boy",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,640,576,0); SDL_Renderer *r=SDL_CreateRenderer(w,-1,SDL_RENDERER_ACCELERATED); SDL_Texture *t=SDL_CreateTexture(r,SDL_PIXELFORMAT_ABGR8888,SDL_TEXTUREACCESS_STREAMING,160,144); int run=1;
#ifdef GB_ENABLE_TUI
  if (debug) { initscr(); cbreak(); noecho(); nodelay(stdscr, TRUE); keypad(stdscr, TRUE); }
#endif
  int paused = debug;
  while(run){
    SDL_Event e; while(SDL_PollEvent(&e)) if(e.type==SDL_QUIT)run=0;
#ifdef GB_ENABLE_TUI
    if (debug) { int c=getch(); if(c=='q')run=0; if(c=='c')paused=0; if(c=='s')paused=1; gb_regs_t regs; gb_dbg_regs(g,&regs); erase(); mvprintw(0,0,"Game Boy debugger  [s]tep [c]ontinue [q]uit  %s",paused?"paused":"running"); mvprintw(2,0,"AF %04X  BC %04X  DE %04X  HL %04X  SP %04X  PC %04X",regs.af,regs.bc,regs.de,regs.hl,regs.sp,regs.pc); refresh(); }
#endif
    if (!paused) gb_run_frame(g);
    SDL_UpdateTexture(t,NULL,gb_framebuffer(g),160*4); SDL_RenderClear(r); SDL_RenderCopy(r,t,NULL,NULL); SDL_RenderPresent(r); SDL_Delay(1);
  }
#ifdef GB_ENABLE_TUI
  if (debug) endwin();
#endif
  SDL_DestroyTexture(t);SDL_DestroyRenderer(r);SDL_DestroyWindow(w);SDL_Quit();gb_destroy(g);return 0;
}
