#include "gb.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  FILE *stream;
  char recent[6];
  size_t length;
  int failed;
  int passed;
  const char *expected;
  size_t expected_length;
  char *output;
  size_t output_length;
} serial_state_t;

static void serial(void *user, uint8_t out, uint8_t *in) {
  serial_state_t *state = user;
  fputc(out, state->stream);
  if (state->length == sizeof state->recent)
    memmove(state->recent, state->recent + 1, sizeof state->recent - 1);
  state->recent[state->length < sizeof state->recent ? state->length++ : sizeof state->recent - 1] = (char)out;
  state->failed = state->length == sizeof state->recent &&
                  memcmp(state->recent, "Failed", sizeof state->recent) == 0;
  if (state->length == sizeof state->recent &&
      memcmp(state->recent, "Passed", sizeof state->recent) == 0)
    state->passed = 1;
  if (state->expected && state->output_length < 4096)
    state->output[state->output_length++] = (char)out;
  *in = 0xff;
}

static int contains(const char *data, size_t length, const char *needle) {
  size_t n = strlen(needle);
  if (!n)
    return 1;
  for (size_t i = 0; i + n <= length; i++)
    if (memcmp(data + i, needle, n) == 0)
      return 1;
  return 0;
}
static int parse_number(const char *text, unsigned long max, unsigned long *value) {
  char *end = NULL;
  unsigned long parsed;
  errno = 0;
  parsed = strtoul(text, &end, 0);
  if (errno == ERANGE || end == text || *end != '\0' || parsed > max)
    return -1;
  *value = parsed;
  return 0;
}

int main(int argc, char **argv) {
  const char *expected = NULL;
  int require_pass = 0;
  int require_hram = 0;
  int require_regs = 0;
  int require_memory = 0;
  uint16_t memory_address = 0;
  uint8_t memory_value = 0;
  uint16_t hram_address = 0;
  uint8_t hram_value = 0;
  int frames_arg = 0;
  if (argc < 2) {
    fprintf(stderr, "usage: %s ROM [frames] [--expect TEXT] [--require-pass] "
                    "[--require-hram ADDRESS VALUE]\n", argv[0]);
    return 2;
  }
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc)
      expected = argv[++i];
    else if (strcmp(argv[i], "--require-pass") == 0)
      require_pass = 1;
    else if (strcmp(argv[i], "--require-hram") == 0 && i + 2 < argc) {
      unsigned long address, value;
      if (parse_number(argv[++i], 0xffff, &address) ||
          parse_number(argv[++i], 0xff, &value))
        return 2;
      if (address < 0xff80 || address > 0xfffe)
        return 2;
      hram_address = (uint16_t)address;
      hram_value = (uint8_t)value;
      require_hram = 1;
    }
    else if (strcmp(argv[i], "--require-regs") == 0) {
      require_regs = 1;
    }
    else if (strcmp(argv[i], "--require-memory") == 0 && i + 2 < argc) {
      unsigned long address, value;
      if (parse_number(argv[++i], 0xffff, &address) ||
          parse_number(argv[++i], 0xff, &value))
        return 2;
      memory_address = (uint16_t)address;
      memory_value = (uint8_t)value;
      require_memory = 1;
    }
    else if (!frames_arg)
      frames_arg = i;
    else {
      fprintf(stderr, "usage: %s ROM [frames] [--expect TEXT] [--require-pass] "
                      "[--require-hram ADDRESS VALUE]\n", argv[0]);
      return 2;
    }
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) return 1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
  long length = ftell(f);
  if (length < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 1; }
  size_t n = (size_t)length;
  uint8_t *rom = malloc(n);
  if (!rom) { fclose(f); return 1; }
  if (fread(rom, 1, n, f) != n) { free(rom); fclose(f); return 1; }
  fclose(f);
  char *end = NULL;
  errno = 0;
  unsigned long parsed = frames_arg ? strtoul(argv[frames_arg], &end, 10) : 1;
  if (frames_arg && (argv[frames_arg][0] == '-' || end == argv[frames_arg] || *end != '\0' ||
                    errno == ERANGE || parsed > UINT_MAX)) {
    free(rom);
    return 2;
  }
  unsigned frames = (unsigned)parsed;
  gb_t *g = gb_create();
  int rc = g ? gb_load_rom(g, rom, n) : -1;
  free(rom);
  if (rc) { gb_destroy(g); return 1; }
  char serial_output[4096] = {0};
  serial_state_t output = {stdout, {0}, 0, 0, 0, expected,
                            expected ? strlen(expected) : 0, serial_output, 0};
  gb_set_serial_callback(g, serial, &output);
  int fingerprint_seen = 0;
  while (frames--) {
    gb_run_frame(g);
    if (require_regs) {
      gb_regs_t current;
      gb_dbg_regs(g, &current);
      fingerprint_seen = (current.bc == 0x0305 && current.de == 0x080d &&
                          current.hl == 0x1522 &&
                          gb_dbg_read(g, current.pc) == 0x40);
      if (fingerprint_seen)
        break;
    }
  }
  int hram_ok = !require_hram || gb_dbg_read(g, hram_address) == hram_value;
  gb_regs_t regs;
  gb_dbg_regs(g, &regs);
  int regs_ok = !require_regs || fingerprint_seen ||
                ((regs.bc >> 8) == 3 && (uint8_t)regs.bc == 5 &&
                 (regs.de >> 8) == 8 && (uint8_t)regs.de == 13 &&
                 (regs.hl >> 8) == 21 && (uint8_t)regs.hl == 34);
  int memory_ok = !require_memory || gb_dbg_read(g, memory_address) == memory_value;
  gb_destroy(g);
  if (output.failed || (require_pass && !output.passed) ||
      (expected && !contains(serial_output, output.output_length, expected)) ||
      !hram_ok || !regs_ok || !memory_ok)
    return 1;
  return 0;
}
