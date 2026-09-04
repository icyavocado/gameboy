#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  long length;
  uint8_t *data;
  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    if (file)
      fclose(file);
    return NULL;
  }
  data = malloc((size_t)length);
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size = (size_t)length;
  return data;
}

static uint32_t framebuffer_hash(const gb_t *gb) {
  const uint32_t *pixels = gb_framebuffer(gb);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < 160u * 144u; i++) {
    hash ^= pixels[i];
    hash *= 16777619u;
  }
  return hash;
}

static void press(gb_t *gb, uint8_t button) {
  gb_set_input(gb, button);
  for (unsigned i = 0; i < 30; i++)
    gb_run_frame(gb);
  gb_set_input(gb, 0);
  gb_run_frame(gb);
}

int main(void) {
  size_t size;
  uint8_t *rom = read_file("tests/roms/tetris.gb", &size);
  gb_t *gb;
  uint32_t title, started;

  if (!rom)
    rom = read_file("../tests/roms/tetris.gb", &size);
  if (!rom) {
    puts("tetris ROM unavailable; gameplay test skipped");
    return 0;
  }
  gb = gb_create();
  if (!gb || gb_load_rom(gb, rom, size) != 0) {
    free(rom);
    gb_destroy(gb);
    return 1;
  }
  free(rom);

  gb_run_frame(gb);
  title = framebuffer_hash(gb);
  press(gb, 1 << 7);
  started = framebuffer_hash(gb);
  if (started == title) {
    fprintf(stderr, "Start did not change the game framebuffer\n");
    gb_destroy(gb);
    return 1;
  }
  for (unsigned i = 0; i < 30; i++)
    gb_run_frame(gb);
  press(gb, 1 << 0);
  press(gb, 1 << 4);
  puts("tetris gameplay reached the PPU framebuffer");
  gb_destroy(gb);
  return 0;
}
