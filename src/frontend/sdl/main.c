#include "gb.h"
#include "input.h"
#include "ui_assets.h"
#include <SDL.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#ifdef GB_ENABLE_TUI
#include <ncurses.h>
#endif

static gb_ui_assets_t ui_assets;

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

static int state_slot_path(const char *base, unsigned slot, char *path,
                           size_t size) {
  return snprintf(path, size, "%s%u", base, slot + 1) < (int)size ? 0 : -1;
}

static int state_slot_exists(const char *base, unsigned slot) {
  char path[4096];
  struct stat info;
  return state_slot_path(base, slot, path, sizeof path) == 0 &&
         stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static int save_slot_path(const char *base, unsigned slot, char *path,
                          size_t size) {
  return snprintf(path, size, "%s%u", base, slot + 1) < (int)size ? 0 : -1;
}

static int save_slot_exists(const char *base, unsigned slot) {
  char path[4096];
  struct stat info;
  return save_slot_path(base, slot, path, sizeof path) == 0 &&
         stat(path, &info) == 0 && S_ISREG(info.st_mode);
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

static void draw_text(SDL_Renderer *renderer, const char *text, int x, int y,
                      int scale, SDL_Color color) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  for (; *text; text++, x += 6 * scale) {
    unsigned char c = (unsigned char)*text;
    if (c >= 'a' && c <= 'z')
      c = (unsigned char)(c - 'a' + 'A');
    for (int row = 0; row < 7; row++)
      for (int bit = 0; bit < 5; bit++)
        if (ui_assets.font[c][row] & (1u << (4 - bit))) {
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
  char *path;
  char title[17];
} rom_entry_t;

static int rom_entry_compare(const void *a, const void *b) {
  const rom_entry_t *left = a;
  const rom_entry_t *right = b;
  return strcasecmp(left->title, right->title);
}

static int contains_test(const char *text) {
  for (; *text; text++)
    if (strncasecmp(text, "test", 4) == 0)
      return 1;
  return 0;
}

static void find_roms(const char *root, rom_entry_t **roms, size_t *count,
                      int debug) {
  DIR *directory = opendir(root);
  struct dirent *entry;
  if (!directory)
    return;
  while ((entry = readdir(directory))) {
    char path[4096];
    struct stat info;
    size_t length;
    if (entry->d_name[0] == '.' ||
        snprintf(path, sizeof path, "%s/%s", root, entry->d_name) >=
            (int)sizeof path ||
        stat(path, &info) != 0)
      continue;
    if (S_ISDIR(info.st_mode)) {
      find_roms(path, roms, count, debug);
      continue;
    }
    length = strlen(entry->d_name);
    if (!S_ISREG(info.st_mode) || length < 3 ||
        strcasecmp(entry->d_name + length - 3, ".gb") != 0)
      continue;
    if (!debug && contains_test(entry->d_name))
      continue;
    {
      uint8_t header[0x150];
      size_t header_size;
      uint8_t *data = read_file(path, &header_size);
      rom_entry_t *grown;
      if (!data || header_size < sizeof header) {
        free(data);
        continue;
      }
      memcpy(header, data, sizeof header);
      free(data);
      grown = realloc(*roms, (*count + 1) * sizeof **roms);
      if (grown) {
        *roms = grown;
        (*roms)[*count].path = malloc(strlen(path) + 1);
        if (!(*roms)[*count].path)
          continue;
        strcpy((*roms)[*count].path, path);
        memcpy((*roms)[*count].title, header + 0x134, 16);
        (*roms)[*count].title[16] = '\0';
        for (int i = 15; i >= 0 &&
                        ((*roms)[*count].title[i] == ' ' ||
                         (*roms)[*count].title[i] == '\0');
             i--)
          (*roms)[*count].title[i] = '\0';
        for (char *p = (*roms)[*count].title; *p; p++)
          if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e)
            *p = '?';
        if (!(*roms)[*count].title[0])
          snprintf((*roms)[*count].title, sizeof (*roms)[*count].title,
                   "UNTITLED");
        if (!debug && contains_test((*roms)[*count].title)) {
          free((*roms)[*count].path);
          continue;
        }
        (*count)++;
      }
    }
  }
  closedir(directory);
}

static void discover_roms(rom_entry_t **roms, size_t *count, int debug) {
  const char *root = "tests/roms";
  struct stat info;
  if (stat(root, &info) != 0 || !S_ISDIR(info.st_mode))
    root = "roms";
  find_roms(root, roms, count, debug);
  qsort(*roms, *count, sizeof **roms, rom_entry_compare);
}

static void free_roms(rom_entry_t *roms, size_t count) {
  for (size_t i = 0; i < count; i++)
    free(roms[i].path);
  free(roms);
}

typedef struct {
  int open;
  int selected;
  int remapping;
  int palette;
  int browser;
  int confirm_load;
  unsigned state_slot;
  rom_entry_t *roms;
  size_t rom_count;
  size_t rom_selected;
} settings_t;

static void open_settings(settings_t *settings, int debug) {
  settings->open = 1;
  settings->selected = 0;
  settings->browser = 0;
  settings->confirm_load = 0;
  free_roms(settings->roms, settings->rom_count);
  settings->roms = NULL;
  settings->rom_count = 0;
  settings->rom_selected = 0;
  discover_roms(&settings->roms, &settings->rom_count, debug);
}

static const char *setting_names[] = {
    "LOAD ROM", "SAVE", "LOAD SAVE", "SAVE STATE", "LOAD STATE",
    "VOLUME", "CHANGE PALETTE", "REMAP KEYS", "RESET", "CLOSE"};

static void draw_settings(SDL_Renderer *renderer, const settings_t *settings,
                          unsigned volume, const char *save_path,
                          const char *state_path,
                          const SDL_Keycode keys[8]) {
  SDL_Rect panel = {70, 45, 500, 485};
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 8, 12, 20, 245);
  SDL_RenderFillRect(renderer, &panel);
  SDL_SetRenderDrawColor(renderer, 110, 210, 190, 255);
  SDL_RenderDrawRect(renderer, &panel);
  draw_text(renderer, "SETTINGS", 105, 70, 4,
            (SDL_Color){220, 250, 235, 255});
  if (settings->browser) {
    draw_text(renderer, "> LOAD ROM", 330, 78, 2,
              (SDL_Color){180, 230, 210, 255});
    if (!settings->rom_count) {
      draw_text(renderer, "NO ROMS FOUND", 120, 210, 3,
                (SDL_Color){255, 180, 160, 255});
    } else {
      size_t first = settings->rom_selected > 7 ? settings->rom_selected - 7 : 0;
      size_t last = first + 10;
      if (last > settings->rom_count)
        last = settings->rom_count;
      for (size_t i = first; i < last; i++) {
        char display[70];
        int y = 125 + (int)(i - first) * 30;
        snprintf(display, sizeof display, "%s", settings->roms[i].title);
        SDL_SetRenderDrawColor(renderer,
                               i == settings->rom_selected ? 20 : 10,
                               i == settings->rom_selected ? 80 : 20,
                               i == settings->rom_selected ? 70 : 30, 255);
        SDL_Rect row = {100, y - 5, 440, 25};
        SDL_RenderFillRect(renderer, &row);
        draw_text(renderer, display, 110, y, 2,
                  (SDL_Color){235, 245, 240, 255});
        if (settings->confirm_load && i == settings->rom_selected)
          draw_text(renderer, "ENTER | ESC", 405, y, 2,
                    (SDL_Color){255, 240, 180, 255});
      }
    }
    draw_text(renderer, "ENTER LOAD  ESC BACK", 145, 475, 2,
              (SDL_Color){180, 230, 210, 255});
    return;
  }
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
      for (unsigned bar = 0; bar < 10; bar++) {
        SDL_SetRenderDrawColor(renderer, bar < volume / 10 ? 120 : 45,
                               bar < volume / 10 ? 220 : 55,
                               bar < volume / 10 ? 190 : 65, 255);
        SDL_Rect meter = {405 + (int)bar * 12, y - 2, 9, 16};
        SDL_RenderFillRect(renderer, &meter);
      }
    } else if (i == 6) {
      static const char *const palettes[] = {"NONE", "GREEN", "SEPIA"};
      const char *value = palettes[settings->palette];
      draw_text(renderer, value, 455, y, 2, (SDL_Color){180, 230, 210, 255});
    } else if (i >= 1 && i <= 4) {
      for (unsigned slot = 0; slot < 5; slot++) {
        int x = 300 + (int)slot * 42;
        int active = slot == settings->state_slot;
        int exists = i <= 2 ? save_slot_exists(save_path, slot)
                            : state_slot_exists(state_path, slot);
        SDL_SetRenderDrawColor(renderer, exists ? 35 : 18,
                               active ? 100 : exists ? 70 : 28,
                               exists ? 55 : 40, 255);
        SDL_Rect slot_rect = {x - 3, y - 5, 36, 27};
        SDL_RenderFillRect(renderer, &slot_rect);
        char label[5];
        snprintf(label, sizeof label, "[%u]", slot + 1);
        draw_text(renderer, label, x, y, 2,
                  exists ? (SDL_Color){255, 240, 180, 255}
                         : (SDL_Color){150, 160, 165, 255});
      }
    }
  }
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

static void draw_button(SDL_Renderer *renderer, SDL_Rect rect, int pressed,
                        int highlighted) {
  SDL_SetRenderDrawColor(renderer,
                         highlighted ? 210 : pressed ? 220 : 70,
                         highlighted ? 170 : pressed ? 70 : 70,
                         highlighted ? 45 : pressed ? 70 : 80, 190);
  SDL_RenderFillRect(renderer, &rect);
  SDL_SetRenderDrawColor(renderer, highlighted ? 255 : 245,
                         highlighted ? 240 : 245, highlighted ? 120 : 245, 220);
  SDL_RenderDrawRect(renderer, &rect);
}

static void draw_ui_icon(SDL_Renderer *renderer, const uint8_t *rows, int x,
                         int y, int scale) {
  SDL_SetRenderDrawColor(renderer, 245, 245, 245, 230);
  for (int row = 0; row < 7; row++)
    for (int bit = 0; bit < 7; bit++)
      if (rows[row] & (1u << (6 - bit))) {
        SDL_Rect pixel = {x - 3 * scale + bit * scale,
                          y - 3 * scale + row * scale, scale, scale};
        SDL_RenderFillRect(renderer, &pixel);
      }
}

static void draw_arrow(SDL_Renderer *renderer, int x, int y, unsigned direction) {
  draw_ui_icon(renderer, ui_assets.arrow[direction], x, y, 3);
}

static void draw_settings_button(SDL_Renderer *renderer) {
  SDL_Rect rect = {600, 704, 32, 32};
  draw_button(renderer, rect, 0, 0);
  draw_ui_icon(renderer, ui_assets.cog, 616, 720, 2);
}

static void draw_debug_button(SDL_Renderer *renderer) {
  SDL_Rect rect = {560, 704, 32, 32};
  draw_button(renderer, rect, 0, 0);
  draw_ui_icon(renderer, ui_assets.bug, 576, 720, 2);
}

static int controller_button_at(int x, int y) {
  static const SDL_Rect rects[8] = {
      {96, 656, 32, 32},  {32, 656, 32, 32},  {64, 624, 32, 32},
      {64, 688, 32, 32},  {576, 608, 40, 40}, {528, 640, 40, 40},
      {264, 656, 48, 24}, {328, 656, 48, 24},
  };
  for (int i = 0; i < 8; i++)
    if (x >= rects[i].x && x < rects[i].x + rects[i].w &&
        y >= rects[i].y && y < rects[i].y + rects[i].h)
      return i;
  return -1;
}

static int settings_button_at(int x, int y) {
  return x >= 600 && x < 632 && y >= 704 && y < 736;
}

static int debug_button_at(int x, int y) {
  return x >= 560 && x < 592 && y >= 704 && y < 736;
}

static void draw_debug_overlay(SDL_Renderer *renderer, const gb_t *gb) {
  gb_regs_t regs;
  char line[32];
  SDL_SetRenderDrawColor(renderer, 8, 12, 20, 230);
  SDL_RenderFillRect(renderer, &(SDL_Rect){12, 12, 220, 92});
  gb_dbg_regs(gb, &regs);
  draw_text(renderer, "DEBUG", 24, 22, 2, (SDL_Color){255, 240, 180, 255});
  snprintf(line, sizeof line, "PC %04X", regs.pc);
  draw_text(renderer, line, 24, 48, 2, (SDL_Color){235, 245, 240, 255});
  snprintf(line, sizeof line, "AF %04X", regs.af);
  draw_text(renderer, line, 24, 72, 2, (SDL_Color){235, 245, 240, 255});
  snprintf(line, sizeof line, "HL %04X", regs.hl);
  draw_text(renderer, line, 120, 72, 2, (SDL_Color){235, 245, 240, 255});
}

static void draw_controller(SDL_Renderer *renderer, uint8_t buttons,
                            int remapping) {
  SDL_Rect rect;
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  rect = (SDL_Rect){64, 624, 32, 32};
  draw_button(renderer, rect, buttons & (1 << 2), remapping == 2);
  rect = (SDL_Rect){64, 688, 32, 32};
  draw_button(renderer, rect, buttons & (1 << 3), remapping == 3);
  rect = (SDL_Rect){32, 656, 32, 32};
  draw_button(renderer, rect, buttons & (1 << 1), remapping == 1);
  rect = (SDL_Rect){96, 656, 32, 32};
  draw_button(renderer, rect, buttons & (1 << 0), remapping == 0);
  draw_arrow(renderer, 80, 640, 0);
  draw_arrow(renderer, 80, 704, 1);
  draw_arrow(renderer, 48, 672, 2);
  draw_arrow(renderer, 112, 672, 3);
  rect = (SDL_Rect){264, 656, 48, 24};
  draw_button(renderer, rect, buttons & (1 << 6), remapping == 6);
  rect = (SDL_Rect){328, 656, 48, 24};
  draw_button(renderer, rect, buttons & (1 << 7), remapping == 7);
  rect = (SDL_Rect){528, 640, 40, 40};
  draw_button(renderer, rect, buttons & (1 << 5), remapping == 5);
  rect = (SDL_Rect){576, 608, 40, 40};
  draw_button(renderer, rect, buttons & (1 << 4), remapping == 4);
  draw_text(renderer, "SELECT", 270, 664, 1, (SDL_Color){245, 245, 245, 230});
  draw_text(renderer, "START", 337, 664, 1, (SDL_Color){245, 245, 245, 230});
  draw_text(renderer, "B", 543, 657, 2, (SDL_Color){245, 245, 245, 230});
  draw_text(renderer, "A", 591, 625, 2, (SDL_Color){245, 245, 245, 230});
  draw_debug_button(renderer);
  draw_settings_button(renderer);
}

static int load_ui_assets(void) {
  static const char *const paths[] = {
      "ui_assets.txt", "src/frontend/sdl/ui_assets.txt",
      "../src/frontend/sdl/ui_assets.txt"};
  for (size_t i = 0; i < sizeof paths / sizeof *paths; i++)
    if (gb_ui_assets_load(&ui_assets, paths[i]) == 0)
      return 0;
  return -1;
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
  settings_t settings = {0};
  settings.remapping = -1;
  settings.state_slot = 0;
  SDL_Keycode keys[8] = {SDLK_RIGHT, SDLK_LEFT, SDLK_UP, SDLK_DOWN,
                         SDLK_z, SDLK_x, SDLK_LSHIFT, SDLK_RETURN};
  int running = 1, paused = debug, debug_overlay = 0;
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
  if (load_ui_assets() != 0) {
    SDL_Quit();
    free(state);
    gb_destroy(gb);
    return 1;
  }
  window = SDL_CreateWindow("Game Boy", SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED, 640, 768, 0);
  renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : NULL;
  texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
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
        if (event.type == SDL_KEYDOWN) {
          SDL_Keycode key = event.key.keysym.sym;
          if (settings.browser) {
            if (settings.confirm_load) {
              if (key == SDLK_ESCAPE || key == SDLK_n) {
                settings.confirm_load = 0;
              } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_y) &&
                         settings.rom_count) {
                size_t new_size;
                uint8_t *new_rom = read_file(settings.roms[settings.rom_selected].path,
                                             &new_size);
                if (new_rom && gb_load_rom(gb, new_rom, new_size) == 0) {
                  if (snprintf(path, sizeof path, "%s",
                               settings.roms[settings.rom_selected].path) <
                          (int)sizeof path &&
                      snprintf(save_path, sizeof save_path, "%s.sav", path) <
                          (int)sizeof save_path &&
                      snprintf(state_path, sizeof state_path, "%s.state", path) <
                          (int)sizeof state_path) {
                    free(state);
                    state_size = gb_save_state_size(gb);
                    state = malloc(state_size);
                    settings.open = 0;
                    settings.browser = 0;
                    settings.confirm_load = 0;
                    SDL_StopTextInput();
                  }
                }
                free(new_rom);
              }
            } else if (key == SDLK_ESCAPE) {
              settings.browser = 0;
            } else if (key == SDLK_UP && settings.rom_selected > 0) {
              settings.rom_selected--;
            } else if (key == SDLK_DOWN && settings.rom_selected + 1 < settings.rom_count) {
              settings.rom_selected++;
            } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) &&
                       settings.rom_count) {
              settings.confirm_load = 1;
            }
          } else if (settings.remapping >= 0 && key != SDLK_ESCAPE) {
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
          } else if (key == SDLK_LEFT && settings.selected >= 1 && settings.selected <= 4 &&
                     settings.state_slot > 0) {
            settings.state_slot--;
          } else if (key == SDLK_RIGHT && settings.selected >= 1 && settings.selected <= 4 &&
                     settings.state_slot < 4) {
            settings.state_slot++;
          } else if (key == SDLK_LEFT && settings.selected == 5 && audio_context.volume >= 10) {
            audio_context.volume -= 10;
          } else if (key == SDLK_RIGHT && settings.selected == 5 && audio_context.volume <= 90) {
            audio_context.volume += 10;
          } else if (key == SDLK_LEFT && settings.selected == 6) {
            settings.palette = (settings.palette + 2) % 3;
          } else if (key == SDLK_RIGHT && settings.selected == 6) {
            settings.palette = (settings.palette + 1) % 3;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 0) {
            settings.browser = 1;
            settings.rom_selected = 0;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 1) {
            char slot_path[4096];
            if (save_slot_path(save_path, settings.state_slot, slot_path,
                               sizeof slot_path) == 0)
              save_ram(gb, slot_path);
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 2) {
            char slot_path[4096];
            if (save_slot_path(save_path, settings.state_slot, slot_path,
                               sizeof slot_path) != 0)
              continue;
            file = read_file(slot_path, &file_size);
            if (file) { gb_load_ram(gb, file, file_size); free(file); }
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 3) {
            char slot_path[4096];
            if (state_slot_path(state_path, settings.state_slot, slot_path,
                                sizeof slot_path) == 0)
              save_state(gb, slot_path, state, state_size);
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 4) {
            char slot_path[4096];
            if (state_slot_path(state_path, settings.state_slot, slot_path,
                                sizeof slot_path) != 0)
              continue;
            file = read_file(slot_path, &file_size);
            if (file) { if (file_size == state_size) gb_load_state(gb, file, file_size); free(file); }
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 6) {
            settings.palette = (settings.palette + 1) % 3;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 7) {
            settings.remapping = 0;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 8) {
            gb_reset(gb);
            settings.open = 0;
            SDL_StopTextInput();
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 9) {
            settings.open = 0;
            SDL_StopTextInput();
          }
        }
        continue;
      }
      if (event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT) {
        if (settings_button_at(event.button.x, event.button.y)) {
          open_settings(&settings, debug);
          SDL_StartTextInput();
          continue;
        }
        if (debug_button_at(event.button.x, event.button.y)) {
          debug_overlay = !debug_overlay;
          paused = debug || debug_overlay;
          continue;
        }
        int button = controller_button_at(event.button.x, event.button.y);
        if (button >= 0) {
          buttons |= (uint8_t)(1u << button);
          gb_set_input(gb, buttons);
        }
      }
      if (event.type == SDL_MOUSEBUTTONUP &&
          event.button.button == SDL_BUTTON_LEFT) {
        int button = controller_button_at(event.button.x, event.button.y);
        if (button >= 0) {
          buttons &= (uint8_t)~(1u << button);
          gb_set_input(gb, buttons);
        }
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
        open_settings(&settings, debug);
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
    if (!paused && !settings.open)
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
    if (debug_overlay)
      draw_debug_overlay(renderer, gb);
    draw_controller(renderer, buttons, settings.remapping);
    if (settings.open)
      draw_settings(renderer, &settings, audio_context.volume, save_path,
                    state_path, keys);
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
  free_roms(settings.roms, settings.rom_count);
  gb_destroy(gb);
  return 0;
}
