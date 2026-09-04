#include "gb.h"
#include "input.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef GB_ENABLE_TUI
#include <ncurses.h>
#endif

static int write_file(const char *path, const uint8_t *data, size_t size) {
  FILE *file = fopen(path, "wb");
  int ok = file && fwrite(data, 1, size, file) == size;
  if (file)
    fclose(file);
  return ok ? 0 : -1;
}

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  long length;
  uint8_t *data = NULL;
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET) != 0 || (data = malloc((size_t)length)) == NULL ||
      fread(data, 1, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size = (size_t)length;
  return data;
}

static void save_ram(const gb_t *gb, const char *path) {
  size_t size = gb_save_ram_size(gb);
  uint8_t *data = size ? malloc(size) : NULL;
  if (size && data) {
    gb_save_ram(gb, data);
    write_file(path, data, size);
  }
  free(data);
}

static void save_state(const gb_t *gb, const char *path, uint8_t *buffer,
                       size_t size) {
  if (gb_save_state(gb, buffer) == size)
    write_file(path, buffer, size);
}

static void audio(void *user, const int16_t *stereo, size_t frames) {
  SDL_AudioDeviceID device = *(SDL_AudioDeviceID *)user;
  const uint32_t bytes = (uint32_t)(frames * 2 * sizeof *stereo);

  if (SDL_GetQueuedAudioSize(device) > bytes * 2)
    return;
  SDL_QueueAudio(device, stereo, bytes);
}

static void draw_button(SDL_Renderer *renderer, SDL_Rect rect, int pressed) {
  SDL_SetRenderDrawColor(renderer, pressed ? 220 : 70, pressed ? 70 : 70,
                        pressed ? 70 : 80, 190);
  SDL_RenderFillRect(renderer, &rect);
  SDL_SetRenderDrawColor(renderer, 245, 245, 245, 220);
  SDL_RenderDrawRect(renderer, &rect);
}

static void draw_controller(SDL_Renderer *renderer, uint8_t buttons) {
  SDL_Rect rect;
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  rect = (SDL_Rect){64, 624, 32, 32};
  draw_button(renderer, rect, buttons & (1 << 2));
  rect = (SDL_Rect){64, 688, 32, 32};
  draw_button(renderer, rect, buttons & (1 << 3));
  rect = (SDL_Rect){32, 656, 32, 32};
  draw_button(renderer, rect, buttons & (1 << 1));
  rect = (SDL_Rect){96, 656, 32, 32};
  draw_button(renderer, rect, buttons & (1 << 0));
  rect = (SDL_Rect){416, 656, 48, 24};
  draw_button(renderer, rect, buttons & (1 << 6));
  rect = (SDL_Rect){480, 656, 48, 24};
  draw_button(renderer, rect, buttons & (1 << 7));
  rect = (SDL_Rect){528, 608, 40, 40};
  draw_button(renderer, rect, buttons & (1 << 4));
  rect = (SDL_Rect){576, 640, 40, 40};
  draw_button(renderer, rect, buttons & (1 << 5));
}

int main(int argc, char **argv) {
  int debug = argc == 3 && strcmp(argv[1], "--debug") == 0;
  const char *path = debug ? argv[2] : argc == 2 ? argv[1] : NULL;
  char save_path[4096], state_path[4096];
  size_t rom_size, file_size;
  uint8_t *rom = NULL, *file = NULL, *state = NULL;
  gb_t *gb = NULL;
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  SDL_Texture *texture = NULL;
  SDL_AudioDeviceID audio_device = 0;
  int running = 1, paused = debug;
  unsigned save_timer = 0;
  uint8_t buttons = 0;

  if (!path) {
    fprintf(stderr, "usage: %s [--debug] ROM\n", argv[0]);
    return 2;
  }
  rom = read_file(path, &rom_size);
  if (!rom)
    return 1;
  gb = gb_create();
  if (!gb || gb_load_rom(gb, rom, rom_size)) {
    free(rom);
    gb_destroy(gb);
    return 1;
  }
  free(rom);
  if (snprintf(save_path, sizeof save_path, "%s.sav", path) >=
          (int)sizeof save_path ||
      snprintf(state_path, sizeof state_path, "%s.state", path) >=
          (int)sizeof state_path) {
    gb_destroy(gb);
    return 1;
  }

  file = read_file(save_path, &file_size);
  if (file) {
    gb_load_ram(gb, file, file_size);
    free(file);
  }
  size_t state_size = gb_save_state_size(gb);
  state = malloc(state_size);
  file = read_file(state_path, &file_size);
  if (file) {
    if (file_size == state_size)
      gb_load_state(gb, file, file_size);
    free(file);
  }
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
    free(state);
    gb_destroy(gb);
    return 1;
  }
  window = SDL_CreateWindow("Game Boy", SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED, 640, 768, 0);
  renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : NULL;
  texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STREAMING, 160, 144)
                     : NULL;
  if (!texture) {
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    free(state);
    gb_destroy(gb);
    return 1;
  }
  SDL_AudioSpec want = {0};
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  audio_device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
  if (audio_device) {
    gb_set_audio_callback(gb, audio, &audio_device);
    SDL_PauseAudioDevice(audio_device, 0);
  }
#ifdef GB_ENABLE_TUI
  if (debug) {
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
  }
#endif
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT)
        running = 0;
      if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        uint8_t button = gb_sdl_key_button(event.key.keysym.sym);
        if (button) {
          if (event.type == SDL_KEYDOWN)
            buttons |= button;
          else
            buttons &= (uint8_t)~button;
          gb_set_input(gb, buttons);
        }
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F5)
        save_state(gb, state_path, state, state_size);
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F8) {
        file = read_file(state_path, &file_size);
        if (file) {
          if (file_size == state_size)
            gb_load_state(gb, file, file_size);
          free(file);
        }
      }
    }
#ifdef GB_ENABLE_TUI
    if (debug) {
      int key = getch();
      if (key == 'q')
        running = 0;
      if (key == 'c')
        paused = 0;
      if (key == 's')
        paused = 1;
      gb_regs_t regs;
      gb_dbg_regs(gb, &regs);
      erase();
      mvprintw(0, 0, "Game Boy debugger  [s]tep [c]ontinue [q]uit  %s",
               paused ? "paused" : "running");
      mvprintw(2, 0, "AF %04X  BC %04X  DE %04X  HL %04X  SP %04X  PC %04X",
               regs.af, regs.bc, regs.de, regs.hl, regs.sp, regs.pc);
      refresh();
    }
#endif
    if (!paused)
      gb_run_frame(gb);
    if (++save_timer == 300) {
      save_ram(gb, save_path);
      save_timer = 0;
    }
    SDL_UpdateTexture(texture, NULL, gb_framebuffer(gb), 160 * 4);
    SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
    SDL_RenderClear(renderer);
    SDL_Rect game_rect = {0, 0, 640, 576};
    SDL_RenderCopy(renderer, texture, NULL, &game_rect);
    draw_controller(renderer, buttons);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
#ifdef GB_ENABLE_TUI
  if (debug)
    endwin();
#endif
  save_ram(gb, save_path);
  if (audio_device)
    SDL_CloseAudioDevice(audio_device);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  free(state);
  gb_destroy(gb);
  return 0;
}
