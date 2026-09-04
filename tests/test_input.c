#include "input.h"
#include "gb.h"
#include <assert.h>
#include <string.h>

static void check_button(gb_t *gb, uint8_t bit, uint8_t select, uint8_t expected) {
  gb_set_input(gb, bit);
  gb_dbg_write(gb, 0xff00, select);
  assert((gb_dbg_read(gb, 0xff00) & 0x0f) == expected);
}

int main(void) {
  static uint8_t rom[0x10000];
  gb_t *gb = gb_create();
  assert(gb);
  assert(gb_load_rom(gb, rom, sizeof rom) == 0);
  assert(gb_sdl_key_button(SDLK_RIGHT) == (1 << 0));
  assert(gb_sdl_key_button(SDLK_LEFT) == (1 << 1));
  assert(gb_sdl_key_button(SDLK_UP) == (1 << 2));
  assert(gb_sdl_key_button(SDLK_DOWN) == (1 << 3));
  assert(gb_sdl_key_button(SDLK_z) == (1 << 4));
  assert(gb_sdl_key_button(SDLK_x) == (1 << 5));
  assert(gb_sdl_key_button(SDLK_LSHIFT) == (1 << 6));
  assert(gb_sdl_key_button(SDLK_RSHIFT) == (1 << 6));
  assert(gb_sdl_key_button(SDLK_RETURN) == (1 << 7));
  assert(gb_sdl_key_button(SDLK_KP_ENTER) == (1 << 7));
  assert(gb_sdl_key_button(SDLK_s) == 0);
  check_button(gb, gb_sdl_key_button(SDLK_z), 0x10, 0x0e);
  check_button(gb, gb_sdl_key_button(SDLK_x), 0x10, 0x0d);
  check_button(gb, gb_sdl_key_button(SDLK_UP), 0x20, 0x0b);
  check_button(gb, gb_sdl_key_button(SDLK_DOWN), 0x20, 0x07);
  check_button(gb, gb_sdl_key_button(SDLK_LSHIFT), 0x10, 0x0b);
  check_button(gb, gb_sdl_key_button(SDLK_RETURN), 0x10, 0x07);
  gb_destroy(gb);
  return 0;
}
