#include "gb.h"
#include <stdlib.h>
#include <string.h>

/* Invalid SM83 opcodes are intentionally treated as NOPs in Phase 1. */
#define STATE_VERSION 5u
#define STATE_HEADER_SIZE 24u
struct gb {
  uint8_t *rom, *ram, mem[65536];
  uint8_t vram[0x2000], oam[0xa0];
  size_t rom_size, ram_size;
  uint32_t fb[160 * 144];
  uint8_t bg_line[160];
  uint16_t af, bc, de, hl, sp, pc, timer, divider;
  uint8_t ime, ei_delay, halted, halt_bug, input, div, mbc, battery, ram_bank,
      ram_enable, upper, mode, ppu_mode, stat_signal, dma_page, dma_index,
      dma_active, timer_signal, rtc_select, rtc_latched_valid, rtc[5],
      rtc_latched[5];
  uint16_t rom_bank;
  unsigned ppu_cycles;
  unsigned dma_cycles;
  uint32_t audio_phase[4];
  uint16_t noise_lfsr;
  uint8_t audio_enabled[4];
  gb_model_t model;
  gb_serial_cb serial;
  void *serial_user;
  gb_audio_cb audio;
  void *audio_user;
};
static uint8_t lo(uint16_t x) { return (uint8_t)x; }
static uint8_t hi(uint16_t x) { return (uint8_t)(x >> 8); }
static uint16_t pr(uint8_t h, uint8_t l) { return (uint16_t)(h << 8 | l); }
static uint8_t f(const gb_t *g, unsigned n) {
  return (uint8_t)(lo(g->af) >> n & 1);
}
static void fs(gb_t *g, unsigned z, unsigned n, unsigned h, unsigned c) {
  g->af = (uint16_t)((g->af & 0xff00) | (z << 7 | n << 6 | h << 5 | c << 4));
}
static uint8_t rd(const gb_t *, uint16_t);
static void wr(gb_t *, uint16_t, uint8_t);
static void ppu_tick(gb_t *);
static void ppu_stat(gb_t *);
static void audio_trigger(gb_t *, unsigned);
static void audio_frame(gb_t *);
static uint8_t audio_read(const gb_t *g, uint16_t a) {
  if (a == 0xff26)
    return (uint8_t)(g->mem[a] | 0x70 | (g->audio_enabled[0] ? 1 : 0) |
                     (g->audio_enabled[1] ? 2 : 0) |
                     (g->audio_enabled[2] ? 4 : 0) |
                     (g->audio_enabled[3] ? 8 : 0));
  if (a >= 0xff30 && a <= 0xff3f)
    return g->mem[a];
  return g->mem[a];
}
static void audio_trigger(gb_t *g, unsigned channel) {
  static const uint16_t base[] = {0xff10, 0xff16, 0xff1a, 0xff20};
  g->audio_enabled[channel] = (uint8_t)((g->mem[base[channel] +
                                                   (channel == 2 ? 0 : channel == 3 ? 1 : 2)] &
                                         (channel == 2 ? 0x80 : 0xf8)) != 0);
  g->audio_phase[channel] = 0;
  if (channel == 3)
    g->noise_lfsr = 0x7fff;
  g->mem[0xff26] |= (uint8_t)(1u << channel);
}
static int16_t audio_sample(gb_t *g, unsigned channel) {
  static const uint8_t duty[] = {0x01, 0x81, 0x87, 0x7e};
  uint16_t base = channel ? (channel == 1 ? 0xff16 : channel == 2 ? 0xff1a : 0xff20)
                          : 0xff10;
  uint8_t control = g->mem[base + 2];
  if (channel == 2) {
    uint16_t frequency = (uint16_t)((g->mem[base + 4] & 7) << 8 | g->mem[base + 3]);
    uint32_t step = (uint32_t)(((uint64_t)(2048 - frequency) * 65536u) / 44100u);
    unsigned index = (g->audio_phase[2] >> 16) & 31;
    uint8_t volume = (control >> 5) & 3;
    uint8_t sample = (g->mem[0xff30 + index / 2] >> (index & 1 ? 0 : 4)) & 15;
    g->audio_phase[2] += step;
    if (!volume) return 0;
    if (volume == 1) return (int16_t)(((int)sample - 8) * 128);
    if (volume == 2) return (int16_t)(((int)sample - 8) * 64);
    return (int16_t)(((int)sample - 8) * 32);
  }
  if (channel == 3) {
    control = g->mem[base + 1];
    uint8_t divisor = g->mem[base] & 7, shift = g->mem[base] >> 4;
    unsigned period = (divisor ? divisor * 16u : 8u) << shift;
    g->audio_phase[3]++;
    if (period && g->audio_phase[3] % period == 0) {
      uint16_t bit = (uint16_t)((g->noise_lfsr ^ (g->noise_lfsr >> 1)) & 1);
      g->noise_lfsr = (uint16_t)((g->noise_lfsr >> 1) | (bit << 14));
      if (g->mem[base] & 8)
        g->noise_lfsr = (uint16_t)((g->noise_lfsr & ~(1u << 6)) | (bit << 6));
    }
    return (control >> 4) && !(g->noise_lfsr & 1) ? (int16_t)((control >> 4) * 512) : 0;
  }
  uint8_t volume = control >> 4, duty_index = g->mem[base] >> 6;
  uint16_t frequency = (uint16_t)((g->mem[base + 4] & 7) << 8 | g->mem[base + 3]);
  uint16_t hz = (uint16_t)((2048 - frequency) * 131072u / 2048u);
  uint32_t step = hz ? (uint32_t)(((uint64_t)hz << 32) / 44100u) : 0;
  unsigned bit = (g->audio_phase[channel] >> 29) & 7;
  g->audio_phase[channel] += step;
  return (int16_t)((duty[duty_index] & (1u << bit) ? volume : 0) * 512);
}
static void audio_frame(gb_t *g) {
  int16_t samples[735 * 2];
  uint8_t routing = g->mem[0xff25];
  uint8_t left = g->mem[0xff24] >> 4, right = g->mem[0xff24] & 7;
  for (unsigned i = 0; i < 735; i++) {
    int16_t value[4];
    for (unsigned channel = 0; channel < 4; channel++)
      value[channel] = g->audio_enabled[channel] ? audio_sample(g, channel) : 0;
    samples[i * 2] = 0;
    samples[i * 2 + 1] = 0;
    for (unsigned channel = 0; channel < 4; channel++) {
      if (routing & (uint8_t)(1u << (channel + 4))) samples[i * 2] += value[channel];
      if (routing & (uint8_t)(1u << channel)) samples[i * 2 + 1] += value[channel];
    }
    samples[i * 2] = (int16_t)(samples[i * 2] * left / 8);
    samples[i * 2 + 1] = (int16_t)(samples[i * 2 + 1] * right / 8);
  }
  if (g->audio && (g->mem[0xff26] & 0x80))
    g->audio(g->audio_user, samples, 735);
}
static unsigned timer_bit(const gb_t *g) {
  static const unsigned bits[] = {9, 3, 5, 7};
  return bits[g->mem[0xff07] & 3];
}
static unsigned timer_level(const gb_t *g) {
  return (g->mem[0xff07] & 4) && ((g->divider >> timer_bit(g)) & 1);
}
static uint8_t *rp8(gb_t *g, unsigned n) {
  switch (n) {
  case 0:
    return (uint8_t *)&g->bc + 1;
  case 1:
    return (uint8_t *)&g->bc;
  case 2:
    return (uint8_t *)&g->de + 1;
  case 3:
    return (uint8_t *)&g->de;
  case 4:
    return (uint8_t *)&g->hl + 1;
  case 5:
    return (uint8_t *)&g->hl;
  default:
    return (uint8_t *)&g->af + 1;
  }
}
static uint16_t *rp16(gb_t *g, unsigned n) {
  return n == 0 ? &g->bc : n == 1 ? &g->de : n == 2 ? &g->hl : &g->sp;
}
static uint8_t gr(gb_t *g, unsigned n) {
  return n == 6 ? rd(g, g->hl) : *rp8(g, n);
}
static void sr(gb_t *g, unsigned n, uint8_t v) {
  if (n == 6)
    wr(g, g->hl, v);
  else
    *rp8(g, n) = v;
}
static int cond(const gb_t *g, unsigned n) {
  return n == 0 ? !f(g, 7) : n == 1 ? f(g, 7) : n == 2 ? !f(g, 4) : f(g, 4);
}
static void addhl(gb_t *g, uint16_t v) {
  uint16_t old = g->hl, r = (uint16_t)(old + v);
  fs(g, f(g, 7), 0, ((old & 0xfff) + (v & 0xfff)) > 0xfff,
     (unsigned)old + v > 0xffff);
  g->hl = r;
}
static uint16_t addsp(gb_t *g, int8_t offset) {
  uint16_t sp = g->sp;
  unsigned value = (uint8_t)offset;
  fs(g, 0, 0, ((sp & 15) + (value & 15)) > 15,
     ((sp & 255) + value) > 255);
  return (uint16_t)(sp + offset);
}
static uint8_t fc(gb_t *g) {
  uint8_t v = rd(g, g->pc);
  if (!g->halt_bug)
    g->pc++;
  else
    g->halt_bug = 0;
  return v;
}
static uint16_t fn(gb_t *g) {
  uint8_t l = fc(g);
  return pr(fc(g), l);
}
static void push(gb_t *g, uint16_t v) {
  wr(g, --g->sp, hi(v));
  wr(g, --g->sp, lo(v));
}
static uint16_t pop(gb_t *g) {
  uint8_t l = rd(g, g->sp++), h = rd(g, g->sp++);
  return pr(h, l);
}
static void pop_pair(gb_t *g, unsigned n) {
  uint16_t value = pop(g);
  if (n == 3)
    value &= 0xfff0;
  if (n == 3)
    g->af = value;
  else
    *rp16(g, n) = value;
}
static size_t ramsize(uint8_t c) {
  static const size_t n[] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
  return c < 6 ? n[c] : 0;
}
static int has_battery(uint8_t type) {
  return type == 3 || type == 9 || (type >= 0x0f && type <= 0x13) ||
         (type >= 0x1b && type <= 0x1e);
}
static void put16(uint8_t **p, uint16_t v) {
  (*p)[0] = (uint8_t)v;
  (*p)[1] = (uint8_t)(v >> 8);
  *p += 2;
}
static void put32(uint8_t **p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p, (uint16_t)(v >> 16));
}
static uint16_t get16(const uint8_t **p) {
  uint16_t v = (uint16_t)((*p)[0] | (*p)[1] << 8);
  *p += 2;
  return v;
}
static uint32_t get32(const uint8_t **p) {
  uint32_t v = get16(p);
  return v | (uint32_t)get16(p) << 16;
}
static size_t rb(const gb_t *g, uint16_t a) {
  size_t banks = g->rom_size / 0x4000;
  unsigned b = a < 0x4000 ? 0 : g->rom_bank;
  if (!banks)
    banks = 1;
  if (g->mbc == 1 && a < 0x4000 && !g->mode)
    b = g->upper << 5;
  return ((size_t)(b % banks) * 0x4000 + (a & 0x3fff)) % g->rom_size;
}
static uint8_t jp(const gb_t *g) {
  uint8_t s = g->mem[0xff00] & 0x30, v = 0xf;
  if (!(s & 0x10))
    v &= (uint8_t)~(g->input >> 4);
  if (!(s & 0x20))
    v &= (uint8_t)~(g->input & 15);
  return (uint8_t)(0xc0 | s | v);
}
static uint8_t rd(const gb_t *g, uint16_t a) {
  if (a < 0x8000 && g->rom)
    return g->rom[rb(g, a)];
  if (a >= 0xe000 && a < 0xfe00)
    a = (uint16_t)(a - 0x2000);
  if (a >= 0x8000 && a < 0xa000)
    return g->vram[a - 0x8000];
  if (a >= 0xfe00 && a < 0xff00)
    return a < 0xfea0 ? g->oam[a - 0xfe00] : 0xff;
  if (a == 0xff44)
    return g->mem[a];
  if (a == 0xff41)
    return (uint8_t)(g->mem[a] | 0x80 | (g->mem[0xff45] == g->mem[0xff44] ? 4 : 0) |
                     g->ppu_mode);
  if (a >= 0xa000 && a < 0xc000 && g->ram && g->ram_enable) {
    if (g->mbc == 3 && g->rtc_select >= 8 && g->rtc_select <= 12)
      return g->rtc_latched[g->rtc_select - 8];
    return g->ram[((size_t)(g->mbc == 1 && g->mode ? g->ram_bank :
                             g->mbc == 5 ? g->ram_bank : 0) *
                    0x2000 + a - 0xa000) % g->ram_size];
  }
  if (a >= 0xff10 && a <= 0xff26)
    return audio_read(g, a);
  return a == 0xff00 ? jp(g) : g->mem[a];
}
static void wr(gb_t *g, uint16_t a, uint8_t v) {
  if (a >= 0xe000 && a < 0xfe00)
    a = (uint16_t)(a - 0x2000);
  if (a >= 0x8000 && a < 0xa000) {
    g->vram[a - 0x8000] = v;
    return;
  }
  if (a >= 0xfe00 && a < 0xff00) {
    if (a < 0xfea0)
      g->oam[a - 0xfe00] = v;
    return;
  }
  if (g->mbc && a < 0x2000) {
    g->ram_enable = (v & 15) == 10;
    return;
  }
  if (g->mbc == 1 && a < 0x4000) {
    g->rom_bank = v & 31;
    if (!g->rom_bank)
      g->rom_bank = 1;
    g->rom_bank |= g->upper << 5;
    return;
  }
  if (g->mbc == 3 && a < 0x4000) {
    g->rom_bank = v & 127;
    if (!g->rom_bank)
      g->rom_bank = 1;
    return;
  }
  if (g->mbc == 5 && a < 0x3000) {
    g->rom_bank = (uint16_t)((g->rom_bank & 0x100) | v);
    return;
  }
  if (g->mbc == 5 && a < 0x4000) {
    g->rom_bank = (uint16_t)((g->rom_bank & 0xff) | ((v & 1) << 8));
    return;
  }
  if (g->mbc == 3 && a < 0x6000) {
    g->ram_bank = v;
    g->rtc_select = v;
    return;
  }
  if (g->mbc == 5 && a < 0x6000) {
    g->ram_bank = v & 15;
    return;
  }
  if (g->mbc == 1 && a < 0x6000) {
    if (g->mode)
      g->ram_bank = v & 3;
    else {
      g->upper = v & 3;
      g->rom_bank = (g->rom_bank & 31) | (g->upper << 5);
    }
    return;
  }
  if (g->mbc == 3 && a < 0x8000) {
    if (v == 1 && g->rtc_latched_valid == 0)
      memcpy(g->rtc_latched, g->rtc, sizeof g->rtc);
    g->rtc_latched_valid = v == 1;
    return;
  }
  if (g->mbc == 1 && a < 0x8000) {
    g->mode = v & 1;
    return;
  }
  if (a >= 0xa000 && a < 0xc000 && g->ram && g->ram_enable) {
    if (g->mbc == 3 && g->rtc_select >= 8 && g->rtc_select <= 12) {
      g->rtc[g->rtc_select - 8] = v;
      return;
    }
    g->ram[((size_t)(g->mbc == 1 && g->mode ? g->ram_bank :
                     g->mbc == 5 ? g->ram_bank : 0) *
             0x2000 + a - 0xa000) % g->ram_size] = v;
    return;
  }
  if (a == 0xff00) {
    g->mem[a] = (uint8_t)((g->mem[a] & 0xcf) | (v & 0x30));
    return;
  }
  if (a == 0xff26) {
    g->mem[a] = (uint8_t)(v & 0x80);
    if (!(v & 0x80)) {
      memset(g->mem + 0xff10, 0, 0x17);
      g->audio_enabled[0] = g->audio_enabled[1] = 0;
    }
    return;
  }
  if (a >= 0xff10 && a <= 0xff25) {
    g->mem[a] = v;
    if ((a == 0xff14 || a == 0xff19 || a == 0xff1e || a == 0xff23) &&
        (v & 0x80))
      audio_trigger(g, a == 0xff14 ? 0 : a == 0xff19 ? 1 : a == 0xff1e ? 2 : 3);
    return;
  }
  if (a >= 0xff30 && a <= 0xff3f) {
    g->mem[a] = v;
    return;
  }
  if (a == 0xff04) {
    unsigned old_signal = g->timer_signal;
    g->mem[a] = 0;
    g->divider = 0;
    g->div = 0;
    g->timer = 0;
    g->timer_signal = 0;
    if (old_signal && !timer_level(g)) {
      if (++g->mem[0xff05] == 0) {
        g->mem[0xff05] = g->mem[0xff06];
        g->mem[0xff0f] |= 4;
      }
    }
    return;
  }
  if (a == 0xff41) {
    g->mem[a] = (uint8_t)((g->mem[a] & 7) | (v & 0x78));
    ppu_stat(g);
    return;
  }
  if (a == 0xff44)
    return;
  if (a == 0xff46) {
    g->mem[a] = v;
    g->dma_page = v;
    g->dma_index = 0;
    g->dma_cycles = 0;
    g->dma_active = 1;
    return;
  }
  if (a == 0xff40) {
    uint8_t old = g->mem[a];
    g->mem[a] = v;
    if (!(v & 0x80)) {
      g->ppu_mode = 0;
      g->ppu_cycles = 0;
      g->mem[0xff44] = 0;
    } else if (!(old & 0x80)) {
      g->ppu_mode = 2;
      g->ppu_cycles = 0;
      g->mem[0xff44] = 0;
    }
    return;
  }
  if (a == 0xff45) {
    g->mem[a] = v;
    ppu_stat(g);
    return;
  }
  if (a == 0xff07) {
    unsigned old_signal = g->timer_signal;
    g->mem[a] = (uint8_t)(v & 7);
    unsigned new_signal = timer_level(g);
    g->timer_signal = (uint8_t)new_signal;
    if (old_signal && !new_signal) {
      if (++g->mem[0xff05] == 0) {
        g->mem[0xff05] = g->mem[0xff06];
        g->mem[0xff0f] |= 4;
      }
    }
    return;
  }
  if (a == 0xff02 && (v & 0x80) && g->serial) {
    uint8_t in = 0xff;
    g->serial(g->serial_user, g->mem[0xff01], &in);
    g->mem[0xff01] = in;
    g->mem[0xff0f] |= 8;
    v &= 0x7f;
  }
  g->mem[a] = v;
}
static uint8_t inc(gb_t *g, uint8_t v) {
  uint8_t r = v + 1;
  fs(g, r == 0, 0, (v & 15) == 15, f(g, 4));
  return r;
}
static uint8_t dec(gb_t *g, uint8_t v) {
  uint8_t r = v - 1;
  fs(g, r == 0, 1, (v & 15) == 0, f(g, 4));
  return r;
}
static void alu(gb_t *g, unsigned n, uint8_t v) {
  uint8_t a = hi(g->af), c = f(g, 4), r = 0;
  unsigned h = 0, ca = 0;
  uint16_t x;
  if (n < 4) {
    if (n < 2) {
      x = a + v + (n == 2 ? c : 0);
      r = (uint8_t)x;
      h = ((a & 15) + (v & 15) + (n == 2 ? c : 0)) > 15;
      ca = x > 255;
    } else {
      x = (uint16_t)a - v - (n == 3 ? c : 0);
      r = (uint8_t)x;
      h = (a & 15) < ((v & 15) + (n == 3 ? c : 0));
      ca = x > 255;
    }
    fs(g, r == 0, n >= 2, h, ca);
  } else {
    r = n == 4 ? a & v : n == 5 ? a ^ v : a | v;
    fs(g, r == 0, 0, n == 4, 0);
  }
  if (n != 7)
    g->af = pr(r, lo(g->af));
}
static void daa(gb_t *g) {
  uint8_t a = hi(g->af), q = 0, c = f(g, 4);
  if (!f(g, 6)) {
    if (c || a > 0x99)
      q = 0x60, c = 1;
    if (f(g, 5) || (a & 15) > 9)
      q |= 6;
    a += q;
  } else {
    if (c)
      q |= 0x60;
    if (f(g, 5))
      q |= 6;
    a -= q;
  }
  g->af = pr(a, lo(g->af));
  fs(g, a == 0, f(g, 6), 0, c);
}
static int cb(gb_t *g, uint8_t o) {
  unsigned n = o & 7, b = o >> 3 & 7, k = o >> 6;
  uint8_t v = gr(g, n), r;
  if (k == 1) {
    fs(g, !(v & (1u << b)), 0, 1, f(g, 4));
    return n == 6 ? 12 : 8;
  }
  if (k == 2) {
    sr(g, n, v & ~(1u << b));
    return n == 6 ? 16 : 8;
  }
  if (k == 3) {
    sr(g, n, v | (1u << b));
    return n == 6 ? 16 : 8;
  }
  switch (b) {
  case 0:
    r = (uint8_t)((v << 1) | (v >> 7));
    fs(g, r == 0, 0, 0, v >> 7);
    break;
  case 1:
    r = (uint8_t)((v >> 1) | (v << 7));
    fs(g, r == 0, 0, 0, v & 1);
    break;
  case 2:
    r = (uint8_t)((v << 1) | f(g, 4));
    fs(g, r == 0, 0, 0, v >> 7);
    break;
  case 3:
    r = (uint8_t)((v >> 1) | (f(g, 4) << 7));
    fs(g, r == 0, 0, 0, v & 1);
    break;
  case 4:
    r = (uint8_t)(v << 1);
    fs(g, r == 0, 0, 0, v >> 7);
    break;
  case 5:
    r = (uint8_t)((v >> 1) | (v & 128));
    fs(g, r == 0, 0, 0, v & 1);
    break;
  case 6:
    r = (uint8_t)((v << 4) | (v >> 4));
    fs(g, r == 0, 0, 0, 0);
    break;
  default:
    r = v >> 1;
    fs(g, r == 0, 0, 0, v & 1);
    break;
  }
  sr(g, n, r);
  return n == 6 ? 16 : 8;
}
static uint8_t tile_pixel(const gb_t *g, int tile, unsigned row, unsigned col) {
  size_t a = (size_t)((tile & 255) * 16 + row * 2);
  uint8_t lo = g->vram[a & 0x1fff], hi = g->vram[(a + 1) & 0x1fff];
  return (uint8_t)(((hi >> (7 - col)) & 1) * 2 + ((lo >> (7 - col)) & 1));
}
static void ppu_line(gb_t *g, unsigned y) {
  static const uint32_t color[] = {0xfff8f8f8, 0xffa8a8a8, 0xff585858, 0xff101010};
  uint8_t lcdc = g->mem[0xff40], scx = g->mem[0xff43], scy = g->mem[0xff42];
  uint8_t wy = g->mem[0xff4a], wx = g->mem[0xff4b];
  unsigned window = (lcdc & 0x20) && y >= wy;
  for (unsigned x = 0; x < 160; x++) {
    unsigned px = x + scx, py = y + scy;
    if (window && x + 7 >= wx) {
      px = x + 7 - wx;
      py = y - wy;
    }
    unsigned map = window ? ((lcdc & 0x40) ? 0x1c00 : 0x1800)
                          : ((lcdc & 8) ? 0x1c00 : 0x1800);
    unsigned tx = (px >> 3) & 31, ty = (py >> 3) & 31;
    uint8_t t = g->vram[map + ty * 32 + tx], pal = g->mem[0xff47];
    int tile = (lcdc & 0x10) ? t : (int8_t)t + 256;
    uint8_t p = (lcdc & 1) ? tile_pixel(g, tile, py & 7, px & 7) : 0;
    g->bg_line[x] = p;
    g->fb[y * 160 + x] = color[(pal >> (p * 2)) & 3];
  }
  if ((lcdc & 2) && (lcdc & 0x80)) {
    unsigned height = (lcdc & 4) ? 16 : 8, drawn = 0;
    for (unsigned i = 0; i < 40 && drawn < 10; i++) {
      uint8_t sy = g->oam[i * 4], sx = g->oam[i * 4 + 1], t = g->oam[i * 4 + 2], a = g->oam[i * 4 + 3];
      int line = (int)y - sy + 16;
      if (line < 0 || line >= (int)height) continue;
      drawn++;
      if (a & 0x40) line = (int)height - 1 - line;
      if (height == 16) t &= 0xfe;
      for (unsigned col = 0; col < 8; col++) {
        unsigned tile_col = a & 0x20 ? 7 - col : col;
        int xx = (int)sx - 8 + (int)col;
        if (xx < 0 || xx >= 160) continue;
        uint8_t p = tile_pixel(g, t + (line >= 8), (unsigned)line & 7, tile_col);
        if (!p || ((a & 0x80) && g->bg_line[xx])) continue;
        uint8_t pal = a & 0x10 ? g->mem[0xff49] : g->mem[0xff48];
        g->fb[y * 160 + xx] = color[(pal >> (p * 2)) & 3];
      }
    }
  }
}
static void ppu_stat(gb_t *g) {
  uint8_t lyc = g->mem[0xff45], stat = g->mem[0xff41];
  unsigned signal = ((g->ppu_mode == 0) && (stat & 8)) ||
                    ((g->ppu_mode == 1) && (stat & 16)) ||
                    ((g->ppu_mode == 2) && (stat & 32)) ||
                    (lyc == g->mem[0xff44] && (stat & 64));
  if (signal && !g->stat_signal) g->mem[0xff0f] |= 2;
  g->stat_signal = (uint8_t)signal;
}
static void ppu_tick(gb_t *g) {
  if (!(g->mem[0xff40] & 0x80)) return;
  if (++g->ppu_cycles < (g->ppu_mode == 2 ? 80 : g->ppu_mode == 3 ? 172 :
                         g->ppu_mode == 1 ? 456 : 204)) return;
  g->ppu_cycles = 0;
  if (g->ppu_mode == 2) g->ppu_mode = 3;
  else if (g->ppu_mode == 3) {
    ppu_line(g, g->mem[0xff44]);
    g->ppu_mode = 0;
  } else if (g->ppu_mode == 0) {
    g->mem[0xff44]++;
    if (g->mem[0xff44] == 144) {
      g->ppu_mode = 1;
      g->mem[0xff0f] |= 1;
    } else g->ppu_mode = 2;
  } else {
    g->mem[0xff44]++;
    if (g->mem[0xff44] >= 154) {
      g->mem[0xff44] = 0;
      g->ppu_mode = 2;
    }
  }
  ppu_stat(g);
}
static void tick(gb_t *g, unsigned n) {
  while (n--) {
    unsigned old = timer_level(g);
    g->divider++;
    g->div = (uint8_t)(g->divider >> 8);
    g->mem[0xff04] = g->div;
    unsigned now = timer_level(g);
    g->timer_signal = (uint8_t)now;
    if (old && !now) {
      if (++g->mem[0xff05] == 0) {
        g->mem[0xff05] = g->mem[0xff06];
        g->mem[0xff0f] |= 4;
      }
    }
    ppu_tick(g);
    if (g->dma_active && ++g->dma_cycles == 4) {
      g->dma_cycles = 0;
      g->oam[g->dma_index] = rd(g, (uint16_t)(g->dma_page * 0x100 + g->dma_index));
      if (++g->dma_index == sizeof g->oam)
        g->dma_active = 0;
    }
  }
}
static int irq(gb_t *g) {
  uint8_t p = g->mem[0xffff] & g->mem[0xff0f] & 31;
  if (!p)
    return 0;
  g->halted = 0;
  if (!g->ime)
    return 0;
  g->ime = 0;
  {
    unsigned n = 0;
    while (!(p & (1u << n)))
      n++;
    g->mem[0xff0f] &= (uint8_t)~(1u << n);
    push(g, g->pc);
    g->pc = 0x40 + n * 8;
  }
  return 20;
}
int gb_dbg_step(gb_t *g) {
  int q = irq(g);
  if (q) {
    tick(g, q);
    return q;
  }
  if (g->dma_active) {
    tick(g, 4);
    return 4;
  }
  if (g->halted) {
    tick(g, 4);
    return 4;
  }
  uint8_t o = fc(g), v;
  unsigned x = o >> 6, y = o >> 3 & 7, z = o & 7;
  uint16_t n;
  int c = 4;
  if (o == 0xcb) {
    c = cb(g, fc(g));
    goto done;
  }
  if (x == 1) {
    if (o == 0x76) {
      g->halted = 1;
      if (!g->ime && (g->mem[0xffff] & g->mem[0xff0f] & 31))
        g->halt_bug = 1;
      c = 4;
    } else {
      sr(g, y, gr(g, z));
      c = (y == 6 || z == 6) ? 8 : 4;
    }
    goto done;
  }
  if (x == 2) {
    alu(g, y, gr(g, z));
    c = z == 6 ? 8 : 4;
    goto done;
  }
  if (x == 3 && z == 6 && o >= 0xc6) {
    alu(g, y, fc(g));
    c = 8;
    goto done;
  }
  switch (o) {
  case 0:
    break;
  case 2:
    wr(g, g->bc, hi(g->af));
    break;
  case 0xa:
    g->af = pr(rd(g, g->bc), lo(g->af));
    break;
  case 0x12:
    wr(g, g->de, hi(g->af));
    break;
  case 0x1a:
    g->af = pr(rd(g, g->de), lo(g->af));
    break;
  case 8:
    n = fn(g);
    wr(g, n, lo(g->sp));
    wr(g, n + 1, hi(g->sp));
    c = 20;
    break;
  case 0x22:
    wr(g, g->hl++, hi(g->af));
    break;
  case 0x2a:
    g->af = pr(rd(g, g->hl++), lo(g->af));
    break;
  case 0x32:
    wr(g, g->hl--, hi(g->af));
    break;
  case 0x3a:
    g->af = pr(rd(g, g->hl--), lo(g->af));
    break;
  case 0x27:
    daa(g);
    break;
  case 0x2f:
    g->af = pr(~hi(g->af), lo(g->af));
    fs(g, f(g, 7), 1, 1, f(g, 4));
    break;
  case 0x37:
    fs(g, f(g, 7), 0, 0, 1);
    break;
  case 0x3f:
    fs(g, f(g, 7), 0, 0, !f(g, 4));
    break;
  case 7:
    v = hi(g->af);
    g->af = pr(v << 1 | v >> 7, lo(g->af));
    fs(g, 0, 0, 0, v >> 7);
    break;
  case 0x0f:
    v = hi(g->af);
    g->af = pr((uint8_t)((v >> 1) | (v << 7)), lo(g->af));
    fs(g, 0, 0, 0, v & 1);
    break;
  case 0x17:
    v = hi(g->af);
    g->af = pr((uint8_t)((v << 1) | f(g, 4)), lo(g->af));
    fs(g, 0, 0, 0, v >> 7);
    break;
  case 0x1f:
    v = hi(g->af);
    g->af = pr((uint8_t)((v >> 1) | (f(g, 4) << 7)), lo(g->af));
    fs(g, 0, 0, 0, v & 1);
    break;
  case 0xf3:
    g->ime = 0;
    g->ei_delay = 0;
    break;
  case 0xfb:
    g->ei_delay = 2;
    break;
  case 0xc3:
    g->pc = fn(g);
    c = 16;
    break;
  case 0x18:
    g->pc += ((int8_t)fc(g));
    c = 12;
    break;
  case 0x20:
  case 0x28:
  case 0x30:
  case 0x38: {
    int take = cond(g, (o - 0x20) / 8);
    int8_t offset = (int8_t)fc(g);
    if (take)
      g->pc += offset;
    c = take ? 12 : 8;
    break;
  }
  case 0x10:
    fc(g);
    g->halted = 1;
    break;
  case 0xcd:
    n = fn(g);
    push(g, g->pc);
    g->pc = n;
    c = 24;
    break;
  case 0xc9:
    g->pc = pop(g);
    c = 16;
    break;
  case 0xc0:
  case 0xc8:
  case 0xd0:
  case 0xd8:
    if (cond(g, (o - 0xc0) / 8)) {
      g->pc = pop(g);
      c = 20;
    } else {
      c = 8;
    }
    break;
  case 0xc1:
  case 0xd1:
  case 0xe1:
  case 0xf1:
    pop_pair(g, (o - 0xc1) / 16);
    c = 12;
    break;
  case 0xc5:
  case 0xd5:
  case 0xe5:
  case 0xf5:
    push(g, o == 0xc5 ? g->bc : o == 0xd5 ? g->de : o == 0xe5 ? g->hl : g->af);
    c = 16;
    break;
  case 0xc4:
  case 0xcc:
  case 0xd4:
  case 0xdc:
    n = fn(g);
    if (cond(g, (o - 0xc4) / 8)) {
      push(g, g->pc);
      g->pc = n;
      c = 24;
    } else {
      c = 12;
    }
    break;
  case 0xe2:
    wr(g, (uint16_t)(0xff00 + lo(g->bc)), hi(g->af));
    c = 8;
    break;
  case 0xf2:
    g->af = pr(rd(g, (uint16_t)(0xff00 + lo(g->bc))), lo(g->af));
    c = 8;
    break;
  case 0xe8:
    g->sp = addsp(g, (int8_t)fc(g));
    c = 16;
    break;
  case 0xf8:
    g->hl = addsp(g, (int8_t)fc(g));
    c = 12;
    break;
  case 0xd9:
    g->pc = pop(g);
    g->ime = 1;
    c = 16;
    break;
  case 0xe0:
    wr(g, 0xff00 + fc(g), hi(g->af));
    c = 12;
    break;
  case 0xf0:
    g->af = pr(rd(g, 0xff00 + fc(g)), lo(g->af));
    c = 12;
    break;
  case 0xea:
    wr(g, fn(g), hi(g->af));
    c = 16;
    break;
  case 0xfa:
    g->af = pr(rd(g, fn(g)), lo(g->af));
    c = 16;
    break;
  case 0xf9:
    g->sp = g->hl;
    c = 8;
    break;
  case 0xe9:
    g->pc = g->hl;
    break;
  default:
    if (x == 0 && z == 1 && !(y & 1)) {
      *rp16(g, y >> 1) = fn(g);
      c = 12;
    } else if (x == 0 && z == 1) {
      addhl(g, *rp16(g, y >> 1));
      c = 8;
    } else if (x == 0 && z == 4) {
      sr(g, y, inc(g, gr(g, y)));
      c = y == 6 ? 12 : 4;
    } else if (x == 0 && z == 5) {
      sr(g, y, dec(g, gr(g, y)));
      c = y == 6 ? 12 : 4;
    } else if (x == 0 && z == 6) {
      sr(g, y, fc(g));
      c = y == 6 ? 12 : 8;
    } else if (x == 0 && z == 3 && !(y & 1)) {
      (*rp16(g, y >> 1))++;
      c = 8;
    } else if (x == 0 && z == 3 && (y & 1)) {
      (*rp16(g, y >> 1))--;
      c = 8;
    } else if (x == 3 && z == 2 && y < 4) {
      if (cond(g, y))
        g->pc = fn(g);
      else
        g->pc += 2;
      c = cond(g, y) ? 16 : 12;
    } else if (x == 3 && z == 7) {
      push(g, g->pc);
      g->pc = y * 8;
      c = 16;
    }
    break;
  }
done:
  if (g->ei_delay && !--g->ei_delay)
    g->ime = 1;
  tick(g, (unsigned)c);
  return c;
}
void gb_run_frame(gb_t *g) {
  unsigned n = 0;
  while (n < 70224)
    n += (unsigned)gb_dbg_step(g);
  audio_frame(g);
}
gb_t *gb_create(void) {
  gb_t *g = calloc(1, sizeof(*g));
  if (g) {
    g->rom_bank = 1;
    g->mem[0xff40] = 0x91;
    g->mem[0xff47] = 0xe4;
    g->mem[0xff00] = 0xcf;
  }
  return g;
}
void gb_destroy(gb_t *g) {
  if (g) {
    free(g->rom);
    free(g->ram);
    free(g);
  }
}
int gb_load_rom(gb_t *g, const uint8_t *r, size_t n) {
  if (!g || !r || n < 0x150)
    return -1;
  free(g->rom);
  free(g->ram);
  g->rom = malloc(n);
  if (!g->rom)
    return -1;
  memcpy(g->rom, r, n);
  g->rom_size = n;
  g->mbc = r[0x147] == 1 || r[0x147] == 2 || r[0x147] == 3
               ? 1
               : r[0x147] >= 0x0f && r[0x147] <= 0x13 ? 3
               : r[0x147] >= 0x19 && r[0x147] <= 0x1e ? 5
                                                       : 0;
  g->battery = (uint8_t)has_battery(r[0x147]);
  g->ram_size = ramsize(r[0x149]);
  g->ram = g->ram_size ? calloc(1, g->ram_size) : NULL;
  if (g->ram_size && !g->ram)
    return -1;
  gb_reset(g);
  return 0;
}
size_t gb_save_ram_size(const gb_t *g) {
  return g && g->battery && g->ram ? g->ram_size : 0;
}
size_t gb_save_ram(const gb_t *g, uint8_t *out) {
  size_t n = gb_save_ram_size(g);
  if (n && out)
    memcpy(out, g->ram, n);
  return n;
}
int gb_load_ram(gb_t *g, const uint8_t *data, size_t n) {
  size_t expected = gb_save_ram_size(g);
  if (!data || n != expected)
    return -1;
  memcpy(g->ram, data, n);
  return 0;
}
size_t gb_save_state_size(const gb_t *g) {
  return g ? STATE_HEADER_SIZE + 0x10000u + 0x2000u + 0xa0u +
                    sizeof g->fb + sizeof g->bg_line + 82u + g->ram_size
           : 0;
}
size_t gb_save_state(const gb_t *g, uint8_t *out) {
  if (!g || !out)
    return 0;
  uint8_t *p = out;
  memcpy(p, "GBSTATE1", 8);
  p += 8;
  put32(&p, STATE_VERSION);
  put32(&p, (uint32_t)g->rom_size);
  put32(&p, (uint32_t)g->ram_size);
  put32(&p, (uint32_t)(gb_save_state_size(g) - STATE_HEADER_SIZE));
  memcpy(p, g->mem, sizeof g->mem);
  p += sizeof g->mem;
  memcpy(p, g->vram, sizeof g->vram);
  p += sizeof g->vram;
  memcpy(p, g->oam, sizeof g->oam);
  p += sizeof g->oam;
  memcpy(p, g->fb, sizeof g->fb);
  p += sizeof g->fb;
  memcpy(p, g->bg_line, sizeof g->bg_line);
  p += sizeof g->bg_line;
  put16(&p, g->af);
  put16(&p, g->bc);
  put16(&p, g->de);
  put16(&p, g->hl);
  put16(&p, g->sp);
  put16(&p, g->pc);
  *p++ = g->ime;
  *p++ = g->ei_delay;
  *p++ = g->halted;
  *p++ = g->halt_bug;
  *p++ = g->input;
  *p++ = g->div;
  *p++ = g->mbc;
  *p++ = g->battery;
  put16(&p, g->rom_bank);
  *p++ = g->ram_bank;
  *p++ = g->ram_enable;
  *p++ = g->upper;
  *p++ = g->mode;
  *p++ = g->ppu_mode;
  *p++ = g->stat_signal;
  *p++ = g->dma_page;
  *p++ = g->dma_index;
  *p++ = g->dma_active;
  put16(&p, g->timer);
  put32(&p, g->ppu_cycles);
  put32(&p, g->dma_cycles);
  put16(&p, g->divider);
  *p++ = g->timer_signal;
  *p++ = g->rtc_select;
  *p++ = g->rtc_latched_valid;
  memcpy(p, g->rtc, sizeof g->rtc);
  p += sizeof g->rtc;
  memcpy(p, g->rtc_latched, sizeof g->rtc_latched);
  p += sizeof g->rtc_latched;
  put32(&p, (uint32_t)g->model);
  put16(&p, g->noise_lfsr);
  for (unsigned i = 0; i < 4; i++) {
    put32(&p, g->audio_phase[i]);
    *p++ = g->audio_enabled[i];
  }
  if (g->ram_size)
    memcpy(p, g->ram, g->ram_size);
  return gb_save_state_size(g);
}
int gb_load_state(gb_t *g, const uint8_t *data, size_t n) {
  if (!g || !data || n != gb_save_state_size(g) || n < STATE_HEADER_SIZE ||
      memcmp(data, "GBSTATE1", 8) != 0)
    return -1;
  const uint8_t *p = data + 8;
  if (get32(&p) != STATE_VERSION || get32(&p) != g->rom_size ||
      get32(&p) != g->ram_size || get32(&p) != n - STATE_HEADER_SIZE)
    return -1;
  memcpy(g->mem, p, sizeof g->mem);
  p += sizeof g->mem;
  memcpy(g->vram, p, sizeof g->vram);
  p += sizeof g->vram;
  memcpy(g->oam, p, sizeof g->oam);
  p += sizeof g->oam;
  memcpy(g->fb, p, sizeof g->fb);
  p += sizeof g->fb;
  memcpy(g->bg_line, p, sizeof g->bg_line);
  p += sizeof g->bg_line;
  g->af = get16(&p);
  g->bc = get16(&p);
  g->de = get16(&p);
  g->hl = get16(&p);
  g->sp = get16(&p);
  g->pc = get16(&p);
  g->ime = *p++;
  g->ei_delay = *p++;
  g->halted = *p++;
  g->halt_bug = *p++;
  g->input = *p++;
  g->div = *p++;
  g->mbc = *p++;
  g->battery = *p++;
  g->rom_bank = *p++;
  g->ram_bank = *p++;
  g->ram_enable = *p++;
  g->upper = *p++;
  g->mode = *p++;
  g->ppu_mode = *p++;
  g->stat_signal = *p++;
  g->dma_page = *p++;
  g->dma_index = *p++;
  g->dma_active = *p++;
  g->timer = get16(&p);
  g->ppu_cycles = get32(&p);
  g->dma_cycles = get32(&p);
  g->divider = get16(&p);
  g->timer_signal = *p++;
  g->rtc_select = *p++;
  g->rtc_latched_valid = *p++;
  memcpy(g->rtc, p, sizeof g->rtc);
  p += sizeof g->rtc;
  memcpy(g->rtc_latched, p, sizeof g->rtc_latched);
  p += sizeof g->rtc_latched;
  g->model = (gb_model_t)get32(&p);
  g->noise_lfsr = get16(&p);
  for (unsigned i = 0; i < 4; i++) {
    g->audio_phase[i] = get32(&p);
    g->audio_enabled[i] = *p++;
  }
  if (g->ram_size)
    memcpy(g->ram, p, g->ram_size);
  return 0;
}
void gb_set_model(gb_t *g, gb_model_t m) { g->model = m; }
void gb_reset(gb_t *g) {
  g->af = 0x1b0;
  g->bc = 0x13;
  g->de = 0xd8;
  g->hl = 0x14d;
  g->sp = 0xfffe;
  g->pc = 0x100;
  g->ime = g->halted = g->halt_bug = g->ei_delay = 0;
  g->rom_bank = 1;
  g->ram_bank = g->upper = g->mode = 0;
  g->div = 0;
  g->divider = g->timer = 0;
  g->timer_signal = 0;
  g->ppu_cycles = 0;
  g->ppu_mode = 2;
  g->stat_signal = 0;
  g->dma_page = g->dma_index = g->dma_active = 0;
  g->dma_cycles = 0;
  g->mem[0xff04] = 0;
  g->mem[0xff05] = 0;
  g->mem[0xff06] = 0;
  g->mem[0xff07] = 0;
  g->mem[0xff0f] = 0;
  g->mem[0xffff] = 0;
  g->mem[0xff40] = 0x91;
  g->mem[0xff47] = 0xe4;
  g->mem[0xff00] = 0xcf;
  g->mem[0xff44] = 0;
  memset(g->mem + 0xff10, 0, 0x17);
  g->mem[0xff26] = 0;
  memset(g->audio_phase, 0, sizeof g->audio_phase);
  g->noise_lfsr = 0x7fff;
  memset(g->audio_enabled, 0, sizeof g->audio_enabled);
}
const uint32_t *gb_framebuffer(const gb_t *g) { return g->fb; }
void gb_set_input(gb_t *g, uint8_t v) { g->input = v; }
void gb_set_audio_callback(gb_t *g, gb_audio_cb c, void *u) {
  g->audio = c;
  g->audio_user = u;
}
void gb_set_serial_callback(gb_t *g, gb_serial_cb c, void *u) {
  g->serial = c;
  g->serial_user = u;
}
uint8_t gb_dbg_read(const gb_t *g, uint16_t a) { return rd(g, a); }
void gb_dbg_write(gb_t *g, uint16_t a, uint8_t v) { wr(g, a, v); }
void gb_dbg_regs(const gb_t *g, gb_regs_t *o) {
  o->af = g->af;
  o->bc = g->bc;
  o->de = g->de;
  o->hl = g->hl;
  o->sp = g->sp;
  o->pc = g->pc;
  o->ime = g->ime;
  o->halted = g->halted;
}
