#ifndef GB_SDL_CONFIG_H
#define GB_SDL_CONFIG_H

#include <SDL.h>
#include <stddef.h>

typedef struct {
  char *path;
  char title[17];
} rom_entry_t;

typedef struct {
  int open;
  int selected;
  int remapping;
  int palette;
  int speed;
  int browser;
  int confirm_load;
  int confirm_action;
  int autosave;
  int config_error;
  unsigned save_slot;
  unsigned state_slot;
  rom_entry_t *roms;
  size_t rom_count;
  size_t rom_selected;
} settings_t;

extern const char *const autosave_names[7];

const char *config_key_name(SDL_Keycode key);
const char *state_config_value(const settings_t *settings);
void load_config(const char *filename, char *rom_path, size_t path_size,
                 settings_t *settings, unsigned *volume, SDL_Keycode keys[8],
                 int allow_rom);
int write_config(const char *filename, const char *rom_path, int include_rom,
                 const settings_t *settings, unsigned volume,
                 const SDL_Keycode keys[8]);
void config_paths(const char *rom_path, char *main_path, size_t main_size,
                  char *specific_path, size_t specific_size);
void persist_config(const char *main_path, const char *specific_path,
                    const char *rom_path, settings_t *settings,
                    unsigned volume, const SDL_Keycode keys[8]);

#endif
