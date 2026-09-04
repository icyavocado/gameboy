#include "input.h"

uint8_t gb_sdl_key_button(SDL_Keycode key) {
  switch (key) {
  case SDLK_RIGHT: return 1 << 0;
  case SDLK_LEFT: return 1 << 1;
  case SDLK_UP: return 1 << 2;
  case SDLK_DOWN: return 1 << 3;
  case SDLK_z: return 1 << 4;
  case SDLK_x: return 1 << 5;
  case SDLK_LSHIFT:
  case SDLK_RSHIFT: return 1 << 6;
  case SDLK_RETURN:
  case SDLK_KP_ENTER: return 1 << 7;
  default: return 0;
  }
}
