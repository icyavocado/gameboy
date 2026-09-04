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
static unsigned audio_calls;
static size_t audio_frames;
static int16_t audio_peak;
static void audio_callback(void *unused, const int16_t *stereo, size_t frames) {
  (void)unused;
  audio_calls++;
  audio_frames += frames;
  for (size_t i = 0; i < frames * 2; i++)
    if (stereo[i] > audio_peak)
      audio_peak = stereo[i];
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

UTEST(core, compare_preserves_accumulator) {
  gb_t *g = load((const uint8_t[]){0x3e, 0x10, 0xfe, 0x10, 0xfe, 0x20, 0x76}, 7);
  step(g, 2);
  ASSERT_EQ(regs(g).af, 0x10c0);
  ASSERT_EQ(regs(g).pc, 0x104);
  gb_dbg_step(g);
  ASSERT_EQ(regs(g).af, 0x1050);
  gb_destroy(g);
}

UTEST(core, adc_uses_carry) {
  gb_t *g = load((const uint8_t[]){0x3e, 0xff, 0x37, 0xce, 0x00, 0x76}, 6);
  step(g, 4);
  ASSERT_EQ(regs(g).af, 0x00b0);
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

UTEST(core, debugger_pc_breakpoints) {
  gb_t *g = load((const uint8_t[]){0x00, 0x00, 0x76}, 3);
  gb_bp_t breakpoint = {GB_BP_PC, 0x101};
  int id = gb_dbg_add_bp(g, breakpoint);
  ASSERT_TRUE(id >= 0);
  gb_dbg_enable(g, true);
  ASSERT_EQ(gb_dbg_run_until_break(g), id);
  ASSERT_EQ(regs(g).pc, 0x101);
  ASSERT_EQ(gb_dbg_run_until_break(g), -1);
  ASSERT_EQ(regs(g).pc, 0x103);
  gb_dbg_del_bp(g, id);
  gb_destroy(g);
}

UTEST(core, optional_boot_rom_mapping) {
  static uint8_t boot[0x900];
  memset(boot, 0, sizeof boot);
  boot[0] = 0xa5;
  boot[0x200] = 0x5a;
  gb_t *g = load((const uint8_t[]){0x00}, 1);
  ASSERT_EQ(gb_load_boot_rom(g, boot, 0x200), -1);
  ASSERT_EQ(gb_load_boot_rom(g, boot, 0x100), 0);
  ASSERT_EQ(gb_dbg_read(g, 0), 0xa5);
  gb_dbg_write(g, 0xff50, 1);
  ASSERT_EQ(gb_dbg_read(g, 0), 0);
  ASSERT_EQ(gb_dbg_read(g, 0x100), 0);
  gb_destroy(g);

  static uint8_t cgb_rom[0x10000];
  memset(cgb_rom, 0, sizeof cgb_rom);
  cgb_rom[0x143] = 0xc0;
  g = gb_create();
  ASSERT_EQ(gb_load_rom(g, cgb_rom, sizeof cgb_rom), 0);
  ASSERT_EQ(gb_load_boot_rom(g, boot, sizeof boot), 0);
  ASSERT_EQ(gb_dbg_read(g, 0), 0xa5);
  ASSERT_EQ(gb_dbg_read(g, 0x200), 0x5a);
  gb_dbg_write(g, 0xff50, 1);
  ASSERT_EQ(gb_dbg_read(g, 0x200), 0);
  gb_destroy(g);
}

UTEST(core, debugger_disassembly) {
  gb_t *g = load((const uint8_t[]){0x3e, 0x42, 0xc3, 0x00, 0x02, 0xd3}, 6);
  char text[32];
  ASSERT_EQ(gb_dbg_disasm(g, 0x100, text, sizeof text), 2);
  ASSERT_TRUE(strcmp(text, "LD A,d8") == 0);
  ASSERT_EQ(gb_dbg_disasm(g, 0x102, text, sizeof text), 3);
  ASSERT_TRUE(strcmp(text, "JP $0200") == 0);
  ASSERT_EQ(gb_dbg_disasm(g, 0x105, text, sizeof text), 1);
  ASSERT_TRUE(strcmp(text, "DB $D3") == 0);
  gb_destroy(g);
}

UTEST(core, debugger_memory_watchpoints) {
  gb_t *g = load((const uint8_t[]){0x3e, 0x42, 0xea, 0x00, 0xc0, 0xfa, 0x00, 0xc0}, 8);
  int write_id = gb_dbg_add_bp(g, (gb_bp_t){GB_BP_WRITE, 0xc000});
  int read_id = gb_dbg_add_bp(g, (gb_bp_t){GB_BP_READ, 0xc000});
  gb_dbg_enable(g, true);
  ASSERT_EQ(gb_dbg_run_until_break(g), write_id);
  ASSERT_EQ(gb_dbg_read(g, 0xc000), 0x42);
  gb_dbg_del_bp(g, write_id);
  ASSERT_EQ(gb_dbg_run_until_break(g), read_id);
  gb_dbg_del_bp(g, read_id);
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
  gb_dbg_write(g, 0xff0f, 0);
  gb_dbg_write(g, 0xff00, 0x20);
  ASSERT_EQ(gb_dbg_read(g, 0xff00), 0xee);
  gb_dbg_write(g, 0xff00, 0x10);
  ASSERT_EQ(gb_dbg_read(g, 0xff00), 0xdd);
  gb_set_input(g, 0xf0);
  gb_dbg_write(g, 0xff00, 0x20);
  ASSERT_EQ(gb_dbg_read(g, 0xff00), 0xef);
  gb_dbg_write(g, 0xff00, 0x10);
  ASSERT_EQ(gb_dbg_read(g, 0xff00), 0xd0);
  gb_set_input(g, 0);
  gb_dbg_write(g, 0xff0f, 0);
  gb_dbg_write(g, 0xff00, 0x10);
  gb_set_input(g, 1 << 4);
  ASSERT_EQ(gb_dbg_read(g, 0xff0f) & 0x10, 0x10);
  gb_set_input(g, 0);
  gb_dbg_write(g, 0xff0f, 0);
  gb_dbg_write(g, 0xff00, 0x20);
  gb_set_input(g, 1 << 4);
  gb_dbg_write(g, 0xff0f, 0);
  gb_dbg_write(g, 0xff00, 0x10);
  ASSERT_EQ(gb_dbg_read(g, 0xff0f) & 0x10, 0x10);
  serial_calls = serial_received = 0;
  gb_set_serial_callback(g, serial_callback, NULL);
  gb_dbg_write(g, 0xff01, 'X');
  gb_dbg_write(g, 0xff02, 0x81);
  ASSERT_EQ(serial_calls, 0);
  ASSERT_TRUE(gb_dbg_read(g, 0xff02) & 0x80);
  step(g, 1024);
  ASSERT_EQ(serial_calls, 1);
  ASSERT_EQ(serial_received, 'X');
  ASSERT_EQ(gb_dbg_read(g, 0xff01), 0xa5);
  ASSERT_EQ(gb_dbg_read(g, 0xff02) & 0x80, 0);
  ASSERT_EQ(gb_dbg_read(g, 0xff0f) & 8, 8);
  gb_destroy(g);
}

UTEST(core, linked_serial_instances_exchange_bytes) {
  gb_t *a = load((const uint8_t[]){0x00}, 1);
  gb_t *b = load((const uint8_t[]){0x00}, 1);
  gb_link_serial(a, b);
  gb_dbg_write(a, 0xff01, 0x12);
  gb_dbg_write(b, 0xff01, 0x34);
  gb_dbg_write(a, 0xff02, 0x81);
  gb_dbg_write(b, 0xff02, 0x81);
  step(a, 1024);
  ASSERT_EQ(gb_dbg_read(a, 0xff01), 0x34);
  ASSERT_EQ(gb_dbg_read(b, 0xff01), 0x12);
  ASSERT_EQ(gb_dbg_read(a, 0xff0f) & 8, 8);
  ASSERT_EQ(gb_dbg_read(b, 0xff0f) & 8, 8);
  gb_destroy(a);
  gb_destroy(b);
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

UTEST(core, mbc3_and_mbc5_bank_switching) {
  static uint8_t rom[0x800000];
  memset(rom, 0, sizeof rom);
  rom[0x147] = 0x13;
  rom[0x149] = 3;
  rom[0x4000] = 1;
  rom[0x8000] = 2;
  rom[0xc000] = 3;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  ASSERT_EQ(gb_dbg_read(g, 0x4000), 1);
  gb_dbg_write(g, 0x2000, 3);
  ASSERT_EQ(gb_dbg_read(g, 0x4000), 3);
  gb_dbg_write(g, 0x0000, 0x0a);
  gb_dbg_write(g, 0x4000, 8);
  gb_dbg_write(g, 0xa000, 30);
  gb_dbg_write(g, 0x6000, 0);
  gb_dbg_write(g, 0x6000, 1);
  ASSERT_EQ(gb_dbg_read(g, 0xa000), 30);
  gb_destroy(g);

  memset(rom, 0, sizeof rom);
  rom[0x147] = 0x1b;
  rom[0x149] = 3;
  rom[0x4000] = 1;
  rom[0x410000] = 4;
  g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  gb_dbg_write(g, 0x0000, 0x0a);
  gb_dbg_write(g, 0x2000, 0);
  ASSERT_EQ(gb_dbg_read(g, 0x4000), 0);
  gb_dbg_write(g, 0x2000, 4);
  gb_dbg_write(g, 0x3000, 1);
  ASSERT_EQ(gb_dbg_read(g, 0x4000), 4);
  gb_dbg_write(g, 0x4000, 1);
  gb_dbg_write(g, 0xa000, 0xa5);
  gb_dbg_write(g, 0x4000, 0);
  ASSERT_EQ(gb_dbg_read(g, 0xa000), 0);
  gb_destroy(g);
}

UTEST(core, mbc2_nibble_ram_and_banking) {
  static uint8_t rom[0x40000];
  memset(rom, 0, sizeof rom);
  rom[0x147] = 6;
  for (unsigned bank = 1; bank < 16; bank++)
    rom[bank * 0x4000] = (uint8_t)bank;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  ASSERT_EQ(gb_save_ram_size(g), 0x200);
  ASSERT_EQ(gb_dbg_read(g, 0x4000), 1);
  gb_dbg_write(g, 0x2100, 3);
  ASSERT_EQ(gb_dbg_read(g, 0x4000), 3);
  gb_dbg_write(g, 0x2000, 0x0a);
  gb_dbg_write(g, 0xa000, 0xab);
  gb_dbg_write(g, 0xa1ff, 5);
  ASSERT_EQ(gb_dbg_read(g, 0xa000), 0xfb);
  ASSERT_EQ(gb_dbg_read(g, 0xa1ff), 0xf5);
  ASSERT_EQ(gb_dbg_read(g, 0xa200), 0xff);
  gb_dbg_write(g, 0x2000, 0);
  ASSERT_EQ(gb_dbg_read(g, 0xa000), 0xff);
  gb_destroy(g);

  memset(rom, 0, sizeof rom);
  rom[0x147] = 5;
  g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  ASSERT_EQ(gb_save_ram_size(g), 0);
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

UTEST(core, cgb_vram_wram_banking_and_state) {
  static uint8_t rom[0x8000];
  memset(rom, 0, sizeof rom);
  rom[0x143] = 0xc0;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  gb_dbg_write(g, 0xff4f, 0);
  gb_dbg_write(g, 0x8000, 0x12);
  gb_dbg_write(g, 0xff4f, 1);
  gb_dbg_write(g, 0x8000, 0x34);
  ASSERT_EQ(gb_dbg_read(g, 0x8000), 0x34);
  gb_dbg_write(g, 0xff4f, 0);
  ASSERT_EQ(gb_dbg_read(g, 0x8000), 0x12);
  gb_dbg_write(g, 0xff70, 2);
  gb_dbg_write(g, 0xd000, 0x56);
  gb_dbg_write(g, 0xff70, 3);
  gb_dbg_write(g, 0xd000, 0x78);
  ASSERT_EQ(gb_dbg_read(g, 0xd000), 0x78);
  gb_dbg_write(g, 0xff70, 2);
  ASSERT_EQ(gb_dbg_read(g, 0xd000), 0x56);
  size_t n = gb_save_state_size(g);
  uint8_t *state = malloc(n);
  ASSERT_TRUE(state);
  ASSERT_EQ(gb_save_state(g, state), n);
  gb_dbg_write(g, 0xff4f, 1);
  gb_dbg_write(g, 0xff70, 3);
  ASSERT_EQ(gb_load_state(g, state, n), 0);
  ASSERT_EQ(gb_dbg_read(g, 0xff4f), 0xfe);
  ASSERT_EQ(gb_dbg_read(g, 0xff70), 0xfa);
  ASSERT_EQ(gb_dbg_read(g, 0x8000), 0x12);
  ASSERT_EQ(gb_dbg_read(g, 0xd000), 0x56);
  free(state);
  gb_destroy(g);
}

UTEST(core, cgb_general_dma) {
  static uint8_t rom[0x8000];
  memset(rom, 0, sizeof rom);
  rom[0x143] = 0xc0;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  for (unsigned i = 0; i < 0x20; i++)
    gb_dbg_write(g, (uint16_t)(0xc000 + i), (uint8_t)(0xa0 + i));
  gb_dbg_write(g, 0xff4f, 1);
  gb_dbg_write(g, 0xff51, 0xc0);
  gb_dbg_write(g, 0xff52, 0x00);
  gb_dbg_write(g, 0xff53, 0x07);
  gb_dbg_write(g, 0xff54, 0x00);
  gb_dbg_write(g, 0xff55, 1);
  for (unsigned i = 0; i < 0x20; i++)
    ASSERT_EQ(gb_dbg_read(g, (uint16_t)(0x8700 + i)), 0xa0 + i);
  ASSERT_EQ(gb_dbg_read(g, 0xff55), 0xff);
  ASSERT_EQ(gb_dbg_read(g, 0xff51), 0xc0);
  gb_destroy(g);
}

UTEST(core, cgb_hblank_dma) {
  static uint8_t rom[0x8000];
  memset(rom, 0, sizeof rom);
  rom[0x143] = 0xc0;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  for (unsigned i = 0; i < 0x20; i++)
    gb_dbg_write(g, (uint16_t)(0xc000 + i), (uint8_t)(0xb0 + i));
  gb_dbg_write(g, 0xff51, 0xc0);
  gb_dbg_write(g, 0xff52, 0x00);
  gb_dbg_write(g, 0xff53, 0x00);
  gb_dbg_write(g, 0xff54, 0x00);
  gb_dbg_write(g, 0xff55, 0x82);
  ASSERT_EQ(gb_dbg_read(g, 0xff55), 2);
  ASSERT_EQ(gb_dbg_read(g, 0x8000), 0);
  for (unsigned i = 0; i < 64; i++)
    gb_dbg_step(g);
  ASSERT_EQ(gb_dbg_read(g, 0xff55), 1);
  ASSERT_EQ(gb_dbg_read(g, 0x8000), 0xb0);
  ASSERT_EQ(gb_dbg_read(g, 0x8010), 0);
  gb_dbg_write(g, 0xff55, 0x80);
  ASSERT_EQ(gb_dbg_read(g, 0xff55), 0x81);
  gb_destroy(g);
}

UTEST(core, cgb_speed_switch) {
  static uint8_t rom[0x8000];
  memset(rom, 0, sizeof rom);
  rom[0x143] = 0xc0;
  rom[0x100] = 0x10;
  rom[0x101] = 0x00;
  rom[0x102] = 0x10;
  rom[0x103] = 0x00;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  gb_dbg_write(g, 0xff4d, 1);
  ASSERT_EQ(gb_dbg_read(g, 0xff4d), 0x7f);
  gb_dbg_step(g);
  ASSERT_EQ(gb_dbg_read(g, 0xff4d), 0xfe);
  ASSERT_TRUE(!regs(g).halted);
  gb_dbg_step(g);
  ASSERT_TRUE(regs(g).halted);
  gb_destroy(g);

  g = load((const uint8_t[]){0x10, 0x00}, 2);
  ASSERT_EQ(gb_dbg_read(g, 0xff4d), 0xff);
  gb_dbg_step(g);
  ASSERT_TRUE(regs(g).halted);
  gb_destroy(g);
}

UTEST(core, cgb_infrared_register) {
  static uint8_t rom[0x8000];
  memset(rom, 0, sizeof rom);
  rom[0x143] = 0xc0;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  ASSERT_EQ(gb_dbg_read(g, 0xff56), 0x3c);
  gb_dbg_write(g, 0xff56, 0xff);
  ASSERT_EQ(gb_dbg_read(g, 0xff56), 0x3f);
  gb_destroy(g);

  g = load((const uint8_t[]){0x00}, 1);
  ASSERT_EQ(gb_dbg_read(g, 0xff56), 0xff);
  gb_dbg_write(g, 0xff56, 3);
  ASSERT_EQ(gb_dbg_read(g, 0xff56), 0xff);
  gb_destroy(g);
}

UTEST(ppu, cgb_tile_attributes_and_palette) {
  static uint8_t rom[0x8000];
  memset(rom, 0, sizeof rom);
  rom[0x143] = 0xc0;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  gb_dbg_write(g, 0xff4f, 1);
  gb_dbg_write(g, 0x8010, 0x80);
  gb_dbg_write(g, 0x8011, 0x00);
  gb_dbg_write(g, 0x9800, 0x08);
  gb_dbg_write(g, 0xff4f, 0);
  gb_dbg_write(g, 0x9800, 0x01);
  gb_dbg_write(g, 0xff68, 0x82);
  gb_dbg_write(g, 0xff69, 0x1f);
  gb_dbg_write(g, 0xff69, 0x00);
  ASSERT_EQ(gb_dbg_read(g, 0xff68), 0x84);
  ASSERT_EQ(gb_dbg_read(g, 0x9800), 1);
  gb_dbg_write(g, 0xff4f, 1);
  ASSERT_EQ(gb_dbg_read(g, 0x8010), 0x80);
  gb_run_frame(g);
  ASSERT_EQ(gb_framebuffer(g)[0], 0xffff0000);
  gb_destroy(g);
}

UTEST(ppu, cgb_bg_priority_and_opri) {
  static uint8_t rom[0x8000];
  memset(rom, 0, sizeof rom);
  rom[0x143] = 0xc0;
  gb_t *g = gb_create();
  ASSERT_EQ(gb_load_rom(g, rom, sizeof rom), 0);
  gb_dbg_write(g, 0x8010, 0x80);
  gb_dbg_write(g, 0x8011, 0x00);
  gb_dbg_write(g, 0x8020, 0x80);
  gb_dbg_write(g, 0x8021, 0x00);
  gb_dbg_write(g, 0x9800, 1);
  gb_dbg_write(g, 0xfe00, 16);
  gb_dbg_write(g, 0xfe01, 8);
  gb_dbg_write(g, 0xfe02, 2);
  gb_dbg_write(g, 0xff26, 0x80);
  gb_dbg_write(g, 0xff40, 0x93);
  gb_dbg_write(g, 0xff68, 0x02);
  gb_dbg_write(g, 0xff69, 0x1f);
  gb_dbg_write(g, 0xff69, 0x00);
  gb_dbg_write(g, 0xff6a, 0x00);
  gb_dbg_write(g, 0xff6b, 0x1f);
  gb_dbg_write(g, 0xff6b, 0x00);
  gb_dbg_write(g, 0xff4f, 1);
  gb_dbg_write(g, 0x9800, 0x80);
  gb_dbg_write(g, 0xff4f, 0);
  gb_run_frame(g);
  ASSERT_EQ(gb_framebuffer(g)[0], 0xff000000);
  ASSERT_EQ(gb_dbg_read(g, 0xff6c), 0xfe);
  gb_dbg_write(g, 0xff6c, 1);
  ASSERT_EQ(gb_dbg_read(g, 0xff6c), 0xff);
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

UTEST(core, apu_square_channel) {
  gb_t *g = load((const uint8_t[]){0x00}, 1);
  audio_calls = 0;
  audio_frames = 0;
  audio_peak = 0;
  gb_set_audio_callback(g, audio_callback, NULL);
  gb_dbg_write(g, 0xff26, 0x80);
  gb_dbg_write(g, 0xff11, 0x80);
  gb_dbg_write(g, 0xff12, 0xf0);
  gb_dbg_write(g, 0xff13, 0x00);
  gb_dbg_write(g, 0xff14, 0x87);
  gb_dbg_write(g, 0xff24, 0x77);
  gb_dbg_write(g, 0xff25, 0x11);
  ASSERT_TRUE(gb_dbg_read(g, 0xff26) & 1);
  gb_run_frame(g);
  ASSERT_EQ(audio_calls, 1);
  ASSERT_EQ(audio_frames, 735);
  ASSERT_TRUE(audio_peak > 0);
  gb_destroy(g);
}

UTEST(core, apu_wave_and_noise_channels) {
  gb_t *g = load((const uint8_t[]){0x00}, 1);
  audio_calls = 0;
  audio_frames = 0;
  audio_peak = 0;
  gb_set_audio_callback(g, audio_callback, NULL);
  gb_dbg_write(g, 0xff26, 0x80);
  gb_dbg_write(g, 0xff30, 0xff);
  gb_dbg_write(g, 0xff31, 0xff);
  gb_dbg_write(g, 0xff1a, 0x80);
  gb_dbg_write(g, 0xff1c, 0x20);
  gb_dbg_write(g, 0xff1e, 0x80);
  gb_dbg_write(g, 0xff24, 0x77);
  gb_dbg_write(g, 0xff25, 0x44);
  ASSERT_TRUE(gb_dbg_read(g, 0xff26) & 4);
  gb_run_frame(g);
  ASSERT_EQ(audio_calls, 1);
  ASSERT_EQ(audio_frames, 735);
  ASSERT_TRUE(audio_peak > 0);
  gb_dbg_write(g, 0xff20, 0x05);
  gb_dbg_write(g, 0xff21, 0xf0);
  gb_dbg_write(g, 0xff23, 0x80);
  ASSERT_TRUE(gb_dbg_read(g, 0xff26) & 8);
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
  core_compare_preserves_accumulator();
  core_adc_uses_carry();
  core_cb_bit_rotate_set_res();
  core_jumps_call_ret_and_stack();
  core_debugger_pc_breakpoints();
  core_debugger_disassembly();
  core_debugger_memory_watchpoints();
  core_optional_boot_rom_mapping();
  core_ei_delay_interrupt_and_halt();
  core_timer_overflow_and_reset_state();
  core_joyp_selection_and_serial_callback();
  core_linked_serial_instances_exchange_bytes();
  core_mbc1_bank_switching();
  core_mbc3_and_mbc5_bank_switching();
  core_mbc2_nibble_ram_and_banking();
  core_wram_echo();
  core_cgb_vram_wram_banking_and_state();
  core_cgb_general_dma();
  core_cgb_hblank_dma();
  core_cgb_speed_switch();
  core_cgb_infrared_register();
  ppu_cgb_tile_attributes_and_palette();
  ppu_cgb_bg_priority_and_opri();
  ppu_background_tile();
  ppu_lcd_timing_and_interrupts();
  ppu_sprite_rendering();
  core_battery_ram_round_trip();
  core_save_state_round_trip();
  core_apu_square_channel();
  core_apu_wave_and_noise_channels();
  core_extended_control_and_stack_opcodes();
  core_conditional_relative_and_signed_stack_arithmetic();
  core_oam_dma_transfer();
  core_timer_uses_divider_edges();
  ppu_stat_write_rechecks_coincidence();
  puts("20 tests passed");
  return 0;
}
