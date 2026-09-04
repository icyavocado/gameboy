#include "gb.h"
#include "utest.h"
#include <stdint.h>
UTEST(core, cpu_load_and_halt) {
  static uint8_t rom[0x8000]; rom[0x147] = 0; rom[0x100] = 0x3e; rom[0x101] = 0x42; rom[0x102] = 0x76;
  gb_t *g = gb_create(); ASSERT_TRUE(g); ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  ASSERT_EQ(gb_dbg_step(g), 8); uint16_t r[6]; gb_dbg_regs(g, r); ASSERT_EQ(r[0] >> 8, 0x42);
  ASSERT_EQ(gb_dbg_step(g), 4); ASSERT_EQ(gb_dbg_read(g, 0xff40), 0x91); gb_destroy(g);
}

UTEST_MAIN();
