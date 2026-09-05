#include "gb.h"
#include <limits.h>
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

typedef struct {
  size_t frames;
  uint64_t energy;
  uint32_t hash;
  unsigned changes;
  int16_t previous;
  int saw_positive;
  int saw_negative;
} audio_probe_t;

static void audio_probe(void *user, const int16_t *stereo, size_t frames) {
  audio_probe_t *probe = user;
  probe->frames += frames;
  for (size_t i = 0; i < frames * 2; i++) {
    int16_t sample = stereo[i];
    probe->energy += (uint64_t)(sample < 0 ? -sample : sample);
    probe->hash ^= (uint16_t)sample;
    probe->hash *= 16777619u;
    if (i && sample != probe->previous)
      probe->changes++;
    probe->previous = sample;
    probe->saw_positive |= sample > 0;
    probe->saw_negative |= sample < 0;
  }
}

static void audio_probe_reset(audio_probe_t *probe) {
  probe->frames = 0;
  probe->energy = 0;
  probe->hash = 2166136261u;
  probe->changes = 0;
  probe->previous = 0;
  probe->saw_positive = 0;
  probe->saw_negative = 0;
}

int main(void) {
  size_t size;
  uint8_t *rom = read_file("tests/roms/tetris.gb", &size);
  gb_t *gb;
  uint32_t title, started;
  audio_probe_t audio = {0, 0, 2166136261u, 0, 0, 0, 0};

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
  gb_set_audio_callback(gb, audio_probe, &audio);

  /* The copyright screen is silent for several seconds: channels are
     triggered with envelope volume 0 and period 0, which must stay mute. */
  for (unsigned i = 0; i < 360; i++)
    gb_run_frame(gb);
  title = framebuffer_hash(gb);
  if (audio.energy != 0) {
    fprintf(stderr, "Tetris produced startup audio: energy=%llu\n",
            (unsigned long long)audio.energy);
    gb_destroy(gb);
    return 1;
  }
  audio_probe_reset(&audio);
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
  if (audio.frames < 1000 || audio.energy == 0 || audio.changes < 100 ||
      audio.hash == 2166136261u || !audio.saw_positive || !audio.saw_negative) {
    fprintf(stderr, "Tetris audio frames=%zu energy=%llu changes=%u hash=%08x nr50=%02x nr51=%02x nr52=%02x n12=%02x n17=%02x n21=%02x n22=%02x\n",
            audio.frames, (unsigned long long)audio.energy, audio.changes,
            audio.hash, gb_dbg_read(gb, 0xff24), gb_dbg_read(gb, 0xff25),
             gb_dbg_read(gb, 0xff26), gb_dbg_read(gb, 0xff12),
            gb_dbg_read(gb, 0xff17), gb_dbg_read(gb, 0xff21),
            gb_dbg_read(gb, 0xff22));
    gb_destroy(gb);
    return 1;
  }
  puts("tetris gameplay reached the PPU framebuffer");
  gb_destroy(gb);
  return 0;
}
