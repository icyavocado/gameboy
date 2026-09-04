#include "gb.h"
#include "utest.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static gb_t *load(const uint8_t *code, size_t length) {
  static uint8_t rom[0x10000];
  memset(rom, 0, sizeof rom);
  rom[0x147] = 0;
  memcpy(rom + 0x100, code, length);
  gb_t *g = gb_create();
  ASSERT_TRUE(g);
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  return g;
}

static void step(gb_t *g, unsigned count) {
  while (count--)
    gb_dbg_step(g);
}

static gb_regs_t regs(gb_t *g) {
  gb_regs_t r;
  gb_dbg_regs(g, &r);
  return r;
}

static unsigned serial_calls;
static uint8_t serial_received;
static void serial_callback(void *unused, uint8_t out, uint8_t *in) {
  (void)unused;
  serial_calls++;
  serial_received = out;
  *in = 0xa5;
}

UTEST(core, immediate_and_register_loads) {
  static const uint8_t code[] = {
      0x06, 0x12, 0x0e, 0x34, 0x16, 0x56, 0x1e, 0x78,
      0x26, 0x9a, 0x2e, 0xbc, 0x7d, 0x76};
  gb_t *g = load(code, sizeof code);
  step(g, 8);
  gb_regs_t r = regs(g);
  ASSERT_EQ(r.af >> 8, 0xbc);
  ASSERT_EQ(r.bc, 0x1234);
  ASSERT_EQ(r.de, 0x5678);
  ASSERT_EQ(r.hl, 0x9abc);
  ASSERT_TRUE(r.halted);
  gb_destroy(g);
}

UTEST(core, alu_flags_and_daa) {
  static const uint8_t code[] = {
      0x3e, 0x0f, 0xc6, 0x01, 0x27, 0x3e, 0xff, 0xd6, 0x01, 0x27, 0x76};
  gb_t *g = load(code, sizeof code);
  step(g, 3);
  gb_regs_t r = regs(g);
  ASSERT_EQ(r.af, 0x1600);
  step(g, 4);
  r = regs(g);
  ASSERT_EQ(r.af, 0xfe40);
  gb_destroy(g);
}

UTEST(core, cb_bit_rotate_set_res) {
  static const uint8_t code[] = {
      0x3e, 0x81, 0xcb, 0x07, 0xcb, 0x47, 0xcb, 0x87, 0xcb, 0xc7, 0x76};
  gb_t *g = load(code, sizeof code);
  step(g, 5);
  gb_regs_t r = regs(g);
  ASSERT_EQ(r.af >> 8, 0x03);
  ASSERT_EQ(r.af & 0xf0, 0x30);
  gb_destroy(g);
}

UTEST(core, jumps_call_ret_and_stack) {
  static const uint8_t code[] = {
      0xcd, 0x08, 0x01, 0x18, 0x03, 0x00, 0x00, 0x00,
      0x3e, 0x42, 0xc9, 0x76};
  gb_t *g = load(code, sizeof code);
  ASSERT_EQ(gb_dbg_step(g), 24);
  ASSERT_EQ(regs(g).pc, 0x108);
  ASSERT_EQ(gb_dbg_step(g), 8);
  ASSERT_EQ(gb_dbg_step(g), 16);
  ASSERT_EQ(regs(g).pc, 0x103);
  ASSERT_EQ(gb_dbg_step(g), 12);
  ASSERT_EQ(regs(g).pc, 0x108);
  gb_destroy(g);
}

UTEST(core, ei_delay_interrupt_and_halt) {
  static const uint8_t code[] = {0xfb, 0x00, 0x76};
  gb_t *g = load(code, sizeof code);
  gb_dbg_write(g, 0xffff, 1);
  gb_dbg_write(g, 0xff0f, 1);
  ASSERT_TRUE(!regs(g).ime);
  gb_dbg_step(g);
  ASSERT_TRUE(!regs(g).ime);
  gb_dbg_step(g);
  ASSERT_TRUE(regs(g).ime);
  gb_dbg_step(g);
  ASSERT_EQ(regs(g).pc, 0x40);
  ASSERT_TRUE(!regs(g).halted);
  gb_destroy(g);

  g = load((const uint8_t[]){0x76}, 1);
  gb_dbg_step(g);
  ASSERT_TRUE(regs(g).halted);
  ASSERT_EQ(gb_dbg_step(g), 4);
  gb_destroy(g);
}

UTEST(core, timer_overflow_and_reset_state) {
  gb_t *g = load((const uint8_t[]){0x00}, 1);
  gb_dbg_write(g, 0xff06, 0x42);
  gb_dbg_write(g, 0xff05, 0xff);
  gb_dbg_write(g, 0xff07, 0x05);
  step(g, 4);
  ASSERT_EQ(gb_dbg_read(g, 0xff05), 0x42);
  ASSERT_EQ(gb_dbg_read(g, 0xff0f) & 4, 4);
  gb_dbg_write(g, 0xffff, 0xff);
  gb_reset(g);
  ASSERT_EQ(gb_dbg_read(g, 0xff05), 0);
  ASSERT_EQ(gb_dbg_read(g, 0xff0f), 0);
  ASSERT_EQ(gb_dbg_read(g, 0xffff), 0);
  gb_destroy(g);
}

UTEST(core, joyp_selection_and_serial_callback) {
  gb_t *g = load((const uint8_t[]){0x00}, 1);
  gb_set_input(g, 0x21);
  gb_dbg_write(g, 0xff00, 0x20);
  ASSERT_EQ(gb_dbg_read(g, 0xff00), 0xed);
  gb_dbg_write(g, 0xff00, 0x10);
  ASSERT_EQ(gb_dbg_read(g, 0xff00), 0xde);
  serial_calls = serial_received = 0;
  gb_set_serial_callback(g, serial_callback, NULL);
  gb_dbg_write(g, 0xff01, 'X');
  gb_dbg_write(g, 0xff02, 0x81);
  ASSERT_EQ(serial_calls, 1);
  ASSERT_EQ(serial_received, 'X');
  ASSERT_EQ(gb_dbg_read(g, 0xff01), 0xa5);
  ASSERT_EQ(gb_dbg_read(g, 0xff0f) & 8, 8);
  gb_destroy(g);
}

UTEST(core, mbc1_bank_switching) {
  static uint8_t rom[0x10000];
  memset(rom, 0, sizeof rom);
  rom[0x147] = 1;
  rom[0x149] = 2;
  rom[0x4000] = 1;
  rom[0x8000] = 2;
  rom[0xc000] = 3;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  ASSERT_EQ(gb_dbg_read(g, 0x4000), 1);
  gb_dbg_write(g, 0x2000, 2);
  ASSERT_EQ(gb_dbg_read(g, 0x4000), 2);
  gb_dbg_write(g, 0x2000, 3);
  ASSERT_EQ(gb_dbg_read(g, 0x4000), 3);
  gb_destroy(g);
}

UTEST(core, wram_echo) {
  gb_t *g = load((const uint8_t[]){0x00}, 1);
  gb_dbg_write(g, 0xc123, 0x5a);
  ASSERT_EQ(gb_dbg_read(g, 0xe123), 0x5a);
  gb_dbg_write(g, 0xe123, 0xa5);
  ASSERT_EQ(gb_dbg_read(g, 0xc123), 0xa5);
  gb_destroy(g);
}

UTEST(ppu, background_tile) {
  gb_t *g = load((const uint8_t[]){0x76}, 1);
  gb_dbg_write(g, 0x8010, 0x80);
  gb_dbg_write(g, 0x8011, 0x00);
  gb_dbg_write(g, 0x9800, 1);
  gb_dbg_write(g, 0xff47, 0xe4);
  gb_run_frame(g);
  ASSERT_EQ(gb_framebuffer(g)[0], 0xffa8a8a8);
  ASSERT_EQ(gb_framebuffer(g)[1], 0xfff8f8f8);
  gb_destroy(g);
}

UTEST(ppu, lcd_timing_and_interrupts) {
  gb_t *g = load((const uint8_t[]){0x76}, 1);
  gb_dbg_write(g, 0xff41, 0x60);
  gb_dbg_write(g, 0xff45, 1);
  gb_dbg_write(g, 0xffff, 2);
  step(g, 20);
  ASSERT_EQ(gb_dbg_read(g, 0xff44), 0);
  ASSERT_EQ(gb_dbg_read(g, 0xff41) & 3, 3);
  step(g, 95);
  ASSERT_EQ(gb_dbg_read(g, 0xff44), 1);
  ASSERT_TRUE(gb_dbg_read(g, 0xff0f) & 2);
  gb_dbg_write(g, 0xff0f, 0);
  gb_reset(g);
  gb_run_frame(g);
  ASSERT_EQ(gb_dbg_read(g, 0xff44), 0);
  ASSERT_TRUE(gb_dbg_read(g, 0xff0f) & 1);
  gb_destroy(g);
}

UTEST(ppu, sprite_rendering) {
  gb_t *g = load((const uint8_t[]){0x76}, 1);
  gb_dbg_write(g, 0x8020, 0x80);
  gb_dbg_write(g, 0x8021, 0x00);
  gb_dbg_write(g, 0xfe00, 16);
  gb_dbg_write(g, 0xfe01, 8);
  gb_dbg_write(g, 0xfe02, 2);
  gb_dbg_write(g, 0xfe03, 0);
  gb_dbg_write(g, 0xff48, 0xe4);
  gb_dbg_write(g, 0xff40, 0x93);
  gb_run_frame(g);
  ASSERT_EQ(gb_framebuffer(g)[0], 0xffa8a8a8);
  gb_destroy(g);
}

UTEST(core, battery_ram_round_trip) {
  static uint8_t rom[0x8000];
  uint8_t save[0x2000];
  memset(rom, 0, sizeof rom);
  rom[0x147] = 3;
  rom[0x149] = 2;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  ASSERT_EQ(gb_save_ram_size(g), 0x2000);
  gb_dbg_write(g, 0x0000, 0x0a);
  gb_dbg_write(g, 0xa123, 0x5a);
  ASSERT_EQ(gb_save_ram(g, save), sizeof save);
  gb_dbg_write(g, 0xa123, 0xa5);
  ASSERT_EQ(gb_load_ram(g, save, sizeof save), 0);
  ASSERT_EQ(gb_dbg_read(g, 0xa123), 0x5a);
  gb_destroy(g);
}

UTEST(core, save_state_round_trip) {
  gb_t *g = load((const uint8_t[]){0x06, 0x42, 0x76}, 3);
  gb_dbg_write(g, 0x8000, 0x9a);
  gb_dbg_step(g);
  size_t n = gb_save_state_size(g);
  uint8_t *state = malloc(n);
  ASSERT_TRUE(state);
  ASSERT_EQ(gb_save_state(g, state), n);
  gb_dbg_write(g, 0x8000, 0x12);
  gb_dbg_step(g);
  ASSERT_EQ(gb_load_state(g, state, n), 0);
  ASSERT_EQ(gb_dbg_read(g, 0x8000), 0x9a);
  ASSERT_EQ(regs(g).bc >> 8, 0x42);
  free(state);
  gb_destroy(g);
}

UTEST(core, extended_control_and_stack_opcodes) {
  static const uint8_t code[] = {
      0x01, 0x34, 0x12, 0x21, 0x00, 0x10, 0x09, 0x0b, 0xc5,
      0xd1, 0x3e, 0x80, 0x0f, 0x17, 0x1f, 0x76};
  gb_t *g = load(code, sizeof code);
  step(g, 11);
  gb_regs_t r = regs(g);
  ASSERT_EQ(r.hl, 0x2234);
  ASSERT_EQ(r.bc, 0x1233);
  ASSERT_EQ(r.de, 0x1233);
  ASSERT_EQ(r.af >> 8, 0x40);
  ASSERT_TRUE(r.halted);
  gb_destroy(g);
}

UTEST(core, conditional_relative_and_signed_stack_arithmetic) {
  static const uint8_t code[] = {
      0x3e, 0x01, 0x06, 0x01, 0x90, 0x30, 0x02, 0x3e, 0xff,
      0x20, 0x02, 0x3e, 0x42, 0xe8, 0x02, 0xf8, 0xfe, 0x76};
  gb_t *g = load(code, sizeof code);
  step(g, 9);
  gb_regs_t r = regs(g);
  ASSERT_EQ(r.af >> 8, 0x42);
  ASSERT_EQ(r.hl, 0xfffe);
  ASSERT_EQ(r.sp, 0x0000);
  ASSERT_TRUE(r.halted);
  gb_destroy(g);
}

UTEST(core, oam_dma_transfer) {
  gb_t *g = load((const uint8_t[]){0x00}, 1);
  for (unsigned i = 0; i < 160; i++)
    gb_dbg_write(g, (uint16_t)(0xc000 + i), (uint8_t)(i ^ 0x5a));
  gb_dbg_write(g, 0xff46, 0xc0);
  for (unsigned i = 0; i < 160; i++)
    ASSERT_EQ(gb_dbg_step(g), 4);
  for (unsigned i = 0; i < 160; i++)
    ASSERT_EQ(gb_dbg_read(g, (uint16_t)(0xfe00 + i)), (uint8_t)(i ^ 0x5a));
  gb_destroy(g);
}

UTEST(core, timer_uses_divider_edges) {
  gb_t *g = load((const uint8_t[]){0x00}, 1);
  gb_dbg_write(g, 0xff05, 0);
  gb_dbg_write(g, 0xff07, 0x05);
  for (unsigned i = 0; i < 16; i++)
    gb_dbg_step(g);
  ASSERT_EQ(gb_dbg_read(g, 0xff05), 4);
  gb_dbg_write(g, 0xff04, 0);
  ASSERT_EQ(gb_dbg_read(g, 0xff04), 0);
  gb_destroy(g);
}

UTEST(ppu, stat_write_rechecks_coincidence) {
  gb_t *g = load((const uint8_t[]){0x00}, 1);
  gb_dbg_write(g, 0xff45, 0);
  gb_dbg_write(g, 0xff0f, 0);
  gb_dbg_write(g, 0xff41, 0x40);
  ASSERT_TRUE(gb_dbg_read(g, 0xff0f) & 2);
  gb_destroy(g);
}

int main(void) {
  core_immediate_and_register_loads();
  core_alu_flags_and_daa();
  core_cb_bit_rotate_set_res();
  core_jumps_call_ret_and_stack();
  core_ei_delay_interrupt_and_halt();
  core_timer_overflow_and_reset_state();
  core_joyp_selection_and_serial_callback();
  core_mbc1_bank_switching();
  core_wram_echo();
  ppu_background_tile();
  ppu_lcd_timing_and_interrupts();
  ppu_sprite_rendering();
  core_battery_ram_round_trip();
  core_save_state_round_trip();
  core_extended_control_and_stack_opcodes();
  core_conditional_relative_and_signed_stack_arithmetic();
  core_oam_dma_transfer();
  core_timer_uses_divider_edges();
  ppu_stat_write_rechecks_coincidence();
  puts("19 tests passed");
  return 0;
}
