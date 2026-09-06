#include "gb.h"
#include "config.h"
#include "input.h"
#include "ui_assets.h"
#include <SDL.h>
#include <ctype.h>
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

static int save_ram(const gb_t *gb, const char *path) {
  size_t size = gb_save_ram_size(gb);
  uint8_t *data = size ? malloc(size) : NULL;
  int result = -1;
  if (size && data) {
    gb_save_ram(gb, data);
    result = write_file(path, data, size);
  }
  free(data);
  return result;
}

static int load_ram(gb_t *gb, const char *path) {
  size_t size;
  uint8_t *data = read_file(path, &size);
  int result;
  if (!data)
    return -1;
  result = gb_load_ram(gb, data, size);
  free(data);
  if (result == 0)
    gb_reset(gb);
  return result;
}

static int save_state(const gb_t *gb, const char *path, uint8_t *buffer,
                      size_t size) {
  if (gb_save_state(gb, buffer) != size)
    return -1;
  return write_file(path, buffer, size);
}

static int load_state_file(gb_t *gb, const char *path, size_t size) {
  size_t file_size;
  uint8_t *file = read_file(path, &file_size);
  int result = -1;
  if (file) {
    if (file_size == size)
      result = gb_load_state(gb, file, file_size);
    free(file);
  }
  return result;
}

static int state_slot_path(const char *base, unsigned slot, char *path,
                           size_t size) {
  if (slot == 5)
    return snprintf(path, size, "%s.auto", base) < (int)size ? 0 : -1;
  if (slot == 0)
    return snprintf(path, size, "%s", base) < (int)size ? 0 : -1;
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
  if (slot == 0)
    return snprintf(path, size, "%s", base) < (int)size ? 0 : -1;
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

static void draw_text_right(SDL_Renderer *renderer, const char *text, int right,
                            int y, int scale, SDL_Color color) {
  draw_text(renderer, text, right - (int)strlen(text) * 6 * scale, y, scale,
            color);
}

static uint8_t mapped_button(SDL_Keycode key, const SDL_Keycode keys[8]) {
  for (unsigned i = 0; i < 8; i++)
    if (keys[i] == key)
      return (uint8_t)(1u << i);
  return 0;
}

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

static void open_settings(settings_t *settings, int debug) {
  settings->open = 1;
  settings->selected = 0;
  settings->browser = 0;
  settings->confirm_load = 0;
  settings->confirm_action = 0;
  settings->config_error = 0;
  free_roms(settings->roms, settings->rom_count);
  settings->roms = NULL;
  settings->rom_count = 0;
  settings->rom_selected = 0;
  discover_roms(&settings->roms, &settings->rom_count, debug);
}

static const char *setting_names[] = {
    "LOAD ROM", "SAVE SLOT", "SAVE STATE", "LOAD STATE", "VOLUME",
    "CHANGE PALETTE", "SPEED", "AUTO SAVE", "REMAP KEYS", "RESET", "CLOSE",
    "QUIT"};

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
    draw_text(renderer, "ENTER LOAD  ESC BACK", 105, 475, 2,
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
    if (i == 4) {
      for (unsigned bar = 0; bar < 10; bar++) {
        SDL_SetRenderDrawColor(renderer, bar < volume / 10 ? 120 : 45,
                               bar < volume / 10 ? 220 : 55,
                               bar < volume / 10 ? 190 : 65, 255);
        SDL_Rect meter = {411 + (int)bar * 12, y - 2, 9, 16};
        SDL_RenderFillRect(renderer, &meter);
      }
    } else if (i == 5) {
      static const char *const palettes[] = {"NONE", "GREEN", "SEPIA"};
      const char *value = palettes[settings->palette];
      draw_text_right(renderer, value, 528, y, 2,
                      (SDL_Color){180, 230, 210, 255});
    } else if (i == 6) {
      static const char *const speeds[] = {"OFF", "2X", "3X", "4X", "5X"};
      draw_text_right(renderer, speeds[settings->speed], 528, y, 2,
                      (SDL_Color){180, 230, 210, 255});
    } else if (i == 7) {
      static const char *const autosaves[] = {"OFF", "5S", "10S", "30S",
                                              "1M", "5M", "30M"};
      draw_text_right(renderer, autosaves[settings->autosave], 528, y, 2,
                      (SDL_Color){180, 230, 210, 255});
    } else if (i >= 1 && i <= 3) {
      unsigned slot_count = i == 1 ? 5 : 6;
      unsigned selected_slot = i == 1 ? settings->save_slot : settings->state_slot;
      int start_x = slot_count == 6 ? 285 : 327;
      for (unsigned display_slot = 0; display_slot < slot_count; display_slot++) {
        unsigned slot = i >= 2 && display_slot == 0 ? 5 :
                        i >= 2 ? display_slot - 1 : display_slot;
        int x = start_x + (int)display_slot * 42;
        int active = slot == selected_slot;
        int exists = i == 1 ? save_slot_exists(save_path, slot)
                            : state_slot_exists(state_path, slot);
        SDL_SetRenderDrawColor(renderer, exists ? 35 : 18,
                               active ? 100 : exists ? 70 : 28,
                               exists ? 55 : 40, 255);
        SDL_Rect slot_rect = {x - 3, y - 5, 36, 27};
        SDL_RenderFillRect(renderer, &slot_rect);
        char label[6];
        if (i >= 2 && slot == 5)
          snprintf(label, sizeof label, "[A]");
        else
          snprintf(label, sizeof label, "[%u]", slot + 1);
        draw_text(renderer, label, x, y, 2,
                  exists ? (SDL_Color){255, 240, 180, 255}
                         : (SDL_Color){150, 160, 165, 255});
      }
    }
  }
  if (settings->confirm_action) {
    SDL_SetRenderDrawColor(renderer, 80, 65, 25, 255);
    SDL_Rect prompt_box = {100, 465, 440, 38};
    SDL_RenderFillRect(renderer, &prompt_box);
    const char *prompt = settings->confirm_action == 1
                             ? "LOAD SAVE? ENTER YES ESC NO"
                             : settings->confirm_action == 2
                                   ? "SAVE STATE? ENTER YES ESC NO"
                                   : "LOAD STATE? ENTER YES ESC NO";
    draw_text(renderer, prompt,
              145, 476, 2, (SDL_Color){255, 240, 180, 255});
  }
  if (settings->config_error)
    draw_text(renderer, "CONFIG SAVE FAILED", 105, 508, 1,
              (SDL_Color){255, 120, 110, 255});
  if (settings->state_error)
    draw_text(renderer, "STATE LOAD FAILED - SAVE A NEW STATE", 105, 520, 1,
              (SDL_Color){255, 120, 110, 255});
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
  SDL_Rect rect = {576, 704, 32, 32};
  draw_button(renderer, rect, 0, 0);
  draw_ui_icon(renderer, ui_assets.cog, 592, 720, 2);
}

static void draw_debug_button(SDL_Renderer *renderer) {
  SDL_Rect rect = {536, 704, 32, 32};
  draw_button(renderer, rect, 0, 0);
  draw_ui_icon(renderer, ui_assets.bug, 552, 720, 2);
}

static int controller_button_at(int x, int y) {
  static const SDL_Rect rects[8] = {
      {96, 656, 32, 32},  {32, 656, 32, 32},  {64, 624, 32, 32},
      {64, 688, 32, 32},  {568, 608, 40, 40}, {520, 640, 40, 40},
      {264, 656, 48, 24}, {328, 656, 48, 24},
  };
  for (int i = 0; i < 8; i++)
    if (x >= rects[i].x && x < rects[i].x + rects[i].w &&
        y >= rects[i].y && y < rects[i].y + rects[i].h)
      return i;
  return -1;
}

static int settings_button_at(int x, int y) {
  return x >= 576 && x < 608 && y >= 704 && y < 736;
}

static int debug_button_at(int x, int y) {
  return x >= 536 && x < 568 && y >= 704 && y < 736;
}

typedef struct {
  uint16_t base;
  uint16_t selected_address;
  int selected;
  char edit[3];
  unsigned edit_len;
  char search[6];
  unsigned search_len;
  int searching;
  int search_refine;
  unsigned search_width;
  size_t match_count;
  uint8_t matches[65536];
  uint8_t snapshot[65536];
} debug_view_t;

static char hex_digit(unsigned value) {
  return (char)(value < 10 ? '0' + value : 'A' + value - 10);
}

static int hex_value(SDL_Keycode key) {
  if (key >= SDLK_0 && key <= SDLK_9)
    return (int)(key - SDLK_0);
  if (key >= SDLK_KP_0 && key <= SDLK_KP_9)
    return (int)(key - SDLK_KP_0);
  if (key >= SDLK_a && key <= SDLK_f)
    return (int)(key - SDLK_a + 10);
  return -1;
}

static int hex_char_value(char key) {
  if (key >= '0' && key <= '9')
    return key - '0';
  if (key >= 'A' && key <= 'F')
    return key - 'A' + 10;
  return -1;
}

static void draw_debug_overlay(SDL_Renderer *renderer, gb_t *gb,
                               debug_view_t *view) {
  gb_regs_t regs;
  char line[64];
  char disasm[64];
  static const char *const names[] = {"Z", "N", "H", "C"};
  SDL_SetRenderDrawColor(renderer, 8, 12, 20, 230);
  SDL_RenderFillRect(renderer, &(SDL_Rect){8, 8, 624, 560});
  gb_dbg_regs(gb, &regs);
  draw_text(renderer, "DEBUG", 20, 18, 2, (SDL_Color){255, 240, 180, 255});
  draw_text(renderer, "ESC CLOSE  S STEP  C CONTINUE  / SEARCH", 20, 42, 1,
            (SDL_Color){190, 205, 215, 255});
  draw_text(renderer, "CPU", 20, 72, 1, (SDL_Color){255, 240, 180, 255});
  snprintf(line, sizeof line, "AF %04X  BC %04X  DE %04X", regs.af, regs.bc,
           regs.de);
  draw_text(renderer, line, 20, 90, 1, (SDL_Color){235, 245, 240, 255});
  snprintf(line, sizeof line, "HL %04X  SP %04X  PC %04X", regs.hl, regs.sp,
           regs.pc);
  draw_text(renderer, line, 20, 106, 1, (SDL_Color){235, 245, 240, 255});
  snprintf(line, sizeof line, "IME %d  HALT %d", regs.ime, regs.halted);
  draw_text(renderer, line, 20, 122, 1, (SDL_Color){235, 245, 240, 255});
  draw_text(renderer, "FLAGS", 220, 72, 1, (SDL_Color){255, 240, 180, 255});
  for (unsigned i = 0; i < 4; i++) {
    snprintf(line, sizeof line, "%s %c", names[i],
             regs.af & (0x80u >> i * 2) ? '*' : '.');
    draw_text(renderer, line, 220 + (int)i * 38, 90, 1,
              (SDL_Color){235, 245, 240, 255});
  }
  draw_text(renderer, "OPS", 20, 150, 1, (SDL_Color){255, 240, 180, 255});
  for (unsigned i = 0; i < 6; i++) {
    uint16_t address = (uint16_t)(regs.pc + i);
    int length = gb_dbg_disasm(gb, address, disasm, sizeof disasm);
    snprintf(line, sizeof line, "%c%04X  %.48s", address == regs.pc ? '>' : ' ',
             address, disasm);
    draw_text(renderer, line, 20, 168 + (int)i * 16, 1,
              (SDL_Color){235, 245, 240, 255});
    if (length > 1)
      i += (unsigned)length - 1;
  }
  draw_text(renderer, "RAM  WRITES USE CPU BUS", 300, 150, 1,
            (SDL_Color){255, 240, 180, 255});
  if (view->searching)
    snprintf(line, sizeof line, "%s DECIMAL [%s]",
             view->search_refine ? "REFINE" : "SEARCH", view->search);
  else
    snprintf(line, sizeof line, "VALUE SEARCH  MATCHES %lu  %u-BYTE",
             (unsigned long)view->match_count, view->search_width ? view->search_width : 1);
  draw_text(renderer, line, 300, 168, 1, (SDL_Color){235, 245, 240, 255});
  draw_button(renderer, (SDL_Rect){300, 400, 92, 22}, 0, 0);
  draw_text(renderer, "SEARCH", 310, 406, 1,
            (SDL_Color){235, 245, 240, 255});
  draw_button(renderer, (SDL_Rect){400, 400, 92, 22}, view->match_count != 0, 0);
  draw_text(renderer, "REFINE", 412, 406, 1,
            (SDL_Color){235, 245, 240, 255});
  draw_button(renderer, (SDL_Rect){500, 400, 92, 22}, view->match_count != 0, 0);
  draw_text(renderer, "NEXT", 518, 406, 1,
            (SDL_Color){235, 245, 240, 255});
  draw_button(renderer, (SDL_Rect){300, 428, 92, 22}, view->match_count != 0, 0);
  draw_text(renderer, "CHANGED", 306, 434, 1,
            (SDL_Color){235, 245, 240, 255});
  draw_button(renderer, (SDL_Rect){400, 428, 92, 22}, view->match_count != 0, 0);
  draw_text(renderer, "SAME", 422, 434, 1,
            (SDL_Color){235, 245, 240, 255});
  draw_text(renderer, "RESULTS  DECIMAL BASE 10", 20, 280, 1,
            (SDL_Color){255, 240, 180, 255});
  {
    unsigned shown = 0;
    for (unsigned address = 0; address <= 0xffffu && shown < 8; address++) {
      if (!view->matches[address])
        continue;
      uint8_t low = gb_dbg_read(gb, (uint16_t)address);
      uint8_t high = view->search_width == 2 && address != 0xffffu
                         ? gb_dbg_read(gb, (uint16_t)(address + 1))
                         : 0;
      if (view->search_width == 2)
        snprintf(line, sizeof line, "%c%04X  %02X %02X  LE %u  BE %u",
                 view->selected && view->selected_address == address ? '>' : ' ',
                 address, low, high, (unsigned)(low | high << 8),
                 (unsigned)(low << 8 | high));
      else
        snprintf(line, sizeof line, "%c%04X  %02X  DEC %u",
                 view->selected && view->selected_address == address ? '>' : ' ',
                 address, low, (unsigned)low);
      draw_text(renderer, line, 20, 300 + (int)shown * 16, 1,
                (SDL_Color){235, 245, 240, 255});
      shown++;
    }
    if (view->match_count > shown)
      draw_text(renderer, "... NEXT FOR MORE", 20, 300 + (int)shown * 16, 1,
                (SDL_Color){180, 195, 205, 255});
  }
  for (unsigned row = 0; row < 8; row++) {
    uint16_t row_address = (uint16_t)(view->base + row * 16);
    snprintf(line, sizeof line, "%04X", row_address);
    draw_text(renderer, line, 300, 190 + (int)row * 22, 1,
              (SDL_Color){180, 195, 205, 255});
    for (unsigned col = 0; col < 16; col++) {
      uint16_t address = (uint16_t)(row_address + col);
      uint8_t value = gb_dbg_read(gb, address);
      int x = 340 + (int)col * 17;
      int y = 190 + (int)row * 22;
      if (view->selected && address == view->selected_address) {
        SDL_SetRenderDrawColor(renderer, 180, 120, 30, 255);
        SDL_RenderFillRect(renderer, &(SDL_Rect){x - 1, y - 1, 15, 13});
      }
      snprintf(line, sizeof line, "%c%c", hex_digit(value >> 4),
               hex_digit(value & 15));
      draw_text(renderer, line, x, y, 1,
                (SDL_Color){235, 245, 240, 255});
    }
  }
  if (view->edit_len) {
    snprintf(line, sizeof line, "EDIT %04X [%s] ENTER WRITE  ESC CANCEL",
             view->selected_address, view->edit);
    draw_text(renderer, line, 300, 380, 1,
              (SDL_Color){255, 210, 120, 255});
  } else {
    draw_text(renderer, "CLICK BYTE  TYPE HEX  ENTER WRITE", 300, 380, 1,
              (SDL_Color){190, 205, 215, 255});
  }
}

static int debug_memory_at(int x, int y, uint16_t base, uint16_t *address) {
  if (x < 340 || x >= 612 || y < 190 || y >= 366)
    return 0;
  unsigned col = (unsigned)(x - 340) / 17;
  unsigned row = (unsigned)(y - 190) / 22;
  if (col >= 16 || row >= 8)
    return 0;
  *address = (uint16_t)(base + row * 16 + col);
  return 1;
}

static int debug_search_button_at(int x, int y, int refine) {
  int left;
  int top;
  if (refine <= 2) {
    left = refine == 0 ? 300 : refine == 1 ? 400 : 500;
    top = 400;
  } else {
    left = refine == 3 ? 300 : 400;
    top = 428;
  }
  return x >= left && x < left + 92 && y >= top && y < top + 22;
}

static int debug_result_at(const debug_view_t *view, int x, int y,
                           uint16_t *address) {
  unsigned wanted;
  if (x < 20 || x >= 180 || y < 300 || y >= 428)
    return 0;
  wanted = (unsigned)(y - 300) / 16;
  for (unsigned candidate = 0; candidate <= 0xffffu; candidate++) {
    if (!view->matches[candidate])
      continue;
    if (wanted == 0) {
      *address = (uint16_t)candidate;
      return 1;
    }
    wanted--;
  }
  return 0;
}

static void debug_edit_key(debug_view_t *view, SDL_Keycode key) {
  int value = hex_value(key);
  if (value < 0)
    return;
  if (view->searching && value >= 0 && value <= 9 && view->search_len < 5) {
    view->search[view->search_len++] = (char)('0' + value);
    view->search[view->search_len] = '\0';
  } else if (!view->searching && view->edit_len < 2) {
    view->edit[view->edit_len++] = hex_digit((unsigned)value);
    view->edit[view->edit_len] = '\0';
  }
}

static void debug_scan_value(gb_t *gb, debug_view_t *view) {
  unsigned value = 0;
  unsigned width;
  size_t new_count = 0;
  size_t first = 0;
  int found = 0;
  if (!view->search_len)
    return;
  for (unsigned i = 0; i < view->search_len; i++)
    value = value * 10u + (unsigned)(view->search[i] - '0');
  if (value > 65535u)
    return;
  width = value > 255u ? 2u : 1u;
  view->search_width = width;
  if (!view->search_refine) {
    memset(view->matches, 0, sizeof view->matches);
  }
  for (unsigned address = 0; address + width <= 0x10000u; address++) {
    if (view->search_refine && !view->matches[address])
      continue;
    uint16_t low = gb_dbg_read(gb, (uint16_t)address);
    int matches_value = low == value;
    if (width == 2) {
      uint16_t high = gb_dbg_read(gb, (uint16_t)(address + 1));
      uint16_t little_endian = (uint16_t)(low | (high << 8));
      uint16_t big_endian = (uint16_t)((low << 8) | high);
      matches_value = little_endian == value || big_endian == value;
    }
    if (matches_value) {
      view->matches[address] = 1;
      new_count++;
      if (!found) {
        first = address;
        found = 1;
      }
    } else {
      view->matches[address] = 0;
    }
  }
  for (unsigned address = 0; address + width <= 0x10000u; address++) {
    if (view->matches[address]) {
      view->snapshot[address] = gb_dbg_read(gb, (uint16_t)address);
      if (width == 2)
        view->snapshot[address + 1] =
            gb_dbg_read(gb, (uint16_t)(address + 1));
    }
  }
  view->match_count = new_count;
  if (found) {
    view->base = (uint16_t)(first & 0xfff0u);
    view->selected_address = (uint16_t)first;
    view->selected = 1;
  }
  view->searching = 0;
  view->search_refine = 0;
  view->search_len = 0;
  view->search[0] = '\0';
}

static void debug_scan_changed(gb_t *gb, debug_view_t *view, int same) {
  size_t count = 0;
  size_t first = 0;
  int found = 0;
  unsigned width = view->search_width ? view->search_width : 1;
  for (unsigned address = 0; address + width <= 0x10000u; address++) {
    if (!view->matches[address])
      continue;
    uint16_t value = gb_dbg_read(gb, (uint16_t)address);
    uint16_t old = view->snapshot[address];
    if (width == 2) {
      value |= (uint16_t)gb_dbg_read(gb, (uint16_t)(address + 1)) << 8;
      old |= (uint16_t)view->snapshot[address + 1] << 8;
    }
    if ((value == old) != same) {
      view->matches[address] = 0;
      continue;
    }
    view->snapshot[address] = (uint8_t)value;
    if (width == 2)
      view->snapshot[address + 1] = (uint8_t)(value >> 8);
    count++;
    if (!found) {
      first = address;
      found = 1;
    }
  }
  view->match_count = count;
  if (found) {
    view->base = (uint16_t)(first & 0xfff0u);
    view->selected_address = (uint16_t)first;
    view->selected = 1;
  }
}

static void debug_next_match(debug_view_t *view) {
  if (!view->match_count)
    return;
  unsigned start = view->selected ? (unsigned)view->selected_address + 1u : 0u;
  for (unsigned offset = 0; offset <= 0xffffu; offset++) {
    unsigned address = (start + offset) & 0xffffu;
    if (view->matches[address]) {
      view->selected_address = (uint16_t)address;
      view->selected = 1;
      view->base = (uint16_t)(address & 0xfff0u);
      return;
    }
  }
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
  rect = (SDL_Rect){520, 640, 40, 40};
  draw_button(renderer, rect, buttons & (1 << 5), remapping == 5);
  rect = (SDL_Rect){568, 608, 40, 40};
  draw_button(renderer, rect, buttons & (1 << 4), remapping == 4);
  draw_text(renderer, "SELECT", 270, 664, 1, (SDL_Color){245, 245, 245, 230});
  draw_text(renderer, "START", 337, 664, 1, (SDL_Color){245, 245, 245, 230});
  draw_text(renderer, "B", 535, 657, 2, (SDL_Color){245, 245, 245, 230});
  draw_text(renderer, "A", 583, 625, 2, (SDL_Color){245, 245, 245, 230});
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
  char main_config_path[4096], config_path[4096];
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
  debug_view_t debug_view = {0};
  settings.remapping = -1;
  settings.state_slot = 0;
  debug_view.base = 0xc000;
  SDL_Keycode keys[8] = {SDLK_RIGHT, SDLK_LEFT, SDLK_UP, SDLK_DOWN,
                         SDLK_z, SDLK_x, SDLK_LSHIFT, SDLK_RETURN};
  int running = 1, paused = debug, debug_overlay = 0;
  int command_line_rom = 0;
  unsigned save_timer = 0, autosave_timer = 0;
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
  command_line_rom = path[0] != '\0';
  if (command_line_rom)
    config_paths(path, main_config_path, sizeof main_config_path,
                 config_path, sizeof config_path);
  else
    snprintf(main_config_path, sizeof main_config_path, "gameboy.cfg");
  /* An explicit ROM argument must win over a persisted LOADROM setting. */
  load_config(main_config_path, path, sizeof path, &settings,
              &audio_context.volume, keys, !command_line_rom);
  if (!path[0]) {
    fprintf(stderr, "usage: %s [--debug] [--boot-rom FILE] ROM\n", argv[0]);
    return 2;
  }
  config_paths(path, main_config_path, sizeof main_config_path,
               config_path, sizeof config_path);
  load_config(config_path, path, sizeof path, &settings,
              &audio_context.volume, keys, 0);
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

  {
    char slot_path[4096];
    if (save_slot_path(save_path, settings.save_slot, slot_path,
                       sizeof slot_path) == 0)
      load_ram(gb, slot_path);
  }
  size_t state_size = gb_save_state_size(gb);
  state = malloc(state_size);
  {
    char slot_path[4096];
    if (state_slot_path(state_path, settings.state_slot, slot_path,
                        sizeof slot_path) == 0) {
      if (state_slot_exists(state_path, settings.state_slot) &&
          load_state_file(gb, slot_path, state_size) != 0)
        settings.state_error = 1;
    }
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
    int skip_boot = 0;
    while (running && !skip_boot && SDL_GetTicks() - boot_start < 5000) {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
          running = 0;
        } else if (event.type == SDL_KEYDOWN ||
                   event.type == SDL_MOUSEBUTTONDOWN ||
                   event.type == SDL_CONTROLLERBUTTONDOWN) {
          skip_boot = 1;
        }
      }
      Uint32 elapsed = SDL_GetTicks() - boot_start;
      int y = -32 + (int)((elapsed < 1500 ? elapsed * 304 / 1500 : 304));
      draw_boot_logo(renderer, y);
      SDL_Delay(16);
    }
    if (running && !skip_boot)
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
                    load_ram(gb, save_path);
                    config_paths(path, main_config_path, sizeof main_config_path,
                                 config_path, sizeof config_path);
                    load_config(config_path, path, sizeof path, &settings,
                                &audio_context.volume, keys, 0);
                    settings.dirty = 1;
                    save_timer = autosave_timer = 0;
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
            } else if (key == SDLK_UP && settings.rom_count) {
              settings.rom_selected = settings.rom_selected == 0
                                          ? settings.rom_count - 1
                                          : settings.rom_selected - 1;
            } else if (key == SDLK_DOWN && settings.rom_count) {
              settings.rom_selected = settings.rom_selected + 1 == settings.rom_count
                                          ? 0
                                          : settings.rom_selected + 1;
            } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) &&
                       settings.rom_count) {
              settings.confirm_load = 1;
            }
          } else if (settings.remapping >= 0 && key != SDLK_ESCAPE) {
            keys[settings.remapping] = key;
            settings.remapping++;
            if (settings.remapping == 8)
              settings.remapping = -1;
            settings.dirty = 1;
          } else if (settings.confirm_action) {
            if (key == SDLK_ESCAPE || key == SDLK_n) {
              settings.confirm_action = 0;
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_y) {
              char slot_path[4096];
              if (settings.confirm_action == 1) {
                if (save_slot_path(save_path, settings.save_slot, slot_path,
                                   sizeof slot_path) == 0) {
                  file = read_file(slot_path, &file_size);
                  if (file) {
                    if (gb_load_ram(gb, file, file_size) == 0)
                      gb_reset(gb);
                    free(file);
                  }
                }
              } else if (state_slot_path(state_path, settings.state_slot, slot_path,
                                         sizeof slot_path) == 0) {
                if (settings.confirm_action == 2) {
                  settings.state_error =
                      save_state(gb, slot_path, state, state_size) != 0;
                } else {
                  settings.state_error =
                      load_state_file(gb, slot_path, state_size) != 0;
                }
              }
              settings.confirm_action = 0;
            }
          } else if (key == SDLK_ESCAPE) {
            settings.open = 0;
            settings.remapping = -1;
            SDL_StopTextInput();
          } else if (key == SDLK_UP) {
            settings.selected = settings.selected == 0 ? 11 : settings.selected - 1;
          } else if (key == SDLK_DOWN) {
            settings.selected = settings.selected == 11 ? 0 : settings.selected + 1;
          } else if (key == SDLK_LEFT && settings.selected == 1 &&
                     settings.save_slot > 0) {
            settings.save_slot--;
            settings.dirty = 1;
          } else if (key == SDLK_RIGHT && settings.selected == 1 &&
                     settings.save_slot < 4) {
            settings.save_slot++;
            settings.dirty = 1;
          } else if (key == SDLK_LEFT && settings.selected >= 2 && settings.selected <= 3) {
            settings.state_slot = settings.state_slot == 0 ? 5 : settings.state_slot - 1;
            settings.dirty = 1;
          } else if (key == SDLK_RIGHT && settings.selected >= 2 && settings.selected <= 3) {
            settings.state_slot = settings.state_slot == 5 ? 0 : settings.state_slot + 1;
            settings.dirty = 1;
          } else if (key == SDLK_LEFT && settings.selected == 4 && audio_context.volume >= 10) {
            audio_context.volume -= 10;
            settings.dirty = 1;
          } else if (key == SDLK_RIGHT && settings.selected == 4 && audio_context.volume <= 90) {
            audio_context.volume += 10;
            settings.dirty = 1;
          } else if (key == SDLK_LEFT && settings.selected == 5) {
            settings.palette = (settings.palette + 2) % 3;
            settings.dirty = 1;
          } else if (key == SDLK_RIGHT && settings.selected == 5) {
            settings.palette = (settings.palette + 1) % 3;
            settings.dirty = 1;
          } else if (key == SDLK_LEFT && settings.selected == 6 && settings.speed > 0) {
            settings.speed--;
            settings.dirty = 1;
          } else if (key == SDLK_RIGHT && settings.selected == 6 && settings.speed < 4) {
            settings.speed++;
            settings.dirty = 1;
          } else if (key == SDLK_LEFT && settings.selected == 7 && settings.autosave > 0) {
            settings.autosave--;
            settings.dirty = 1;
          } else if (key == SDLK_RIGHT && settings.selected == 7 && settings.autosave < 6) {
            settings.autosave++;
            settings.dirty = 1;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 0) {
            settings.browser = 1;
            settings.rom_selected = 0;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 1) {
            settings.confirm_action = 1;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 2) {
            settings.confirm_action = 2;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 3) {
            settings.confirm_action = 3;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 5) {
            settings.palette = (settings.palette + 1) % 3;
            settings.dirty = 1;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 6) {
            settings.speed = (settings.speed + 1) % 5;
            settings.dirty = 1;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 7) {
            settings.autosave = (settings.autosave + 1) % 7;
            settings.dirty = 1;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 8) {
            settings.remapping = 0;
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 9) {
            gb_reset(gb);
            settings.open = 0;
            SDL_StopTextInput();
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 10) {
            settings.open = 0;
            SDL_StopTextInput();
          } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && settings.selected == 11) {
            running = 0;
          }
          if (settings.dirty) {
            settings.dirty = 0;
            persist_config(main_config_path, config_path, path, &settings,
                           audio_context.volume, keys);
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
      if (debug_overlay && event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT) {
        uint16_t address;
        if (debug_search_button_at(event.button.x, event.button.y, 0)) {
          debug_view.searching = 1;
          debug_view.search_refine = 0;
          debug_view.search_len = 0;
          debug_view.search[0] = '\0';
          continue;
        }
        if (debug_view.match_count &&
            debug_search_button_at(event.button.x, event.button.y, 1)) {
          debug_view.searching = 1;
          debug_view.search_refine = 1;
          debug_view.search_len = 0;
          debug_view.search[0] = '\0';
          continue;
        }
        if (debug_view.match_count &&
            debug_search_button_at(event.button.x, event.button.y, 2)) {
          debug_next_match(&debug_view);
          continue;
        }
        if (debug_view.match_count &&
            debug_search_button_at(event.button.x, event.button.y, 3)) {
          debug_scan_changed(gb, &debug_view, 0);
          continue;
        }
        if (debug_view.match_count &&
            debug_search_button_at(event.button.x, event.button.y, 4)) {
          debug_scan_changed(gb, &debug_view, 1);
          continue;
        }
        if (debug_result_at(&debug_view, event.button.x, event.button.y,
                            &address)) {
          debug_view.selected_address = address;
          debug_view.selected = 1;
          debug_view.base = (uint16_t)(address & 0xfff0u);
          debug_view.edit_len = 0;
          debug_view.edit[0] = '\0';
          continue;
        }
        if (debug_memory_at(event.button.x, event.button.y, debug_view.base,
                            &address)) {
          debug_view.selected_address = address;
          debug_view.selected = 1;
          debug_view.edit_len = 0;
          debug_view.edit[0] = '\0';
        }
        continue;
      }
      if (debug_overlay && event.type == SDL_KEYDOWN) {
        SDL_Keycode key = event.key.keysym.sym;
        if (key == SDLK_ESCAPE) {
          debug_overlay = 0;
          paused = debug;
        } else if (key == SDLK_s && !debug_view.searching) {
          gb_dbg_step(gb);
        } else if (key == SDLK_PAGEUP && !debug_view.searching) {
          debug_view.base = (uint16_t)(debug_view.base - 128);
        } else if (key == SDLK_PAGEDOWN && !debug_view.searching) {
          debug_view.base = (uint16_t)(debug_view.base + 128);
        } else if (key == SDLK_n && !debug_view.searching) {
          debug_next_match(&debug_view);
        } else if (key == SDLK_SLASH && !debug_view.edit_len) {
          debug_view.searching = 1;
          debug_view.search_len = 0;
          debug_view.search[0] = '\0';
          } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
          if (debug_view.searching && debug_view.search_len) {
            debug_scan_value(gb, &debug_view);
          } else if (debug_view.selected && debug_view.edit_len == 2) {
            unsigned value = (unsigned)(hex_char_value(debug_view.edit[0]) * 16 +
                                        hex_char_value(debug_view.edit[1]));
            gb_dbg_write(gb, debug_view.selected_address, (uint8_t)value);
            debug_view.edit_len = 0;
            debug_view.edit[0] = '\0';
          }
        } else if (key == SDLK_BACKSPACE) {
          if (debug_view.searching && debug_view.search_len) {
            debug_view.search[--debug_view.search_len] = '\0';
          } else if (debug_view.edit_len) {
            debug_view.edit[--debug_view.edit_len] = '\0';
          }
        } else if (key == SDLK_ESCAPE) {
          debug_view.searching = 0;
          debug_view.search_len = 0;
          debug_view.edit_len = 0;
        } else {
          debug_edit_key(&debug_view, key);
        }
        continue;
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
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F5) {
        char slot_path[4096];
        if (state_slot_path(state_path, settings.state_slot, slot_path,
                            sizeof slot_path) == 0)
          settings.state_error = save_state(gb, slot_path, state, state_size) != 0;
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F8) {
        char slot_path[4096];
          if (state_slot_path(state_path, settings.state_slot, slot_path,
                              sizeof slot_path) == 0) {
          settings.state_error =
              load_state_file(gb, slot_path, state_size) != 0;
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
    unsigned emulated_frames = 0;
    if (!paused && !settings.open) {
      unsigned frames = (unsigned)settings.speed + 1;
      for (unsigned i = 0; i < frames; i++) {
        gb_run_frame(gb);
        emulated_frames++;
      }
    }
    if (emulated_frames && (save_timer += emulated_frames) >= 300) {
      char slot_path[4096];
      if (save_slot_path(save_path, settings.save_slot, slot_path,
                         sizeof slot_path) == 0)
        save_ram(gb, slot_path);
      save_timer %= 300;
    }
    static const unsigned autosave_frames[] = {0, 300, 600, 1800, 3600,
                                                18000, 108000};
    if (emulated_frames && settings.autosave &&
        (autosave_timer += emulated_frames) >= autosave_frames[settings.autosave]) {
      char auto_path[4096];
      if (state_slot_path(state_path, 5, auto_path, sizeof auto_path) == 0)
        settings.state_error = save_state(gb, auto_path, state, state_size) != 0;
      autosave_timer = 0;
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
      draw_debug_overlay(renderer, gb, &debug_view);
    draw_controller(renderer, buttons, settings.remapping);
    if (settings.open)
      draw_settings(renderer, &settings, audio_context.volume, save_path,
                    state_path, keys);
    SDL_RenderPresent(renderer);
    SDL_Delay(16 / ((unsigned)settings.speed + 1));
  }
#ifdef GB_ENABLE_TUI
  if (debug)
    endwin();
#endif
  {
    char slot_path[4096];
    if (save_slot_path(save_path, settings.save_slot, slot_path,
                       sizeof slot_path) == 0)
      save_ram(gb, slot_path);
  }
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
