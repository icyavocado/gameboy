#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gb {
  uint8_t *rom, *ram;
  size_t rom_size, ram_size;
  uint8_t mem[0x10000];
  uint8_t *vram;
  uint32_t fb[160 * 144];
  uint16_t af, bc, de, hl, sp, pc;
  uint8_t ime, halted, input, div_counter, lcd_line;
  uint8_t mbc, rom_bank, ram_bank, ram_enable;
  uint16_t timer_counter;
  gb_model_t model;
};

static uint8_t lo(uint16_t x) { return (uint8_t)x; }
static uint8_t hi(uint16_t x) { return (uint8_t)(x >> 8); }
static uint16_t pair(uint8_t h, uint8_t l) { return (uint16_t)((h << 8) | l); }
static uint8_t read8(const gb_t *g, uint16_t a);
static void write8(gb_t *g, uint16_t a, uint8_t v);
static void set_r8(gb_t *g, unsigned n, uint8_t v) {
  switch (n) { case 0: g->bc = pair(v, lo(g->bc)); break; case 1: g->bc = pair(hi(g->bc), v); break; case 2: g->de = pair(v, lo(g->de)); break; case 3: g->de = pair(hi(g->de), v); break; case 4: g->hl = pair(v, lo(g->hl)); break; case 5: g->hl = pair(hi(g->hl), v); break; case 6: write8(g, g->hl, v); break; default: g->af = pair(v, lo(g->af)); break; }
}

static uint8_t read8(const gb_t *g, uint16_t a) {
  if (a < 0x4000 && g->rom) return g->rom[a];
  if (a < 0x8000 && g->rom) {
    size_t bank = g->rom_bank % (g->rom_size / 0x4000 ? g->rom_size / 0x4000 : 1);
    return g->rom[bank * 0x4000 + (a - 0x4000)];
  }
  if (a >= 0xa000 && a < 0xc000 && g->ram && g->ram_enable) return g->ram[(g->ram_bank * 0x2000 + a - 0xa000) % g->ram_size];
  if (a == 0xff00) return (uint8_t)(0xc0 | (g->input & 0x0f));
  return g->mem[a];
}

static void write8(gb_t *g, uint16_t a, uint8_t v) {
  if (a < 0x2000 && g->mbc) { g->ram_enable = (v & 0x0f) == 0x0a; return; }
  if (a >= 0x2000 && a < 0x4000 && g->mbc) { g->rom_bank = v & 0x1f; if (!g->rom_bank) g->rom_bank = 1; return; }
  if (a >= 0x4000 && a < 0x6000 && g->mbc) { g->ram_bank = v & 3; return; }
  if (a >= 0xa000 && a < 0xc000 && g->ram && g->ram_enable) { g->ram[(g->ram_bank * 0x2000 + a - 0xa000) % g->ram_size] = v; return; }
  if (a == 0xff04) { g->mem[a] = 0; g->div_counter = 0; return; }
  if (a == 0xff44) { g->mem[a] = 0; g->lcd_line = 0; return; }
  g->mem[a] = v;
}

static uint8_t fetch(gb_t *g) { return read8(g, g->pc++); }
static uint16_t fetch16(gb_t *g) { uint8_t l = fetch(g); return pair(fetch(g), l); }
static void push(gb_t *g, uint16_t v) { write8(g, --g->sp, hi(v)); write8(g, --g->sp, lo(v)); }
static uint16_t pop(gb_t *g) { uint8_t l = read8(g, g->sp++), h = read8(g, g->sp++); return pair(h, l); }
static void flags(gb_t *g, uint8_t z, uint8_t n, uint8_t h, uint8_t c) { g->af = (g->af & 0xff00) | (z << 7) | (n << 6) | (h << 5) | (c << 4); }
static uint8_t f(const gb_t *g, uint8_t n) { return (lo(g->af) >> n) & 1; }

static void add_a(gb_t *g, uint8_t v, uint8_t carry) {
  uint8_t a = hi(g->af); uint16_t x = a + v + carry;
  flags(g, !(x & 255), 0, ((a & 15) + (v & 15) + carry) > 15, x > 255); g->af = pair((uint8_t)x, lo(g->af));
}
static void render_line(gb_t *g, unsigned y) {
  uint8_t scx = g->mem[0xff43], scy = g->mem[0xff42], lcdc = g->mem[0xff40];
  for (unsigned x = 0; x < 160; x++) {
    uint8_t shade = 0;
    if (lcdc & 1) {
      unsigned px = (x + scx) & 255, py = (y + scy) & 255;
      uint16_t map = (lcdc & 8) ? 0x1c00 : 0x1800, tile = g->vram[map + (py / 8) * 32 + px / 8];
      uint16_t data = (lcdc & 16) ? tile * 16 : 0x1000 + (int8_t)tile * 16;
      uint8_t b1 = g->vram[data + (py & 7) * 2], b2 = g->vram[data + (py & 7) * 2 + 1];
      unsigned bit = 7 - (px & 7); shade = ((b2 >> bit) & 1) * 2 + ((b1 >> bit) & 1);
    }
    static const uint32_t colors[] = { 0xfff8f8f8, 0xffa8a8a8, 0xff585858, 0xff101010 };
    g->fb[y * 160 + x] = colors[(g->mem[0xff47] >> (shade * 2)) & 3];
  }
}

int gb_dbg_step(gb_t *g) {
  if (g->halted) return 4;
  uint8_t op = fetch(g), a, v; uint16_t nn;
  switch (op) {
  case 0x00: return 4;
  case 0x01: g->bc = fetch16(g); return 12;
  case 0x11: g->de = fetch16(g); return 12;
  case 0x21: g->hl = fetch16(g); return 12;
  case 0x31: g->sp = fetch16(g); return 12;
  case 0x3e: g->af = pair(fetch(g), lo(g->af)); return 8;
  case 0x06: g->bc = pair(fetch(g), lo(g->bc)); return 8;
  case 0x0e: g->bc = pair(hi(g->bc), fetch(g)); return 8;
  case 0x16: g->de = pair(fetch(g), lo(g->de)); return 8;
  case 0x1e: g->de = pair(hi(g->de), fetch(g)); return 8;
  case 0x26: g->hl = pair(fetch(g), lo(g->hl)); return 8;
  case 0x2e: g->hl = pair(hi(g->hl), fetch(g)); return 8;
  case 0x32: write8(g, g->hl, hi(g->af)); g->hl--; return 8;
  case 0x3a: g->af = pair(read8(g, g->hl--), lo(g->af)); return 8;
  case 0x77: write8(g, g->hl, hi(g->af)); return 8;
  case 0x7e: g->af = pair(read8(g, g->hl), lo(g->af)); return 8;
  case 0x36: write8(g, g->hl, fetch(g)); return 12;
  case 0x80: add_a(g, hi(g->bc), 0); return 4;
  case 0x81: add_a(g, lo(g->bc), 0); return 4;
  case 0x87: add_a(g, hi(g->af), 0); return 4;
  case 0x90: a = hi(g->af); v = hi(g->bc); flags(g, a == v, 1, (a & 15) < (v & 15), a < v); g->af = pair(a - v, lo(g->af)); return 4;
  case 0xaf: a = hi(g->af); g->af = pair(0, lo(g->af)); flags(g, 1, 0, 0, 0); (void)a; return 4;
  case 0xc3: g->pc = fetch16(g); return 16;
  case 0x18: g->pc = (uint16_t)(g->pc + (int8_t)fetch(g)); return 12;
  case 0x20: nn = fetch(g); if (!f(g, 7)) g->pc = (uint16_t)(g->pc + (int8_t)nn); return f(g, 7) ? 8 : 12;
  case 0xcd: nn = fetch16(g); push(g, g->pc); g->pc = nn; return 24;
  case 0xc9: g->pc = pop(g); return 16;
  case 0x76: g->halted = 1; return 4;
  case 0xf3: g->ime = 0; return 4;
  case 0xfb: g->ime = 1; return 4;
  default:
    if ((op & 0xc7) == 0x06) { unsigned n = (op >> 3) & 7; v = fetch(g); set_r8(g, n, v); return n == 6 ? 12 : 8; }
    return 4;
  }
}

void gb_run_frame(gb_t *g) {
  unsigned cycles = 0;
  while (cycles < 70224) { int n = gb_dbg_step(g); cycles += (unsigned)n; g->div_counter += (uint8_t)n; g->timer_counter += (uint16_t)n; if (g->timer_counter >= 256) { g->timer_counter -= 256; g->mem[0xff04]++; } }
  for (unsigned y = 0; y < 144; y++) render_line(g, y);
}
gb_t *gb_create(void) { gb_t *g = calloc(1, sizeof(*g)); g->model = GB_MODEL_AUTO; g->rom_bank = 1; g->vram = g->mem + 0x8000; g->mem[0xff40] = 0x91; g->mem[0xff47] = 0xe4; return g; }
void gb_destroy(gb_t *g) { if (g) { free(g->rom); free(g->ram); free(g); } }
int gb_load_rom(gb_t *g, const uint8_t *rom, size_t size) { if (!g || !rom || size < 0x150) return -1; free(g->rom); g->rom = malloc(size); if (!g->rom) return -1; memcpy(g->rom, rom, size); g->rom_size = size; g->mbc = rom[0x147] >= 1 && rom[0x147] <= 3; unsigned banks = rom[0x149] ? (1u << (rom[0x149] + 1)) : 1; g->ram_size = banks * 0x2000; free(g->ram); g->ram = calloc(1, g->ram_size); gb_reset(g); return 0; }
void gb_set_model(gb_t *g, gb_model_t m) { g->model = m; }
void gb_reset(gb_t *g) { g->af = 0x01b0; g->bc = 0x0013; g->de = 0x00d8; g->hl = 0x014d; g->sp = 0xfffe; g->pc = 0x0100; g->halted = 0; g->mem[0xff40] = 0x91; g->mem[0xff47] = 0xe4; }
const uint32_t *gb_framebuffer(const gb_t *g) { return g->fb; }
void gb_set_input(gb_t *g, uint8_t buttons) { g->input = buttons; }
uint8_t gb_dbg_read(const gb_t *g, uint16_t a) { return read8(g, a); }
void gb_dbg_write(gb_t *g, uint16_t a, uint8_t v) { write8(g, a, v); }
void gb_dbg_regs(const gb_t *g, uint16_t out[6]) { out[0]=g->af; out[1]=g->bc; out[2]=g->de; out[3]=g->hl; out[4]=g->sp; out[5]=g->pc; }
