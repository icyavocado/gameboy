#include "gb.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/* Single browser instance. The JS glue drives one frame per
   requestAnimationFrame and copies the framebuffer to a canvas. */
static gb_t *browser_gb;

/* One second of 48 kHz stereo lookahead for the Web Audio pump. Frames
   the core produces while muted (callback unset) are simply dropped. */
#define AUDIO_CAPACITY_FRAMES 48000u
static int16_t *audio_buf;
static size_t audio_start;
static size_t audio_count;

static void audio_sink(void *user, const int16_t *stereo, size_t frames) {
  (void)user;
  if (!audio_buf || !stereo)
    return;
  if (frames >= AUDIO_CAPACITY_FRAMES) {
    stereo += (frames - AUDIO_CAPACITY_FRAMES) * 2;
    frames = AUDIO_CAPACITY_FRAMES;
    audio_start = 0;
    audio_count = 0;
  }
  while (audio_count + frames > AUDIO_CAPACITY_FRAMES) {
    size_t drop = audio_count + frames - AUDIO_CAPACITY_FRAMES;
    audio_start = (audio_start + drop) % AUDIO_CAPACITY_FRAMES;
    audio_count -= drop;
  }
  for (size_t i = 0; i < frames * 2; i++) {
    audio_buf[(audio_start + audio_count * 2 + i) % (AUDIO_CAPACITY_FRAMES * 2)] =
        stereo[i];
  }
  audio_count += frames;
}

int EMSCRIPTEN_KEEPALIVE wasm_boot(void) {
  if (browser_gb)
    return 0;
  browser_gb = gb_create();
  if (!browser_gb)
    return -1;
  audio_buf = malloc(AUDIO_CAPACITY_FRAMES * 2 * sizeof *audio_buf);
  if (!audio_buf) {
    gb_destroy(browser_gb);
    browser_gb = NULL;
    return -1;
  }
  audio_start = audio_count = 0;
  gb_set_audio_callback(browser_gb, audio_sink, NULL);
  return 0;
}

void EMSCRIPTEN_KEEPALIVE wasm_free(void) {
  gb_destroy(browser_gb);
  browser_gb = NULL;
  free(audio_buf);
  audio_buf = NULL;
  audio_start = audio_count = 0;
}

int EMSCRIPTEN_KEEPALIVE wasm_load(const uint8_t *data, size_t size) {
  if (!browser_gb || !data)
    return -1;
  return gb_load_rom(browser_gb, data, size);
}

int EMSCRIPTEN_KEEPALIVE wasm_logo_valid(void) {
  return browser_gb && gb_rom_logo_valid(browser_gb) ? 1 : 0;
}

void EMSCRIPTEN_KEEPALIVE wasm_run_frame(void) {
  if (browser_gb)
    gb_run_frame(browser_gb);
}

const uint32_t EMSCRIPTEN_KEEPALIVE *wasm_framebuffer(void) {
  return browser_gb ? gb_framebuffer(browser_gb) : NULL;
}

void EMSCRIPTEN_KEEPALIVE wasm_set_input(uint8_t buttons) {
  if (browser_gb)
    gb_set_input(browser_gb, buttons);
}

size_t EMSCRIPTEN_KEEPALIVE wasm_save_size(void) {
  return browser_gb ? gb_save_ram_size(browser_gb) : 0;
}

size_t EMSCRIPTEN_KEEPALIVE wasm_save(uint8_t *out) {
  if (!browser_gb || !out)
    return 0;
  return gb_save_ram(browser_gb, out);
}

int EMSCRIPTEN_KEEPALIVE wasm_load_save(const uint8_t *data, size_t size) {
  if (!browser_gb || !data)
    return -1;
  return gb_load_ram(browser_gb, data, size);
}

size_t EMSCRIPTEN_KEEPALIVE wasm_state_size(void) {
  return browser_gb ? gb_save_state_size(browser_gb) : 0;
}

size_t EMSCRIPTEN_KEEPALIVE wasm_state_save(uint8_t *out) {
  if (!browser_gb || !out)
    return 0;
  return gb_save_state(browser_gb, out);
}

int EMSCRIPTEN_KEEPALIVE wasm_state_load(const uint8_t *data, size_t size) {
  if (!browser_gb || !data)
    return -1;
  return gb_load_state(browser_gb, data, size);
}

void EMSCRIPTEN_KEEPALIVE wasm_reset(void) {
  if (browser_gb)
    gb_reset(browser_gb);
}

size_t EMSCRIPTEN_KEEPALIVE wasm_audio_available(void) {
  return audio_count;
}

const int16_t EMSCRIPTEN_KEEPALIVE *wasm_audio_ptr(void) {
  return audio_buf ? audio_buf + audio_start * 2 : NULL;
}

void EMSCRIPTEN_KEEPALIVE wasm_audio_consume(size_t frames) {
  if (frames > audio_count)
    frames = audio_count;
  audio_start = (audio_start + frames) % AUDIO_CAPACITY_FRAMES;
  audio_count -= frames;
  /* Keep the unread run contiguous so one pointer read suffices. */
  if (audio_count && audio_start + audio_count > AUDIO_CAPACITY_FRAMES) {
    size_t first = AUDIO_CAPACITY_FRAMES - audio_start;
    memmove(audio_buf + first * 2, audio_buf,
            (audio_count - first) * 2 * sizeof *audio_buf);
    memmove(audio_buf, audio_buf + audio_start * 2, first * 2 * sizeof *audio_buf);
    audio_start = 0;
  } else if (!audio_count) {
    audio_start = 0;
  }
}

/* JS drives frames via requestAnimationFrame; main only exists to satisfy
   the Emscripten linker. Native builds compile this file as a stub. */
int main(void) {
  return 0;
}
