#include "gb.h"
#include <stddef.h>

int main(void) {
  /* The browser frontend will supply ROM loading and a main loop in Phase 8. */
  gb_t *gb = gb_create();
  gb_destroy(gb);
  return 0;
}
