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

int main(int argc, char **argv) {
  const char *expected = NULL;
  if (argc == 5 && strcmp(argv[3], "--expect") == 0)
    expected = argv[4];
  else if (argc < 2 || argc > 3) { fprintf(stderr, "usage: %s ROM [frames] [--expect TEXT]\n", argv[0]); return 2; }
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
  unsigned long parsed = argc == 3 ? strtoul(argv[2], &end, 10) : 1;
  if (argc == 3 && (argv[2][0] == '-' || end == argv[2] || *end != '\0' ||
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
  serial_state_t output = {stdout, {0}, 0, 0, expected,
                            expected ? strlen(expected) : 0, serial_output, 0};
  gb_set_serial_callback(g, serial, &output);
  while (frames--) gb_run_frame(g);
  gb_destroy(g);
  if (output.failed || (expected &&
                        !contains(serial_output, output.output_length, expected)))
    return 1;
  return 0;
}
