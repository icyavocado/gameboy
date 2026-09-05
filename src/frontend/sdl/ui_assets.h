#ifndef GB_SDL_UI_ASSETS_H
#define GB_SDL_UI_ASSETS_H

#include <stdint.h>

typedef struct {
  uint8_t font[128][7];
  uint8_t arrow[4][7];
  uint8_t cog[7];
  uint8_t bug[7];
} gb_ui_assets_t;

int gb_ui_assets_load(gb_ui_assets_t *assets, const char *path);

#endif
