#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *const autosave_names[7] = {"OFF", "5s", "10s", "30s",
                                       "1m",  "5m", "30m"};

static const struct {
  const char *name;
  SDL_Keycode key;
} config_keys[] = {
    {"SDLK_RIGHT", SDLK_RIGHT}, {"SDLK_LEFT", SDLK_LEFT},
    {"SDLK_UP", SDLK_UP},       {"SDLK_DOWN", SDLK_DOWN},
    {"SDLK_z", SDLK_z},         {"SDLK_x", SDLK_x},
    {"SDLK_LSHIFT", SDLK_LSHIFT}, {"SDLK_RSHIFT", SDLK_RSHIFT},
    {"SDLK_RETURN", SDLK_RETURN}, {"SDLK_KP_ENTER", SDLK_KP_ENTER},
    {"SDLK_SPACE", SDLK_SPACE}, {"SDLK_a", SDLK_a},
    {"SDLK_b", SDLK_b},
};

static char *trim_config(char *text) {
  char *end;
  while (isspace((unsigned char)*text)) text++;
  end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
  return text;
}

static SDL_Keycode config_key(const char *name) {
  for (size_t i = 0; i < sizeof config_keys / sizeof *config_keys; i++)
    if (strcasecmp(name, config_keys[i].name) == 0) return config_keys[i].key;
  return SDLK_UNKNOWN;
}

const char *config_key_name(SDL_Keycode key) {
  for (size_t i = 0; i < sizeof config_keys / sizeof *config_keys; i++)
    if (key == config_keys[i].key) return config_keys[i].name;
  return "SDLK_UNKNOWN";
}

static int config_autosave(const char *value) {
  for (int i = 0; i < 7; i++)
    if (strcasecmp(value, autosave_names[i]) == 0) return i;
  return -1;
}

static int config_speed(const char *value) {
  if (strcasecmp(value, "OFF") == 0 || strcmp(value, "1") == 0 ||
      strcmp(value, "1x") == 0) return 0;
  if (strcmp(value, "2") == 0 || strcasecmp(value, "2x") == 0) return 1;
  if (strcmp(value, "3") == 0 || strcasecmp(value, "3x") == 0) return 2;
  if (strcmp(value, "4") == 0 || strcasecmp(value, "4x") == 0) return 3;
  if (strcmp(value, "5") == 0 || strcasecmp(value, "5x") == 0) return 4;
  return -1;
}

void load_config(const char *filename, char *rom_path, size_t path_size,
                 settings_t *settings, unsigned *volume, SDL_Keycode keys[8],
                 int allow_rom) {
  FILE *file = fopen(filename, "r");
  char line[4096];
  if (!file) return;
  while (fgets(line, sizeof line, file)) {
    char *text = trim_config(line);
    char *value = strchr(text, ' ');
    if (!*text || *text == '#') continue;
    if (!value) continue;
    *value++ = '\0';
    value = trim_config(value);
    if (strcasecmp(text, "LOADROM") == 0 && allow_rom) {
      /* Prefer the path verbatim (it may be relative to CWD, as written by
         an earlier session); fall back to resolving it against the config
         file's directory. */
      FILE *probe = value[0] ? fopen(value, "rb") : NULL;
      if (probe) {
        fclose(probe);
        snprintf(rom_path, path_size, "%s", value);
      } else if (value[0] != '/' && value[0] != '\0') {
        const char *slash = strrchr(filename, '/');
        if (slash) {
          size_t dirlen = (size_t)(slash - filename);
          if (dirlen + 1 + strlen(value) < path_size) {
            memcpy(rom_path, filename, dirlen);
            rom_path[dirlen] = '/';
            strcpy(rom_path + dirlen + 1, value);
          } else {
            snprintf(rom_path, path_size, "%s", value);
          }
        } else {
          snprintf(rom_path, path_size, "%s", value);
        }
      } else {
        snprintf(rom_path, path_size, "%s", value);
      }
    } else if (strcasecmp(text, "SAVESLOT") == 0) {
      int slot = atoi(value);
      if (slot >= 1 && slot <= 5) settings->save_slot = (unsigned)slot - 1;
    } else if (strcasecmp(text, "STATE") == 0) {
      if (strcasecmp(value, "A") == 0) settings->state_slot = 5;
      else {
        int slot = atoi(value);
        if (slot >= 1 && slot <= 5) settings->state_slot = (unsigned)slot - 1;
      }
    } else if (strcasecmp(text, "VOLUME") == 0) {
      int level = atoi(value);
      if (level >= 0 && level <= 100) *volume = (unsigned)level;
    } else if (strcasecmp(text, "PALETTE") == 0) {
      int palette = atoi(value);
      if (palette >= 0 && palette <= 2) settings->palette = palette;
    } else if (strcasecmp(text, "SPEED") == 0) {
      int speed = config_speed(value);
      if (speed >= 0) settings->speed = speed;
    } else if (strcasecmp(text, "AUTOSAVE") == 0) {
      int autosave = config_autosave(value);
      if (autosave >= 0) settings->autosave = autosave;
    } else if (strcasecmp(text, "REMAPKEYS") == 0) {
      char *token = strtok(value, " \t");
      for (unsigned i = 0; i < 8 && token; i++) {
        SDL_Keycode key = config_key(token);
        if (key != SDLK_UNKNOWN) keys[i] = key;
        token = strtok(NULL, " \t");
      }
    }
  }
  fclose(file);
}

const char *state_config_value(const settings_t *settings) {
  static const char *const slots[] = {"1", "2", "3", "4", "5"};
  return settings->state_slot == 5 ? "A" : slots[settings->state_slot];
}

int write_config(const char *filename, const char *rom_path, int include_rom,
                 const settings_t *settings, unsigned volume,
                 const SDL_Keycode keys[8]) {
  char tmp[4096];
  FILE *file;
  if (snprintf(tmp, sizeof tmp, "%s.tmp", filename) >= (int)sizeof tmp)
    return -1;
  file = fopen(tmp, "w");
  if (!file) return -1;
  if (include_rom) fprintf(file, "LOADROM %s\n", rom_path);
  fprintf(file, "SAVESLOT %u\nSTATE %s\nVOLUME %u\nPALETTE %d\n",
          settings->save_slot + 1, state_config_value(settings), volume,
          settings->palette);
  fprintf(file, "SPEED %s\nAUTOSAVE %s\nREMAPKEYS",
          settings->speed == 0 ? "1x" : settings->speed == 1 ? "2x" :
          settings->speed == 2 ? "3x" : settings->speed == 3 ? "4x" : "5x",
          autosave_names[settings->autosave]);
  for (unsigned i = 0; i < 8; i++)
    fprintf(file, " %s", config_key_name(keys[i]));
  fprintf(file, "\n");
  if (fclose(file) != 0) {
    remove(tmp);
    return -1;
  }
  if (rename(tmp, filename) != 0) {
    remove(tmp);
    return -1;
  }
  return 0;
}

void config_paths(const char *rom_path, char *main_path, size_t main_size,
                  char *specific_path, size_t specific_size) {
  const char *slash = strrchr(rom_path, '/');
  if (slash)
    snprintf(main_path, main_size, "%.*s/gameboy.cfg", (int)(slash - rom_path),
             rom_path);
  else
    snprintf(main_path, main_size, "gameboy.cfg");
  snprintf(specific_path, specific_size, "%s.cfg", rom_path);
}

void persist_config(const char *main_path, const char *specific_path,
                    const char *rom_path, settings_t *settings,
                    unsigned volume, const SDL_Keycode keys[8]) {
  int main_ok = write_config(main_path, rom_path, 1, settings, volume, keys);
  int specific_ok =
      write_config(specific_path, rom_path, 0, settings, volume, keys);
  settings->config_error = (main_ok != 0 || specific_ok != 0);
}
