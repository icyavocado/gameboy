#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) { fprintf(stderr, "usage: %s ROM [frames]\n", argv[0]); return 2; }
  FILE *f = fopen(argv[1], "rb"); if (!f) return 1; fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  uint8_t *rom = malloc((size_t)n); if (!rom || fread(rom, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
  gb_t *g = gb_create(); int rc = gb_load_rom(g, rom, (size_t)n); free(rom); if (rc) return 1;
  unsigned frames = argc == 3 ? (unsigned)strtoul(argv[2], NULL, 10) : 1;
  while (frames--) gb_run_frame(g);
  gb_destroy(g);
  return 0;
}
