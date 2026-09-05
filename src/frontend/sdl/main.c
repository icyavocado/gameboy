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

typedef struct {
  SDL_AudioDeviceID device;
  unsigned volume;
} audio_context_t;

static void audio(void *user, const int16_t *stereo, size_t frames) {
  audio_context_t *context = user;
  const uint32_t bytes = (uint32_t)(frames * 2 * sizeof *stereo);
  int16_t *scaled = NULL;

  if (SDL_GetQueuedAudioSize(context->device) > bytes * 2)
    return;
  if (context->volume == 100) {
    SDL_QueueAudio(context->device, stereo, bytes);
    return;
  }
  scaled = malloc(frames * 2 * sizeof *scaled);
  if (!scaled)
    return;
  for (size_t i = 0; i < frames * 2; i++)
    scaled[i] = (int16_t)((int)stereo[i] * (int)context->volume / 100);
  SDL_QueueAudio(context->device, scaled, bytes);
  free(scaled);
}

static const uint8_t font[128][7] = {
    ['A'] = {14, 17, 17, 31, 17, 17, 17}, ['B'] = {30, 17, 17, 30, 17, 17, 30},
    ['C'] = {14, 17, 16, 16, 16, 17, 14}, ['D'] = {30, 17, 17, 17, 17, 17, 30},
    ['E'] = {31, 16, 16, 30, 16, 16, 31}, ['F'] = {31, 16, 16, 30, 16, 16, 16},
    ['G'] = {14, 17, 16, 23, 17, 17, 14}, ['H'] = {17, 17, 17, 31, 17, 17, 17},
    ['I'] = {31, 4, 4, 4, 4, 4, 31}, ['L'] = {16, 16, 16, 16, 16, 16, 31},
    ['M'] = {17, 27, 21, 21, 17, 17, 17}, ['N'] = {17, 25, 21, 19, 17, 17, 17},
    ['O'] = {14, 17, 17, 17, 17, 17, 14}, ['P'] = {30, 17, 17, 30, 16, 16, 16},
    ['R'] = {30, 17, 17, 30, 20, 18, 17}, ['S'] = {15, 16, 16, 14, 1, 1, 30},
    ['T'] = {31, 4, 4, 4, 4, 4, 4}, ['U'] = {17, 17, 17, 17, 17, 17, 14},
    ['V'] = {17, 17, 17, 17, 17, 10, 4}, ['Y'] = {17, 17, 10, 4, 4, 4, 4},
    ['0'] = {14, 17, 19, 21, 25, 17, 14}, ['1'] = {4, 12, 4, 4, 4, 4, 14},
    ['2'] = {14, 17, 1, 2, 4, 8, 31}, ['3'] = {30, 1, 1, 14, 1, 1, 30},
    ['4'] = {2, 6, 10, 18, 31, 2, 2}, ['5'] = {31, 16, 16, 30, 1, 1, 30},
    ['6'] = {14, 16, 16, 30, 17, 17, 14}, ['7'] = {31, 1, 2, 4, 8, 8, 8},
    ['8'] = {14, 17, 17, 14, 17, 17, 14}, ['9'] = {14, 17, 17, 15, 1, 1, 14},
    [' '] = {0, 0, 0, 0, 0, 0, 0}, ['-'] = {0, 0, 0, 31, 0, 0, 0},
    [':'] = {0, 4, 0, 0, 4, 0, 0}, ['/'] = {1, 2, 4, 8, 16, 0, 0}
};

static void draw_text(SDL_Renderer *renderer, const char *text, int x, int y,
                      int scale, SDL_Color color) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  for (; *text; text++, x += 6 * scale) {
    unsigned char c = (unsigned char)*text;
    for (int row = 0; row < 7; row++)
      for (int bit = 0; bit < 5; bit++)
        if (font[c][row] & (1u << (4 - bit))) {
          SDL_Rect pixel = {x + bit * scale, y + row * scale, scale, scale};
          SDL_RenderFillRect(renderer, &pixel);
        }
  }
}

static uint8_t mapped_button(SDL_Keycode key, const SDL_Keycode keys[8]) {
  for (unsigned i = 0; i < 8; i++)
    if (keys[i] == key)
      return (uint8_t)(1u << i);
  return 0;
}

typedef struct {
  int open;
  int selected;
  int remapping;
  int palette;
  char path[256];
  size_t path_length;
} settings_t;

static const char *setting_names[] = {
    "LOAD ROM", "SAVE", "LOAD SAVE", "SAVE STATE", "LOAD STATE",
    "VOLUME", "CHANGE PALETTE", "REMAP KEYS", "RESET", "CLOSE"};

static void draw_settings(SDL_Renderer *renderer, const settings_t *settings,
                          unsigned volume, const SDL_Keycode keys[8]) {
  SDL_Rect panel = {70, 45, 500, 485};
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 8, 12, 20, 245);
  SDL_RenderFillRect(renderer, &panel);
  SDL_SetRenderDrawColor(renderer, 110, 210, 190, 255);
  SDL_RenderDrawRect(renderer, &panel);
  draw_text(renderer, "SETTINGS", 105, 70, 4, (SDL_Color){220, 250, 235, 255});
  for (unsigned i = 0; i < sizeof setting_names / sizeof *setting_names; i++) {
    int y = 125 + (int)i * 32;
    SDL_Color color = i == (unsigned)settings->selected
                          ? (SDL_Color){20, 80, 70, 255}
                          : (SDL_Color){18, 28, 40, 255};
    SDL_Rect row = {100, y - 5, 440, 27};
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &row);
    draw_text(renderer, setting_names[i], 112, y, 2,
              (SDL_Color){235, 245, 240, 255});
    if (i == 5) {
      char value[16];
      snprintf(value, sizeof value, "%u%%", volume);
      draw_text(renderer, value, 455, y, 2, (SDL_Color){180, 230, 210, 255});
    } else if (i == 6) {
      char value[16];
      snprintf(value, sizeof value, "%d", settings->palette + 1);
      draw_text(renderer, value, 500, y, 2, (SDL_Color){180, 230, 210, 255});
    }
  }
  if (settings->remapping >= 0) {
    SDL_Rect modal = {120, 210, 400, 100};
    SDL_SetRenderDrawColor(renderer, 20, 25, 35, 255);
    SDL_RenderFillRect(renderer, &modal);
    char prompt[32];
    snprintf(prompt, sizeof prompt, "KEY %d OF 8", settings->remapping + 1);
    draw_text(renderer, prompt, 185, 240, 3,
              (SDL_Color){255, 240, 180, 255});
  } else if (settings->path_length) {
    draw_text(renderer, settings->path, 112, 475, 2,
              (SDL_Color){180, 230, 210, 255});
  }
  if (settings->remapping < 0 && settings->selected == 0)
    draw_text(renderer, "TYPE PATH THEN ENTER", 112, 475, 2,
              (SDL_Color){180, 230, 210, 255});
  (void)keys;
}

static void draw_boot_logo(SDL_Renderer *renderer, int y) {
  static const uint8_t logo[48] = {
      0xce, 0xed, 0x66, 0x66, 0xcc, 0x0d, 0x00, 0x0b,
      0x03, 0x73, 0x00, 0x83, 0x00, 0x0c, 0x00, 0x0d,
      0x00, 0x08, 0x11, 0x1f, 0x88, 0x89, 0x00, 0x0e,
      0xdc, 0xcc, 0x6e, 0xe6, 0xdd, 0xdd, 0xd9, 0x99,
      0xbb, 0xbb, 0x67, 0x63, 0x6e, 0x0e, 0xec, 0xcc,
      0xdd, 0xdc, 0x99, 0x9f, 0xbb, 0xb9, 0x33, 0x3e,
  };
  uint8_t tile_data[24][8];
  for (unsigned i = 0; i < sizeof logo; i++) {
    for (unsigned nibble = 0; nibble < 2; nibble++) {
      unsigned value = nibble ? logo[i] & 0x0f : logo[i] >> 4;
      uint8_t expanded = 0;
      for (unsigned bit = 0; bit < 4; bit++)
        if (value & (1u << (3 - bit)))
          expanded |= (uint8_t)(3u << (6 - bit * 2));
      unsigned tile = i / 2;
      unsigned row = (i % 2) * 4 + nibble * 2;
      tile_data[tile][row] = expanded;
      tile_data[tile][row + 1] = expanded;
    }
  }
  const int scale = 4;
  const int width = 12 * 8 * scale;
  const int left = (640 - width) / 2;

  SDL_SetRenderDrawColor(renderer, 224, 248, 208, 255);
  SDL_RenderClear(renderer);
  SDL_SetRenderDrawColor(renderer, 15, 56, 15, 255);
  for (unsigned map_row = 0; map_row < 2; map_row++) {
    for (unsigned map_column = 0; map_column < 12; map_column++) {
      unsigned tile = map_row * 12 + map_column;
      for (unsigned row = 0; row < 8; row++) {
        for (unsigned bit = 0; bit < 8; bit++) {
          if (tile_data[tile][row] & (uint8_t)(1u << (7 - bit))) {
            SDL_Rect pixel = {left + (int)(map_column * 8 + bit) * scale,
                              y + (int)(map_row * 8 + row) * scale, scale, scale};
            SDL_RenderFillRect(renderer, &pixel);
          }
        }
      }
    }
  }
  SDL_RenderPresent(renderer);
}

static void boot_chime(SDL_AudioDeviceID device) {
  enum { rate = 48000, frames = 14400 };
  int16_t *samples = malloc(frames * 2 * sizeof *samples);
  if (!samples)
    return;
  uint32_t phase = 0;
  for (unsigned i = 0; i < frames; i++) {
    unsigned frequency = i < 7200 ? 1049 : 2080;
    unsigned amplitude = i < 2400 ? i * 180 / 2400
                                   : i > 12000 ? (frames - i) * 180 / 2400 : 180;
    phase += (uint32_t)(((uint64_t)frequency << 32) / rate);
    int value = (phase & 0x80000000u) ? (int)amplitude : -(int)amplitude;
    samples[i * 2] = (int16_t)value;
    samples[i * 2 + 1] = (int16_t)value;
  }
  if (device)
    SDL_QueueAudio(device, samples, frames * 2 * sizeof *samples);
  free(samples);
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
  int debug = 0;
  char path[4096] = "";
  const char *boot_path = NULL;
  char save_path[4096], state_path[4096];
  size_t rom_size, file_size, boot_size = 0;
  uint8_t *rom = NULL, *file = NULL, *state = NULL;
  uint8_t *boot = NULL;
  gb_t *gb = NULL;
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  SDL_Texture *texture = NULL;
  SDL_AudioDeviceID audio_device = 0;
  audio_context_t audio_context = {0, 100};
  settings_t settings = {0, 0, -1, 0, "", 0};
  SDL_Keycode keys[8] = {SDLK_RIGHT, SDLK_LEFT, SDLK_UP, SDLK_DOWN,
                         SDLK_z, SDLK_x, SDLK_LSHIFT, SDLK_RETURN};
  int running = 1, paused = debug;
  unsigned save_timer = 0;
  uint8_t buttons = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--debug") == 0) {
      debug = 1;
    } else if (strcmp(argv[i], "--boot-rom") == 0 && i + 1 < argc) {
      boot_path = argv[++i];
    } else if (!path[0]) {
      if (snprintf(path, sizeof path, "%s", argv[i]) >= (int)sizeof path)
        path[0] = '\0';
    } else {
      path[0] = '\0';
      break;
    }
  }
  if (!path[0]) {
    fprintf(stderr, "usage: %s [--debug] [--boot-rom FILE] ROM\n", argv[0]);
    return 2;
  }
  if (boot_path) {
    boot = read_file(boot_path, &boot_size);
    if (!boot) {
      fprintf(stderr, "cannot read boot ROM: %s\n", boot_path);
      return 1;
    }
    if (boot_size != 0x100 && boot_size != 0x900) {
      fprintf(stderr, "boot ROM must be 256 or 2304 bytes\n");
      free(boot);
      return 1;
    }
  }
  rom = read_file(path, &rom_size);
  if (!rom) {
    free(boot);
    return 1;
  }
  gb = gb_create();
  if (!gb || (boot && gb_load_boot_rom(gb, boot, boot_size)) ||
      gb_load_rom(gb, rom, rom_size)) {
    free(boot);
    free(rom);
    gb_destroy(gb);
    return 1;
  }
  free(boot);
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
    audio_context.device = audio_device;
    gb_set_audio_callback(gb, audio, &audio_context);
    SDL_PauseAudioDevice(audio_device, 0);
  }
  if (gb_rom_logo_valid(gb)) {
    Uint32 boot_start = SDL_GetTicks();
    while (running && SDL_GetTicks() - boot_start < 5000) {
      SDL_Event event;
      while (SDL_PollEvent(&event))
        if (event.type == SDL_QUIT)
          running = 0;
      Uint32 elapsed = SDL_GetTicks() - boot_start;
      int y = -32 + (int)((elapsed < 1500 ? elapsed * 304 / 1500 : 304));
      draw_boot_logo(renderer, y);
      SDL_Delay(16);
    }
    if (running)
      boot_chime(audio_device);
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
      if (settings.open) {
        if (event.type == SDL_TEXTINPUT && settings.selected == 0 &&
            settings.path_length + strlen(event.text.text) < sizeof settings.path) {
          strcpy(settings.path + settings.path_length, event.text.text);
          settings.path_length += strlen(event.text.text);
        }
        if (event.type == SDL_KEYDOWN) {
          SDL_Keycode key = event.key.keysym.sym;
          if (settings.remapping >= 0 && key != SDLK_ESCAPE) {
            keys[settings.remapping] = key;
            settings.remapping++;
            if (settings.remapping == 8)
              settings.remapping = -1;
          } else if (key == SDLK_ESCAPE) {
            settings.open = 0;
            settings.remapping = -1;
            SDL_StopTextInput();
          } else if (key == SDLK_UP && settings.selected > 0) {
            settings.selected--;
          } else if (key == SDLK_DOWN && settings.selected < 9) {
            settings.selected++;
          } else if (key == SDLK_LEFT && settings.selected == 5 && audio_context.volume >= 10) {
            audio_context.volume -= 10;
          } else if (key == SDLK_RIGHT && settings.selected == 5 && audio_context.volume <= 90) {
            audio_context.volume += 10;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 0) {
            size_t new_size;
            uint8_t *new_rom = read_file(settings.path, &new_size);
            if (new_rom && gb_load_rom(gb, new_rom, new_size) == 0) {
              strncpy(path, settings.path, sizeof save_path - 1);
              path[sizeof save_path - 1] = '\0';
              if (snprintf(save_path, sizeof save_path, "%s.sav", path) >=
                      (int)sizeof save_path ||
                  snprintf(state_path, sizeof state_path, "%s.state", path) >=
                      (int)sizeof state_path) {
                free(new_rom);
                settings.path_length = 0;
                settings.path[0] = '\0';
                continue;
              }
              free(state);
              state_size = gb_save_state_size(gb);
              state = malloc(state_size);
            }
            free(new_rom);
            settings.path_length = 0;
            settings.path[0] = '\0';
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 1) {
            save_ram(gb, save_path);
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 2) {
            file = read_file(save_path, &file_size);
            if (file) { gb_load_ram(gb, file, file_size); free(file); }
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 3) {
            save_state(gb, state_path, state, state_size);
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 4) {
            file = read_file(state_path, &file_size);
            if (file) { if (file_size == state_size) gb_load_state(gb, file, file_size); free(file); }
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 6) {
            settings.palette = (settings.palette + 1) % 3;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 7) {
            settings.remapping = 0;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 8) {
            gb_reset(gb);
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 9) {
            settings.open = 0;
            SDL_StopTextInput();
          } else if (key == SDLK_BACKSPACE && settings.selected == 0 && settings.path_length) {
            settings.path[--settings.path_length] = '\0';
          }
        }
        continue;
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
        settings.open = 1;
        settings.selected = 0;
        settings.path_length = 0;
        settings.path[0] = '\0';
        SDL_StartTextInput();
        continue;
      }
      if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        uint8_t button = mapped_button(event.key.keysym.sym, keys);
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
    if (settings.palette == 1)
      SDL_SetTextureColorMod(texture, 190, 235, 190);
    else if (settings.palette == 2)
      SDL_SetTextureColorMod(texture, 235, 210, 160);
    else
      SDL_SetTextureColorMod(texture, 255, 255, 255);
    SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
    SDL_RenderClear(renderer);
    SDL_Rect game_rect = {0, 0, 640, 576};
    SDL_RenderCopy(renderer, texture, NULL, &game_rect);
    draw_controller(renderer, buttons);
    if (settings.open)
      draw_settings(renderer, &settings, audio_context.volume, keys);
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
